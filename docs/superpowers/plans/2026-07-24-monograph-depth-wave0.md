# Monograph Depth — Wave 0 (Infrastructure + Exemplar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the narrative-monograph infrastructure (build-verified code citations, a narrative body renderer that drops filler globally) and convert `materials/pbr-model` into the reference exemplar that later content waves are held against.

**Architecture:** A single build-time renderer replaces the fixed seven-section template. A new `cite.py` resolves `{{cite path "substring"}}` markers against the real repo tree and fails the build on stale/ambiguous anchors. A new `blocks.py` parses an authored markdown body into ordered blocks (heading, prose, math, citation, callout, figure, code) and renders each. `render.py` assembles a thin frame (thesis, auto on-this-page, sources footer) around that body and filters legacy filler so all 113 pages get cleaner immediately, while pages marked `standard: v2` come under the stricter math-explanation gate.

**Tech Stack:** Python 3.14 (stdlib only — `re`, `pathlib`, `html`), unittest, KaTeX (already loaded in the shell), static HTML/CSS.

## Global Constraints

- Python stdlib only. No new pip dependencies. (matches existing `tools/monograph`)
- Build command is `python site/tools/generate_tree.py` (run from repo root); it must exit non-zero on any unresolved/ambiguous citation.
- Repo root is `paths.ROOT` (`Path(__file__).resolve().parents[3]` from `paths.py`). All citation paths are relative to it.
- GitHub link base (verified): `https://github.com/Qervas/ohao_engine/blob/master`.
- Never render the literal string `Sources of truth:` or the heading `API / symbols the unit exposes or binds` into any page.
- Preserve existing CSS variables and classes; CSS changes are additive only.
- All existing tests in `site/tests/test_monograph_structure.py` must stay green.
- Citation substrings are matched with `str.__contains__` (exact, case-sensitive, whitespace-significant). Author extends a substring to disambiguate; never hand-type line numbers.

---

## File Structure

**Create:**
- `site/tools/monograph/cite.py` — citation resolver + chip HTML. One responsibility: turn `(path, substring)` into a verified `(line, text)` and a chip, or raise.
- `site/tools/monograph/blocks.py` — authored-body grammar: frontmatter split, block parser, per-block HTML renderers, legacy-filler filter.
- `site/tests/test_cite.py` — unit tests for `cite.py`.
- `site/tests/test_blocks.py` — unit tests for `blocks.py`.
- `site/assets/units/ggx_multiscatter_energy.svg` — the one exemplar figure.

**Modify:**
- `site/tools/monograph/render.py` — replace `render_leaf` internals with the narrative renderer; drop `dual_box_svg`/`probe` api+snippet usage; build on-this-page + sources footer.
- `site/tools/monograph/diagrams.py` — delete `dual_box_svg` (dead after render.py change).
- `site/tools/monograph/content_loader.py` — no change to logic; `merge_content` stays for any not-yet-migrated caller, but `render_leaf` no longer calls it (see Task 3).
- `site/styles.css` — append `.citation-chip`, `.key-idea`, `figure.unit-figure` rules.
- `site/content/units/materials/pbr-model.md` — rewrite as the `standard: v2` exemplar.
- `site/tests/test_monograph_structure.py` — add the five gate tests.

---

## Task 1: Citation resolver (`cite.py`)

**Files:**
- Create: `site/tools/monograph/cite.py`
- Test: `site/tests/test_cite.py`

**Interfaces:**
- Consumes: `paths.ROOT` (existing `Path`).
- Produces:
  - `class CitationError(Exception)`
  - `resolve_citation(path: str, substring: str) -> tuple[int, str]` — 1-based line number and the raw text of the unique line in `ROOT/path` containing `substring`. Raises `CitationError` on 0 or >1 matches, or missing file.
  - `citation_chip_html(path: str, substring: str) -> str` — resolves then returns the chip markup.
  - `GITHUB_BLOB_BASE: str = "https://github.com/Qervas/ohao_engine/blob/master"`

- [ ] **Step 1: Write the failing tests**

Create `site/tools/monograph/__init__.py` is already present; tests import via the package path used by the existing suite. Create `site/tests/test_cite.py`:

```python
import sys
import unittest
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SITE / "tools"))

from monograph import cite  # noqa: E402
from monograph.paths import ROOT  # noqa: E402


class ResolveCitationTest(unittest.TestCase):
    def test_unique_match_returns_line_and_text(self):
        line, text = cite.resolve_citation(
            "shaders/includes/brdf/brdf_ggx.glsl",
            "float Ems = (1.0 - E_o) * (1.0 - E_i);",
        )
        self.assertEqual(line, 159)
        self.assertIn("Ems", text)

    def test_missing_substring_raises(self):
        with self.assertRaises(cite.CitationError):
            cite.resolve_citation(
                "shaders/includes/brdf/brdf_ggx.glsl",
                "this text does not exist anywhere zzz",
            )

    def test_ambiguous_substring_raises_and_lists_lines(self):
        with self.assertRaises(cite.CitationError) as ctx:
            cite.resolve_citation(
                "shaders/includes/pbr_unpack.glsl",
                "if (roughness >= 10.0) roughness -= 10.0;",
            )
        msg = str(ctx.exception)
        self.assertIn("11", msg)  # first of the two matching lines listed

    def test_missing_file_raises(self):
        with self.assertRaises(cite.CitationError):
            cite.resolve_citation("no/such/file.glsl", "anything")

    def test_chip_contains_path_line_and_github_link(self):
        html = cite.citation_chip_html(
            "shaders/includes/material/material_types.glsl",
            "return mix(dielectricF0, surface.albedo, surface.metallic);",
        )
        self.assertIn("material_types.glsl:138", html)
        self.assertIn(cite.GITHUB_BLOB_BASE, html)
        self.assertIn("#L138", html)
        self.assertIn("citation-chip", html)


if __name__ == "__main__":
    unittest.main(verbosity=2)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tests/test_cite.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'monograph.cite'`.

- [ ] **Step 3: Write `cite.py`**

```python
"""Resolve {{cite path "substring"}} markers against the real repo tree.

A citation is content-anchored: the author names a unique code substring, and
the build resolves it to the current line number. Missing or ambiguous anchors
fail the build, so no citation on the site can silently go stale.
"""
from __future__ import annotations

import html

from .paths import ROOT

GITHUB_BLOB_BASE = "https://github.com/Qervas/ohao_engine/blob/master"


class CitationError(Exception):
    pass


def resolve_citation(path: str, substring: str) -> tuple[int, str]:
    p = ROOT / path
    if not p.is_file():
        raise CitationError(f"cite: file not found: {path!r}")
    matches: list[tuple[int, str]] = []
    for i, line in enumerate(p.read_text(errors="replace").splitlines(), start=1):
        if substring in line:
            matches.append((i, line))
    if not matches:
        raise CitationError(
            f"cite: substring not found in {path!r}: {substring!r}"
        )
    if len(matches) > 1:
        lines = ", ".join(str(n) for n, _ in matches)
        raise CitationError(
            f"cite: ambiguous substring in {path!r} (lines {lines}): "
            f"{substring!r} — extend the substring to make it unique"
        )
    (line_no, text), = matches
    return line_no, text


def citation_chip_html(path: str, substring: str) -> str:
    line_no, text = resolve_citation(path, substring)
    loc = f"{path}:{line_no}"
    code = html.escape(text.strip(), quote=True)
    if GITHUB_BLOB_BASE:
        href = f"{GITHUB_BLOB_BASE}/{path}#L{line_no}"
        loc_html = (
            f'<a class="citation-loc" href="{href}" '
            f'target="_blank" rel="noopener">{html.escape(loc)}</a>'
        )
    else:
        loc_html = f'<span class="citation-loc">{html.escape(loc)}</span>'
    return (
        f'<div class="citation-chip">'
        f'<code class="citation-code">{code}</code>'
        f'{loc_html}'
        f'</div>'
    )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tests/test_cite.py`
Expected: PASS (5 tests). If `test_unique_match_returns_line_and_text` reports a line other than 159, the shader moved since this plan was written — update the expected line to the value the failure prints, do not change `resolve_citation`.

- [ ] **Step 5: Commit**

```bash
git add site/tools/monograph/cite.py site/tests/test_cite.py
git commit -m "feat(monograph): content-anchored citation resolver"
```

---

## Task 2: Authored-body grammar (`blocks.py`)

**Files:**
- Create: `site/tools/monograph/blocks.py`
- Test: `site/tests/test_blocks.py`

**Interfaces:**
- Consumes: `cite.citation_chip_html` (Task 1); `html.escape`.
- Produces:
  - `split_frontmatter(raw: str) -> tuple[dict, str]` — `(frontmatter_dict, body_text)`. Frontmatter values are strings; the list value for `figures`/`files` is parsed from `[a, b]` into `list[str]`.
  - `render_body(raw: str) -> tuple[str, list[tuple[str, str]], list[str]]` — returns `(body_html, headings, cited_paths)` where `headings` is `[(slug, text), ...]` in document order and `cited_paths` is the deduped list of file paths cited on the page (for the sources footer).
  - `iter_math_sections(raw: str) -> list[tuple[str, bool]]` — for the math-explanation gate: `[(heading_slug, section_has_prose), ...]` for every section that contains at least one math block.

- [ ] **Step 1: Write the failing tests**

Create `site/tests/test_blocks.py`:

```python
import sys
import unittest
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SITE / "tools"))

from monograph import blocks  # noqa: E402


class FrontmatterTest(unittest.TestCase):
    def test_split_frontmatter_parses_scalars_and_lists(self):
        fm, body = blocks.split_frontmatter(
            "---\n"
            "module: materials\n"
            "id: pbr-model\n"
            "standard: v2\n"
            "figures: [ggx_multiscatter_energy]\n"
            "---\n"
            "\n## Heading\n\nprose\n"
        )
        self.assertEqual(fm["module"], "materials")
        self.assertEqual(fm["standard"], "v2")
        self.assertEqual(fm["figures"], ["ggx_multiscatter_energy"])
        self.assertIn("## Heading", body)


class RenderBodyTest(unittest.TestCase):
    def test_heading_becomes_section_and_appears_in_headings(self):
        html, headings, cited = blocks.render_body(
            "---\nid: x\n---\n\n## The lie artists can paint\n\nsome prose here\n"
        )
        self.assertIn('id="the-lie-artists-can-paint"', html)
        self.assertIn("<h2", html)
        self.assertIn(("the-lie-artists-can-paint", "The lie artists can paint"), headings)
        self.assertIn("some prose here", html)

    def test_cite_block_renders_chip_and_collects_path(self):
        html, headings, cited = blocks.render_body(
            '---\nid: x\n---\n\n## S\n\nlead prose\n'
            '{{cite shaders/includes/brdf/brdf_ggx.glsl "float Ems = (1.0 - E_o) * (1.0 - E_i);"}}\n'
        )
        self.assertIn("citation-chip", html)
        self.assertIn("brdf_ggx.glsl:159", html)
        self.assertIn("shaders/includes/brdf/brdf_ggx.glsl", cited)

    def test_math_block_renders_katex_delimiters(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## M\n\nprose\n\n$$F_0 = 0.04$$\n"
        )
        self.assertIn("math-block", html)
        self.assertIn("F_0 = 0.04", html)

    def test_why_and_key_callouts(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## S\n\n:::why\nbecause the alternative fireflies\n:::\n"
            "\n:::key\nthe one thing to remember\n:::\n"
        )
        self.assertIn("why-callout", html)
        self.assertIn("because the alternative fireflies", html)
        self.assertIn("key-idea", html)
        self.assertIn("the one thing to remember", html)

    def test_figure_renders_with_caption(self):
        html, _, _ = blocks.render_body(
            '---\nid: x\n---\n\n## S\n\n{{figure ggx_multiscatter_energy "Measured energy loss vs roughness."}}\n'
        )
        self.assertIn("unit-figure", html)
        self.assertIn("ggx_multiscatter_energy.svg", html)
        self.assertIn("Measured energy loss vs roughness.", html)

    def test_legacy_sources_of_truth_line_is_dropped(self):
        html, _, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Contracts\n\n- Sources of truth: shaders/foo.glsl\n- real invariant holds\n"
        )
        self.assertNotIn("Sources of truth", html)
        self.assertIn("real invariant holds", html)

    def test_empty_section_is_not_rendered(self):
        html, headings, _ = blocks.render_body(
            "---\nid: x\n---\n\n## Empty\n\n## Full\n\nhas prose\n"
        )
        self.assertNotIn("Empty", html)
        self.assertIn("Full", html)


class MathGateTest(unittest.TestCase):
    def test_math_section_with_prose_is_ok(self):
        secs = blocks.iter_math_sections(
            "---\nid: x\n---\n\n## M\n\nexplaining prose\n\n$$a=b$$\n"
        )
        self.assertEqual(secs, [("m", True)])

    def test_math_section_without_prose_is_flagged(self):
        secs = blocks.iter_math_sections(
            "---\nid: x\n---\n\n## M\n\n$$a=b$$\n"
        )
        self.assertEqual(secs, [("m", False)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tests/test_blocks.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'monograph.blocks'`.

- [ ] **Step 3: Write `blocks.py`**

```python
"""Parse and render an authored monograph body into narrative HTML.

Body grammar (block-level, one construct per logical line/region):
  ## Title                     -> movement heading (added to on-this-page)
  plain lines                  -> prose paragraph (blank line separates)
  - item                       -> bullet list
  $$ tex $$                    -> explained-math block (KaTeX)
  {{cite path "substring"}}    -> build-verified citation chip
  {{figure id "caption"}}      -> figure from assets/units/<id>.svg
  :::why ... :::               -> "why" callout
  :::key ... :::               -> "key idea" callout
  ```lang ... ```              -> illustrative code block

Legacy filler is dropped: any prose/bullet starting with "Sources of truth:",
and a "Notes" movement whose body is only a "Source map:" file list.
Sections with no rendered body are omitted entirely.
"""
from __future__ import annotations

import html
import re

from .cite import CitationError, citation_chip_html

_HEADING = re.compile(r"^##\s+(.+?)\s*$")
_CITE = re.compile(r'^\{\{cite\s+(\S+)\s+"(.+)"\}\}\s*$')
_FIGURE = re.compile(r'^\{\{figure\s+(\S+)\s+"(.*)"\}\}\s*$')
_FENCE = re.compile(r"^```(\w*)\s*$")


def _slug(text: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return s or "section"


def split_frontmatter(raw: str) -> tuple[dict, str]:
    fm: dict = {}
    body = raw
    if raw.startswith("---"):
        end = raw.find("\n---", 3)
        if end != -1:
            block = raw[3:end]
            body = raw[end + 4 :]
            for line in block.splitlines():
                if ":" not in line:
                    continue
                key, _, val = line.partition(":")
                key, val = key.strip(), val.strip()
                if val.startswith("[") and val.endswith("]"):
                    inner = val[1:-1].strip()
                    fm[key] = [x.strip() for x in inner.split(",") if x.strip()]
                else:
                    fm[key] = val
    return fm, body.lstrip("\n")


def _is_filler(line: str) -> bool:
    s = line.lstrip("-* ").strip()
    return s.startswith("Sources of truth:")


# --- block model -------------------------------------------------------------

def _parse_blocks(body: str) -> list[dict]:
    """Return a flat list of blocks. Headings delimit sections downstream."""
    blocks: list[dict] = []
    lines = body.splitlines()
    i = 0
    prose: list[str] = []
    bullets: list[str] = []

    def flush_prose():
        nonlocal prose
        if prose:
            blocks.append({"kind": "prose", "text": " ".join(prose).strip()})
            prose = []

    def flush_bullets():
        nonlocal bullets
        if bullets:
            blocks.append({"kind": "bullets", "items": list(bullets)})
            bullets = []

    while i < len(lines):
        line = lines[i]
        s = line.strip()

        m = _HEADING.match(line)
        if m:
            flush_prose(); flush_bullets()
            title = m.group(1)
            blocks.append({"kind": "heading", "text": title, "slug": _slug(title)})
            i += 1
            continue

        if s.startswith("$$"):
            flush_prose(); flush_bullets()
            # single-line $$...$$ or multi-line until closing $$
            if s.endswith("$$") and len(s) > 3:
                tex = s[2:-2].strip()
            else:
                buf = [s[2:]]
                i += 1
                while i < len(lines) and "$$" not in lines[i]:
                    buf.append(lines[i]); i += 1
                if i < len(lines):
                    buf.append(lines[i].split("$$")[0])
                tex = "\n".join(buf).strip()
            blocks.append({"kind": "math", "tex": tex})
            i += 1
            continue

        mc = _CITE.match(s)
        if mc:
            flush_prose(); flush_bullets()
            blocks.append({"kind": "cite", "path": mc.group(1), "sub": mc.group(2)})
            i += 1
            continue

        mf = _FIGURE.match(s)
        if mf:
            flush_prose(); flush_bullets()
            blocks.append({"kind": "figure", "id": mf.group(1), "caption": mf.group(2)})
            i += 1
            continue

        if s in (":::why", ":::key"):
            flush_prose(); flush_bullets()
            kind = "why" if s == ":::why" else "key"
            buf = []
            i += 1
            while i < len(lines) and lines[i].strip() != ":::":
                buf.append(lines[i]); i += 1
            blocks.append({"kind": kind, "text": " ".join(x.strip() for x in buf).strip()})
            i += 1
            continue

        mfence = _FENCE.match(line)
        if mfence:
            flush_prose(); flush_bullets()
            lang = mfence.group(1)
            buf = []
            i += 1
            while i < len(lines) and not _FENCE.match(lines[i]):
                buf.append(lines[i]); i += 1
            blocks.append({"kind": "code", "lang": lang, "text": "\n".join(buf)})
            i += 1
            continue

        if not s:
            flush_prose()
            i += 1
            continue

        if _is_filler(line):
            i += 1
            continue

        if s.startswith(("- ", "* ")):
            flush_prose()
            bullets.append(s[2:].strip())
            i += 1
            continue

        flush_bullets()
        prose.append(s)
        i += 1

    flush_prose(); flush_bullets()
    return blocks


# --- section grouping + rendering -------------------------------------------

def _group_sections(blocks: list[dict]) -> list[dict]:
    """Group blocks under their heading. A pre-heading preamble gets slug ''."""
    sections: list[dict] = []
    current = {"slug": "", "title": "", "blocks": []}
    for b in blocks:
        if b["kind"] == "heading":
            if current["blocks"] or current["slug"]:
                sections.append(current)
            current = {"slug": b["slug"], "title": b["text"], "blocks": []}
        else:
            current["blocks"].append(b)
    if current["blocks"] or current["slug"]:
        sections.append(current)
    return sections


def _is_legacy_sourcemap(section: dict) -> bool:
    txt = " ".join(
        b.get("text", "") for b in section["blocks"] if b["kind"] in ("prose", "bullets")
    )
    joined = " ".join(
        " ".join(b["items"]) if b["kind"] == "bullets" else b.get("text", "")
        for b in section["blocks"]
    )
    return "Source map:" in txt or joined.strip().startswith("Source map:")


def _render_block(b: dict, cited: list[str]) -> str:
    k = b["kind"]
    if k == "prose":
        if not b["text"]:
            return ""
        return f'<p>{html.escape(b["text"])}</p>'
    if k == "bullets":
        lis = "\n".join(f"<li>{html.escape(x)}</li>" for x in b["items"])
        return f"<ul>\n{lis}\n</ul>"
    if k == "math":
        return f'<div class="math-block">$$\n{b["tex"]}\n$$</div>'
    if k == "cite":
        cited.append(b["path"])
        return citation_chip_html(b["path"], b["sub"])
    if k == "figure":
        src = f'../../assets/units/{b["id"]}.svg'
        cap = html.escape(b["caption"])
        return (
            f'<figure class="unit-figure reveal">'
            f'<img src="{src}" alt="{cap}" loading="lazy" />'
            f'<figcaption>{cap}</figcaption></figure>'
        )
    if k == "why":
        return (
            f'<div class="why-callout reveal"><div class="why-label">Why</div>'
            f'<p>{html.escape(b["text"])}</p></div>'
        )
    if k == "key":
        return (
            f'<div class="key-idea reveal"><div class="key-label">Key idea</div>'
            f'<p>{html.escape(b["text"])}</p></div>'
        )
    if k == "code":
        return f'<pre class="code-block reveal"><code>{html.escape(b["text"])}</code></pre>'
    return ""


def render_body(raw: str) -> tuple[str, list[tuple[str, str]], list[str]]:
    _fm, body = split_frontmatter(raw)
    sections = _group_sections(_parse_blocks(body))
    out: list[str] = []
    headings: list[tuple[str, str]] = []
    cited: list[str] = []
    for sec in sections:
        if _is_legacy_sourcemap(sec):
            continue
        rendered = [_render_block(b, cited) for b in sec["blocks"]]
        rendered = [r for r in rendered if r]
        if not rendered:
            continue  # empty section: omit heading entirely
        if sec["slug"]:
            headings.append((sec["slug"], sec["title"]))
            out.append(
                f'<div class="section-rule"><h2 id="{sec["slug"]}">'
                f'{html.escape(sec["title"])}</h2></div>'
            )
        out.append('<div class="prose reveal">')
        out.extend(rendered)
        out.append("</div>")
    # dedupe cited, keep order
    seen: set[str] = set()
    cited_unique = [p for p in cited if not (p in seen or seen.add(p))]
    return "\n".join(out), headings, cited_unique


def iter_math_sections(raw: str) -> list[tuple[str, bool]]:
    _fm, body = split_frontmatter(raw)
    sections = _group_sections(_parse_blocks(body))
    result: list[tuple[str, bool]] = []
    for sec in sections:
        has_math = any(b["kind"] == "math" for b in sec["blocks"])
        if not has_math:
            continue
        has_prose = any(
            b["kind"] in ("prose", "bullets") and (b.get("text") or b.get("items"))
            for b in sec["blocks"]
        )
        result.append((sec["slug"], has_prose))
    return result
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tests/test_blocks.py`
Expected: PASS (11 tests).

- [ ] **Step 5: Commit**

```bash
git add site/tools/monograph/blocks.py site/tests/test_blocks.py
git commit -m "feat(monograph): narrative body grammar + renderer"
```

---

## Task 3: Narrative renderer + global filler removal (`render.py`, `diagrams.py`)

**Files:**
- Modify: `site/tools/monograph/render.py` (rewrite `render_leaf`; trim imports)
- Modify: `site/tools/monograph/diagrams.py` (delete `dual_box_svg`, lines 52-86)
- Modify: `site/styles.css` (append citation/key/figure CSS)
- Test: `site/tests/test_monograph_structure.py` (add global gates in Task 4; here we prove the build stays green)

**Interfaces:**
- Consumes: `blocks.render_body` (Task 2), `cite` (Task 1), existing `shell`, `paths.ROOT`, `paths.CONTENT`.
- Produces: unchanged `render_leaf(module: str, child: dict, hub: str | None) -> str` signature (so `build.py` needs no change).

- [ ] **Step 1: Add the CSS (needed before the exemplar renders correctly)**

Append to `site/styles.css`:

```css
/* Narrative monograph — citations, key ideas, figures (Wave 0) */
.citation-chip {
  display: flex; flex-direction: column; gap: 0.3rem;
  border-left: 2px solid var(--ray-hot);
  background: var(--paper); border-radius: 0 4px 4px 0;
  padding: 0.6rem 0.85rem; margin: 0.6rem 0 1rem;
}
.citation-chip .citation-code {
  font-family: var(--font-mono); font-size: 0.76rem; line-height: 1.4;
  color: var(--ivory-dim); white-space: pre-wrap; word-break: break-word;
}
.citation-chip .citation-loc {
  font-family: var(--font-mono); font-size: 0.72rem; color: var(--ray-hot);
  text-decoration: none; letter-spacing: 0.02em;
}
.citation-chip .citation-loc:hover { text-decoration: underline; }
.key-idea {
  border-left: 3px solid var(--emerald); background: var(--paper-lift);
  padding: 0.85rem 1.1rem; margin: 1rem 0; border-radius: 0 4px 4px 0;
}
.key-idea .key-label {
  font-family: var(--font-mono); font-size: 0.7rem; letter-spacing: 0.14em;
  text-transform: uppercase; color: var(--emerald); margin-bottom: 0.35rem;
}
.key-idea p { margin: 0; color: var(--ivory-dim); font-size: 0.95rem; line-height: 1.55; }
figure.unit-figure { margin: 1.25rem 0; padding: 0; }
figure.unit-figure img { width: 100%; max-width: 44rem; height: auto; display: block; margin: 0 auto; }
figure.unit-figure figcaption {
  font-family: var(--font-mono); font-size: 0.72rem; color: var(--ivory-mute);
  text-align: center; margin-top: 0.5rem; letter-spacing: 0.02em;
}
```

- [ ] **Step 2: Delete `dual_box_svg`**

In `site/tools/monograph/diagrams.py`, delete the entire `dual_box_svg` function (currently lines 52-86, the `def dual_box_svg(...)` block through its final `return "\n".join(parts)`).

- [ ] **Step 3: Rewrite `render.py`**

Replace the imports block and the `enrich_child` + `render_leaf` functions. Keep `shell` unchanged. New top of file and functions:

```python
"""HTML shell + narrative leaf rendering."""
from __future__ import annotations

from . import blocks
from .htmlutil import esc
from .paths import CONTENT, ROOT

# shell(...) stays exactly as-is below this line — do not modify it.
```

(Keep the existing `shell` function verbatim. Remove the old `from .code_probe import probe`, `from .content_loader import merge_content`, `from .diagrams import dual_box_svg, workflow_svg` line and the `from .htmlutil import algo, bullets, esc, paras` line — replace with the imports above plus `from .diagrams import workflow_svg` only if a later wave needs it; Wave 0 does not call it, so omit.)

Delete `enrich_child` entirely. Replace `render_leaf` with:

```python
def _sources_footer(paths: list[str]) -> str:
    if not paths:
        return ""
    rows = "\n".join(
        f'        <div class="row"><code>{esc(p)}</code>'
        f'<span class="desc">{"" if (ROOT / p).exists() else "path note"}</span></div>'
        for p in paths
    )
    return (
        '      <div class="section-rule"><h2 id="sources">Source files</h2></div>\n'
        f'      <div class="file-map reveal">\n{rows}\n      </div>'
    )


def _on_this_page(headings: list[tuple[str, str]]) -> str:
    if not headings:
        return ""
    items = "".join(f'<li><a href="#{slug}">{esc(title)}</a></li>' for slug, title in headings)
    return (
        '      <nav class="on-this-page reveal"><div class="otp-label">On this page</div>\n'
        f'        <ol>{items}<li><a href="#sources">Sources</a></li></ol>\n'
        "      </nav>"
    )


def render_leaf(module: str, child: dict, hub: str | None) -> str:
    md_path = CONTENT / module / f'{child["id"]}.md'
    raw = md_path.read_text(encoding="utf-8") if md_path.is_file() else ""
    body_html, headings, cited = blocks.render_body(raw) if raw else ("", [], [])

    # Sources footer: citations first, then the unit's declared files, deduped.
    footer_paths: list[str] = list(cited)
    for f in child.get("files") or []:
        if f not in footer_paths:
            footer_paths.append(f)

    hub_link = f'<a href="../{hub}">← Module hub</a>' if hub else '<a href="../sitemap.html">← Sitemap</a>'
    if not body_html:
        body_html = '<div class="prose reveal"><p>Documentation for this unit is in progress.</p></div>'

    return f'''
      <p class="crumb"><a href="../../index.html">Monograph</a> ·
        <a href="../sitemap.html">Tree</a> ·
        {hub_link} · {esc(child["title"])}</p>
      <header class="chapter-head reveal">
        <div class="chapter-num">§</div>
        <div>
          <p class="chapter-kicker">{esc(module)}</p>
          <h1>{esc(child["title"])}</h1>
        </div>
      </header>
      <div class="thesis-box reveal">
        <div class="label">Design unit</div>
        <p>{esc(child["summary"])}</p>
      </div>
{_on_this_page(headings)}

{body_html}
{_sources_footer(footer_paths)}
      <div class="aside ray reveal">
        <div class="aside-label">Navigate</div>
        <p>Parent hub for the full pipeline narrative; this page is the file-level design unit.
        <a href="../sitemap.html">Sitemap</a> · hover glossary terms anywhere.</p>
      </div>
'''
```

- [ ] **Step 4: Rebuild the whole site**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tools/generate_tree.py`
Expected: prints "Generated N leaf pages across M modules" and exits 0. (Legacy `.md` files still build because unknown constructs fall through to prose; `{{cite}}` appears in none of them yet.)

- [ ] **Step 5: Verify existing structural tests still pass**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tests/test_monograph_structure.py`
Expected: PASS. If `test_codebase_tree_coverage` fails on "What it is"/"How we implement"/"Source files" strings: those assertions check the *old* template wording. Update just those three asserts to match the new page (they should assert `"On this page"` and `"Source files"` — "Source files" is retained by `_sources_footer`; remove the `"What it is"` and `"How we implement"` asserts, which no longer describe the design). Re-run until green.

- [ ] **Step 6: Spot-check that filler is gone globally**

Run:
```bash
cd /home/frankyin/Desktop/Github/ohao_engine
grep -rl "Sources of truth" site/m/ | wc -l
grep -rl "API / symbols the unit exposes or binds" site/m/ | wc -l
grep -rl "unit-diagram" site/m/ | grep -v sitemap | wc -l
```
Expected: `0`, `0`, and `0` (no dual-box diagrams left; workflow diagrams are not emitted in Wave 0).

- [ ] **Step 7: Commit**

```bash
git add site/tools/monograph/render.py site/tools/monograph/diagrams.py site/styles.css site/tests/test_monograph_structure.py site/m site/js
git commit -m "feat(monograph): narrative renderer, drop filler + dual-box sitewide"
```

---

## Task 4: Gate tests + `pbr-model` exemplar

**Files:**
- Modify: `site/tests/test_monograph_structure.py` (add five gate tests)
- Create: `site/assets/units/ggx_multiscatter_energy.svg`
- Rewrite: `site/content/units/materials/pbr-model.md`

**Interfaces:**
- Consumes: `blocks.iter_math_sections`, `cite.resolve_citation`, `cite.CitationError`, `blocks.split_frontmatter`.

- [ ] **Step 1: Write the five gate tests**

Append to `site/tests/test_monograph_structure.py` (inside the class, or as a new `class GateTest(unittest.TestCase)` at module level; use the latter to avoid touching existing methods). Add near the top of the file, after the existing imports:

```python
sys.path.insert(0, str(SITE / "tools"))
from monograph import blocks as _blocks  # noqa: E402
from monograph import cite as _cite      # noqa: E402

CONTENT = SITE / "content" / "units"


class GateTest(unittest.TestCase):
    def _md_files(self):
        return sorted(CONTENT.rglob("*.md"))

    def test_no_sources_of_truth_filler(self):
        hits = [str(p.relative_to(SITE)) for p in M.rglob("*.html")
                if "Sources of truth:" in p.read_text(encoding="utf-8", errors="replace")]
        self.assertEqual(hits, [], f"filler present: {hits}")

    def test_no_api_dump(self):
        needle = "API / symbols the unit exposes or binds"
        hits = [str(p.relative_to(SITE)) for p in M.rglob("*.html")
                if needle in p.read_text(encoding="utf-8", errors="replace")]
        self.assertEqual(hits, [], f"api dump present: {hits}")

    def test_all_citations_resolve(self):
        errors = []
        for md in self._md_files():
            _fm, body = _blocks.split_frontmatter(md.read_text(encoding="utf-8"))
            for m in re.finditer(r'\{\{cite\s+(\S+)\s+"(.+?)"\}\}', body):
                try:
                    _cite.resolve_citation(m.group(1), m.group(2))
                except _cite.CitationError as e:
                    errors.append(f"{md.relative_to(CONTENT)}: {e}")
        self.assertEqual(errors, [], "\n".join(errors))

    def test_no_empty_section_rules(self):
        # Every <h2 id=...> must be followed by non-empty content before the next h2/footer.
        bad = []
        for p in M.rglob("*.html"):
            html = p.read_text(encoding="utf-8", errors="replace")
            for m in re.finditer(r'<h2 id="([^"]+)">.*?</h2></div>\s*(.*?)(?=<div class="section-rule"|<div class="aside|</article>)',
                                 html, re.S):
                if not re.sub(r"<[^>]+>|\s", "", m.group(2)):
                    bad.append(f"{p.relative_to(SITE)}#{m.group(1)}")
        self.assertEqual(bad, [], f"empty sections: {bad}")

    def test_v2_math_blocks_are_explained(self):
        bad = []
        for md in self._md_files():
            fm, _ = _blocks.split_frontmatter(md.read_text(encoding="utf-8"))
            if fm.get("standard") != "v2":
                continue
            for slug, has_prose in _blocks.iter_math_sections(md.read_text(encoding="utf-8")):
                if not has_prose:
                    bad.append(f"{md.relative_to(CONTENT)}#{slug}")
        self.assertEqual(bad, [], f"unexplained math in v2 pages: {bad}")
```

- [ ] **Step 2: Run gate tests to verify they currently pass (except math gate, which is vacuous until the exemplar exists)**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python site/tests/test_monograph_structure.py`
Expected: PASS. (After Task 3 the filler is already gone, so the four global gates pass now; `test_v2_math_blocks_are_explained` passes vacuously — no `standard: v2` file exists yet.)

- [ ] **Step 3: Create the exemplar figure**

Create `site/assets/units/ggx_multiscatter_energy.svg` (a real measured-shape plot: single-scatter GGX directional albedo E dropping with roughness, and the compensated total returning to ~1; ink palette matching existing plates):

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 720 320" class="unit-diagram" role="img" aria-label="GGX single-scatter energy loss vs roughness, with multi-scatter compensation">
<rect width="100%" height="100%" fill="#0f1419"/>
<text x="28" y="26" fill="#c4a574" font-family="IBM Plex Mono,monospace" font-size="11" letter-spacing="1.5">GGX DIRECTIONAL ALBEDO E(roughness), NdotV=1</text>
<!-- axes -->
<line x1="70" y1="40" x2="70" y2="260" stroke="#3d4a58" stroke-width="1"/>
<line x1="70" y1="260" x2="680" y2="260" stroke="#3d4a58" stroke-width="1"/>
<text x="24" y="52" fill="#8a8490" font-family="IBM Plex Mono,monospace" font-size="10">1.0</text>
<text x="24" y="150" fill="#8a8490" font-family="IBM Plex Mono,monospace" font-size="10">0.7</text>
<text x="24" y="262" fill="#8a8490" font-family="IBM Plex Mono,monospace" font-size="10">0.4</text>
<text x="66" y="278" fill="#8a8490" font-family="IBM Plex Mono,monospace" font-size="10">0</text>
<text x="660" y="278" fill="#8a8490" font-family="IBM Plex Mono,monospace" font-size="10">1</text>
<text x="360" y="296" fill="#8a8490" font-family="DM Sans,sans-serif" font-size="12">roughness →</text>
<!-- compensated total ~= 1 (flat top) -->
<polyline points="70,52 375,54 680,58" fill="none" stroke="#7dba8a" stroke-width="2"/>
<text x="470" y="46" fill="#7dba8a" font-family="DM Sans,sans-serif" font-size="12">single + multi-scatter (energy conserved)</text>
<!-- single-scatter only: droops toward high roughness -->
<polyline points="70,54 200,66 340,104 470,150 600,196 680,224" fill="none" stroke="#c4784a" stroke-width="2"/>
<text x="360" y="200" fill="#c4784a" font-family="DM Sans,sans-serif" font-size="12">single-scatter GGX (loses energy)</text>
<!-- lost-energy shading gap -->
<text x="120" y="240" fill="#6e685c" font-family="DM Sans,sans-serif" font-size="11">gap = energy Kulla-Conty/Turquin adds back</text>
<text x="28" y="314" fill="#6e685c" font-family="IBM Plex Mono,monospace" font-size="10">Conceptual shape from GGX_E() polynomial (Turquin 2019) — brdf_ggx.glsl. Not a captured render.</text>
</svg>
```

- [ ] **Step 4: Rewrite the exemplar `pbr-model.md`**

Overwrite `site/content/units/materials/pbr-model.md` with the `standard: v2` narrative (every `{{cite}}` below is pre-verified unique against the current tree):

```markdown
---
module: materials
id: pbr-model
title: PBR metallic-roughness
standard: v2
figures: [ggx_multiscatter_energy]
---

## The lie artists can paint

The physically-correct microfacet BRDF wants a spectral, complex index of
refraction at every surface point. No artist authors that. OHAO collapses it to
three scalars an artist can paint into textures — base color, metallic, roughness
— and reconstructs the physics at shade time. This page is about what that
reconstruction gets right and the three places it deliberately cheats.

## F0: the 0.04 that is not arbitrary

F0 is the Fresnel reflectance at normal incidence: the fraction of light a flat
surface bounces straight back when you look at it head-on. For a dielectric it is
fixed by the index of refraction n through F0 = ((n - 1)/(n + 1))^2. Common
dielectrics sit near n = 1.5, so:

$$F_0^{\text{dielectric}} = \left(\frac{1.5 - 1}{1.5 + 1}\right)^2 \approx 0.04$$

OHAO hard-codes that constant as the dielectric base reflectivity.

{{cite shaders/includes/material/material_types.glsl "surface.F0 = vec3(0.04); // Dielectric F0"}}

Metals have no meaningful single-scalar IOR in this model and absorb all
transmitted light within nanometers, so they get zero diffuse and reuse base
color as F0. One branch-free `mix` spans the whole range:

{{cite shaders/includes/material/material_types.glsl "return mix(dielectricF0, surface.albedo, surface.metallic);"}}

:::key
The intermediate values of that lerp (metallic = 0.5) are not physical — no real
material is half-conductor. But the slider is continuous and monotonic, which is
all an artist needs, and pure 0/1 endpoints are exactly right.
:::

## Fresnel at grazing angles: the Schlick term

Head-on reflectance is only F0; at grazing angles every surface approaches a
perfect mirror. OHAO uses the roughness-aware Schlick approximation:

$$F(\theta) = F_0 + \big(\max(1-\text{rough},\,F_0) - F_0\big)\,(1 - \cos\theta)^5$$

{{cite shaders/includes/lighting/ibl.glsl "return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);"}}

The textbook Schlick term tops out at 1.0. The `max(1 - roughness, F0)` cap is the
cheat: on a rough surface the grazing highlight should not blow out to a mirror,
so the ceiling is pulled down as roughness rises. This matches the integrated
reflectance a pre-baked BRDF LUT would give, without the texture fetch.

## Past textbook Cook-Torrance

Most engines evaluate specular as single-scatter D·G·F and stop. OHAO adds two
refinements. First, the geometry term is the height-correlated Smith form, which
accounts for masking and shadowing being correlated on the same microsurface
rather than independent:

{{cite shaders/includes/brdf/brdf_ggx.glsl "float geometrySmithCorrelated(float NdotV, float NdotL, float roughness) {"}}

Second — and this is the part a reviewer will notice is missing from most hobby
renderers — single-scatter GGX loses energy at high roughness because it models
exactly one bounce off the microsurface. Rough metal comes out visibly too dark.
OHAO adds the lost energy back with a Kulla-Conty / Turquin compensation term:

$$E_{ms} = (1 - E(\mu_o))\,(1 - E(\mu_i)), \qquad f_{ms} = \frac{F_{avg}\,E_{ms}}{1 - F_{avg}(1 - E(\mu_o))}$$

where E is the directional albedo of single-scatter GGX, evaluated from a
polynomial fit instead of a lookup table.

{{cite shaders/includes/brdf/brdf_ggx.glsl "float Ems = (1.0 - E_o) * (1.0 - E_i);"}}
{{cite shaders/includes/brdf/brdf_ggx.glsl "vec3 Favg = surface.F0 + (1.0 - surface.F0) / 21.0;"}}

{{figure ggx_multiscatter_energy "Single-scatter GGX droops with roughness; the compensation term returns total energy to ~1. Shape from the GGX_E() fit in brdf_ggx.glsl — conceptual, not a captured render."}}

:::why
Energy compensation is not cosmetic. Without it, rough metals read darker than
their albedo implies, so an artist compensates by over-brightening the base
color, and the asset then looks wrong under a different light. Matching Cycles'
multi-scatter GGX keeps look-dev portable between the offline and realtime paths.
:::

## The direct-vs-IBL k remap

The Smith-Schlick geometry approximation needs a roughness remap, and it is a
different constant for punctual lights than for image-based lighting — a subtlety
that silently darkens IBL if you use the wrong one:

{{cite shaders/includes/brdf/brdf_ggx.glsl "float k = (r * r) / 8.0;"}}

This `(rough + 1)^2 / 8` form is the direct-lighting remap (Karis 2013). The IBL
path uses `rough^2 / 2`; they are not interchangeable.

## Where the packed roughness comes from

At the closest-hit shader the material arrives packed into a payload vector, and
the unpack carries a backward-compatibility story worth knowing before you touch
it. The current encoding is continuous roughness in `.x`, metallic in `.y`:

{{cite shaders/includes/pbr_unpack.glsl "if (att.x < 0.0 && att.y < 1e-4) {"}}

A legacy signed encoding (negative `.x` meant "binary metal") is still accepted
when `.y` is ~0, so old shader-binding-table payloads authored mid-session do not
silently break. Removing that branch is safe only once every producer writes the
new form.

## Contracts

- Metals must have zero diffuse and F0 = base color; dielectrics F0 = 0.04. Violating this double-counts or loses energy.
- Roughness is clamped away from 0 (`max(roughness, 0.01)` in the unpack) so the specular lobe never becomes a zero-width delta that fireflies in the path tracer.
- The direct and IBL geometry remaps are not interchangeable; using the IBL k for punctual lights darkens direct specular.
```

- [ ] **Step 5: Rebuild and run the full suite**

Run:
```bash
cd /home/frankyin/Desktop/Github/ohao_engine
python site/tools/generate_tree.py && python site/tests/test_monograph_structure.py && python site/tests/test_cite.py && python site/tests/test_blocks.py
```
Expected: build exits 0; all three test files PASS. In particular `test_v2_math_blocks_are_explained` now exercises the exemplar's three math sections and passes (each has explanatory prose).

- [ ] **Step 6: Eyeball the exemplar**

Run: `cd /home/frankyin/Desktop/Github/ohao_engine && python -m http.server 8765 --directory site` (background), open `http://127.0.0.1:8765/m/materials/pbr-model.html`. Confirm: narrative reads top-to-bottom as an essay; three citation chips resolve to real lines and link to GitHub; the figure renders with its caption; on-this-page lists the movement headings; no "Sources of truth", no API dump, no truncated diagram. Stop the server when done.

- [ ] **Step 7: Commit**

```bash
git add site/tests/test_monograph_structure.py site/assets/units/ggx_multiscatter_energy.svg site/content/units/materials/pbr-model.md site/m site/js
git commit -m "feat(monograph): five gate tests + pbr-model exemplar (standard v2)"
```

---

## Self-Review

**Spec coverage** (against `2026-07-24-monograph-depth-design.md`):
- §4.1 explained math → Task 4 `test_v2_math_blocks_are_explained` + `iter_math_sections`. ✓
- §4.3 no "Sources of truth" filler → Task 2 `_is_filler` + Task 4 `test_no_sources_of_truth_filler`. ✓
- §4.4 no API dump → Task 3 drops `probe` api/snippet; Task 4 `test_no_api_dump`. ✓
- §4.5 sections earn their place → Task 2 empty-section omission + Task 4 `test_no_empty_section_rules`. ✓
- §4.6 / §6 verified citations → Task 1 `cite.py` + Task 4 `test_all_citations_resolve`; build fails on stale/ambiguous. ✓
- §5 narrative body, thin frame, auto on-this-page, sources footer → Task 3 `render_leaf`, `_on_this_page`, `_sources_footer`. ✓
- §7 delete `dual_box_svg`, keep figures that carry info → Task 3 deletion + Task 4 figure. ✓
- §8 Wave 0 = infra + pbr-model exemplar → this plan. ✓ (Waves 1–5 are out of scope here, authored later via the workflow.)
- §9 grounding/honesty → every exemplar citation pre-verified unique; figure caption marked conceptual. ✓

**Placeholder scan:** No TBD/TODO; all code and test bodies are complete and runnable; the one interim `Documentation for this unit is in progress.` string is intentional fallback copy for not-yet-authored units, not a plan placeholder.

**Type consistency:** `render_body -> (str, list[tuple[str,str]], list[str])` is produced in Task 2 and consumed identically in Task 3 (`body_html, headings, cited`). `resolve_citation -> (int, str)` and `citation_chip_html -> str` are consistent across Tasks 1, 2, 4. `iter_math_sections -> list[tuple[str,bool]]` consistent between Tasks 2 and 4. `render_leaf(module, child, hub)` signature unchanged, so `build.py` is untouched. ✓

**Known interim state (documented, not a defect):** after Wave 0, the ~72 legacy `.md` files render as clean prose (filler filtered) but are not yet at the top-tier bar; their `## Math` legacy sections (mis, ggx, raygen) are not `standard: v2`, so they are exempt from the math gate until reauthored in Waves 1–5. This is the intended migration path from the spec.
