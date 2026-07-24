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

from .cite import citation_chip_html

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
    slug_counts: dict[str, int] = {}

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
            slug = _slug(title)
            slug_counts[slug] = slug_counts.get(slug, 0) + 1
            count = slug_counts[slug]
            if count > 1:
                slug = f"{slug}-{count}"
            blocks.append({"kind": "heading", "text": title, "slug": slug})
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
                if i >= len(lines):
                    raise ValueError("blocks: unterminated $$ math block")
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
            if i >= len(lines):
                raise ValueError(f"blocks: unterminated {s} callout")
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
            if i >= len(lines):
                raise ValueError("blocks: unterminated ``` code fence")
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
    """True iff some prose block in the section is exactly "Source map:".

    This is the auto-generated legacy source-map list marker; a section is
    only dropped when it contains that exact standalone prose line, never on
    a mere substring match elsewhere in the prose.
    """
    return any(
        b["kind"] == "prose" and b.get("text", "").strip() == "Source map:"
        for b in section["blocks"]
    )


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
