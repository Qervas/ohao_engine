# OHAO Monograph — Depth Pass & Noise Removal

**Date:** 2026-07-24
**Status:** Design approved, pending spec review
**Scope:** `site/` documentation monograph only. No engine code changes.

---

## 1. Problem

The monograph under `site/m/` presents 113 pages (93 leaf "design units" + hubs)
that *look* like a rigorous engineering monograph but are machine-scaffolded stubs.

Root cause: `site/tools/monograph/author_rich.py` auto-generates one markdown stub
per unit from regex code-probes (`_auto_fill`, `KNOWLEDGE`, `write_unit_md`), then
`render.py` wraps every stub in seven fixed ruled sections
(What / How / Math / Why / Contracts / In-the-code / Sources) whether or not there
is anything to say.

Concrete defects, verified on `m/materials/pbr-model.html`:

- **Math without explanation.** `F₀ ≈ 0.04` and `F₀ = lerp(0.04, c_base, m)` render
  as bare formulas. No definition of F₀ (Fresnel reflectance at normal incidence),
  no derivation from `((n−1)/(n+1))²`, no Schlick term — which the shader actually
  computes in `shaders/includes/lighting/ibl.glsl`.
- **Shallow "Why."** One sentence ("one material language for deferred and PT")
  that never explains why metallic-roughness exists or what it trades away.
- **Filler "Contracts & invariants."** Auto-filled with
  `"Sources of truth: <filepath>"` bullets (`author_rich.py:823`) — 218 of them
  across 151 pages — duplicating the Source-files table two sections below.
- **API-dump noise.** `code_probe.extract_api` lists every regex-matched GLSL
  function including the builtin `clamp()`, with `distributionGGX`,
  `geometrySmith`, `geometrySchlickGGX` each appearing twice.
- **Truncating auto-diagram.** `diagrams.dual_box_svg` clips text mid-word
  ("Metallic-roughness PBR: baseColor, m", "pbr_unpack.glsl expands packed rows/")
  and only restates the adjacent prose.

The deeper finding: **the engine is far more sophisticated than its own docs admit.**
`brdf_ggx.glsl` alone implements height-correlated Smith, Kulla-Conty/Turquin
multi-scatter energy compensation, Burley diffuse, roughness-aware Fresnel, the
direct-vs-IBL `k` remap, and the Karis analytic split-sum — none surfaced. The
substance already exists in the code; the docs simply fail to do it justice.

Corpus stats: median 107 words/unit, 30 units under 90 words, 3 of 93 with any math.

## 2. Goal

Every submodule page meets one uniform **top-tier standard** — the depth a graphics
engineer or systems PhD would respect — grounded line-by-line in the real code.
No visible tiers, no "brief" pages. Page *length* varies with real substance; the
*craft bar* is uniform and nothing ships below it.

## 3. Non-goals (YAGNI)

- No redesign of `styles.css` layout, fonts, or color system. Additive CSS only
  (citation chips, figure captions, callout variants).
- No changes to chrome (`js/chrome.js`), nav-tree, glossary, or hub-page structure
  beyond what the content model requires.
- No engine code changes. The monograph documents the tree as it is.
- No new runtime JS framework. KaTeX (already loaded) stays; everything else is
  build-time Python + static HTML.

## 4. The standard (enforceable rules, not vibes)

A page conforms iff:

1. **Every formula is explained.** A math block must sit in a `##` section that also
   contains explanatory prose defining its symbols and stating where the formula
   comes from (derivation *or* a `{{cite}}`). A math block with no prose in its
   section is a **build error**.
2. **Every "Why" names its rejected alternative** and the consequence of choosing
   otherwise. Enforced in authoring/review, not by the build.
3. **"Invariants" holds only real invariants** — statements that, if violated, break
   correctness. The literal string `Sources of truth:` is banned sitewide (test).
4. **No API dump.** `extract_api` output is never rendered. Symbols are named only
   when hand-picked and annotated with what they do.
5. **Sections earn their place.** A section renders only if it has authored body
   content. No empty ruled headers.
6. **Every code claim is a verified `{{cite}}`** (§6). Build fails on any stale or
   ambiguous anchor.

## 5. Content model — narrative body, thin frame

### 5.1 Page anatomy

**Frame** (rendered on every page, thin and consistent):
- crumb + chapter header (module kicker + title) — unchanged
- `thesis-box` — one-sentence claim of what the unit is
- `on-this-page` — **auto-generated from the page's own `##` headings** (not the
  fixed What/How/Why list)
- **sources footer** — auto-collected, deduped from every `{{cite}}` path on the
  page plus any `files:` frontmatter. Replaces the old standalone "Source files"
  table (removing the duplication the reader complained about).
- pager — unchanged

**Body** — authored prose in argument order, composed from a small block vocabulary.
The rigid seven-section skeleton is removed.

### 5.2 Authoring format (`site/content/units/<module>/<id>.md`)

Frontmatter:
```yaml
---
module: materials
id: pbr-model
title: PBR metallic-roughness
figures: [ggx_multiscatter_energy]   # optional; ids under site/assets/units/
files: []                             # optional extra source paths for the footer
---
```

Body grammar (superset of the current `##`-section parser):

| Construct | Syntax | Renders as |
|---|---|---|
| Movement heading | `## Title` | `section-rule` h2; added to on-this-page |
| Prose | plain paragraphs | `.prose` `<p>` |
| Explained math | `$$ … $$` block **inside** a `##` section with prose | `.math-block` (KaTeX) — build-checked for adjacent prose |
| Code citation | `{{cite path "unique substring"}}` | citation chip (§6) |
| Why callout | `:::why … :::` | `.why-callout` |
| Key idea | `:::key … :::` | `.key-idea` callout (new, additive CSS) |
| Figure | `{{figure id "caption"}}` | `<figure>` + `<figcaption>` from `site/assets/units/<id>.svg` |
| Illustrative snippet | fenced ```lang | `.code-block` (used sparingly; prefer `{{cite}}` to real files) |

Parsing extends `content_loader.py`. The `KNOWLEDGE` dict and `_auto_fill` prose
generation in `author_rich.py` are **removed from the ship path**; what remains is
a `scaffold` command that emits a `## TODO`-marked skeleton (frontmatter + empty
movement headings + the unit's file list as `{{cite}}` starting points) for an
un-authored unit. Skeletons are never published — a test rejects any page whose
body still contains `TODO`.

### 5.3 Renderer changes (`render.py`)

- Delete the fixed What/How/Why/Contracts/Code/Files scaffold and the unconditional
  seven `section-rule` blocks.
- Render the authored body: walk movements, emit each block via its component.
- Drop the `enrich_child` calls to `probe()` for `_api` and `_snippet`; those no
  longer feed content. `code_probe.extract_api` and `extract_snippet` remain in the
  module but are unused by the render path (kept only for the optional scaffolder).
- Auto-build on-this-page from `##` headings; auto-build the sources footer from
  citations.

## 6. Citation system — the anti-rot spine

New module `site/tools/monograph/cite.py`. At build time, each
`{{cite path "substring"}}`:

1. Reads the real repo file at `path` (relative to repo root).
2. Finds lines containing the exact `substring`.
3. **0 matches → build error** naming the unit, path, and substring.
   **>1 match → build error** listing every matching line number, so the author
   disambiguates by extending the substring.
4. **1 match → emit a citation chip**: the resolved `path:line`, the source line
   itself (escaped, monospace), linking to the file on the project's GitHub at that
   line. The resolved line number is computed at build, never hand-typed.

Link base (verified): `https://github.com/Qervas/ohao_engine/blob/master/<path>#L<line>`,
held as a single constant `GITHUB_BLOB_BASE` in `cite.py`. If the resolved remote
is ever absent, the chip degrades to plain `path:line` text (no link) rather than
emitting a dead URL.

Tests (`site/tests/test_monograph_structure.py`, extends existing file):
- `test_citations_resolve` — walk every `{{cite}}` in `content/`; assert each
  resolves to exactly one line. Fails CI on stale/ambiguous anchors.
- `test_no_sources_of_truth_filler` — assert `Sources of truth:` appears in zero
  rendered HTML files.
- `test_no_empty_sections` — assert no `section-rule` h2 is followed by an empty
  body.
- `test_no_api_dump` — assert the string `API / symbols the unit exposes or binds`
  appears in zero rendered files.
- `test_math_has_explanation` — for each `.md`, assert every `$$` block's `##`
  section also contains ≥1 prose paragraph.

Existing tests (`test_path_tracer_nee_sphere_walk`, `test_conceptual_art_captioned`,
etc.) must stay green; run the suite before and after.

## 7. Figures

- **Delete `dual_box_svg`** from `diagrams.py` and its call site in `render.py`.
- **Keep `workflow_svg`**, rendered only when a unit authors an explicit ordered
  step list.
- **Hand-author figures that carry information prose can't**, as standalone SVG under
  `site/assets/units/<id>.svg`, referenced by `{{figure}}`. Priority order follows
  the crown-jewel clusters. Examples: MIS variance-vs-roughness crossover; GGX lobe
  geometry + D·G·F split; Kulla-Conty energy-loss-vs-roughness with the compensation
  curve; NRD buffer lifetime across bindings 22–28 with per-stage ms; CSM cascade
  split distances from the real constants.
- Every figure caption states **conceptual vs measured**, matching the existing
  `test_conceptual_art_captioned` discipline. A figure that plots real numbers cites
  where they came from.

## 8. Execution — parallel-agent authoring, human-verified

Approved path: multi-agent `Workflow`, opt-in confirmed by the user.

**Wave 0 — infrastructure (authored directly, no agents):**
1. `cite.py` + the five new tests.
2. `content_loader.py` grammar extension (`{{cite}}`, `:::why`, `:::key`,
   `{{figure}}`, auto on-this-page, sources footer).
3. `render.py` narrative rewrite; remove seven-section scaffold; delete
   `dual_box_svg`.
4. Additive CSS: `.citation-chip`, `.key-idea`, `figure`/`figcaption`.
5. Convert **one** unit by hand (`materials/pbr-model`) as the reference exemplar
   and the fixture the tests run against. Ship it, eyeball it, lock the bar.

**Waves 1–4 — crown-jewel clusters** (author to the exemplar bar):
sampling (5) → BRDF/PT (6) → denoise (5) → GPU (5). Each cluster is a `Workflow`:
- **Fan out**: one agent per submodule. Each agent deep-reads that unit's real
  source files, then drafts the `.md` to the standard, using `{{cite}}` for every
  code claim and `$$…$$` + prose for every formula.
- **Pipeline verify**: for each drafted unit, a verification agent (and then the
  build itself) checks every `{{cite}}` resolves to one line, every math block has
  explanation, no filler string, no API dump.
- **Human gate**: after each cluster builds green, the user reviews the rendered
  pages before the next wave starts (honoring "validate before moving on").

**Wave 5 — remaining ~72 units** in sub-clusters, same fan-out/verify/gate loop.

**Invariant across all waves:** a page ships only when it builds green under the
five new tests. Unauthored units keep their old page or a clearly-marked skeleton —
never a padded fake.

## 9. Grounding / honesty constraint

Every claim must survive `grep` against the current tree. No circular validation,
no invented numbers, no aspirational features. When a figure or sentence asserts a
measurement, it cites its source. This directly honors the project's
`inverse_rendering_honesty` and `feedback_bias_in_offline` principles: the docs
describe what the code does, not what we wish it did. Where the code contains a
known limitation (e.g. RT static-BLAS shows bind pose for animated meshes), the page
says so.

## 10. Acceptance criteria

- `Sources of truth:` and the API-dump heading appear in **zero** rendered pages.
- `dual_box_svg` is deleted; no truncated auto-diagram remains.
- Every `{{cite}}` in `content/` resolves to exactly one line; build fails otherwise.
- Every `$$` math block has explanatory prose in its section.
- `materials/pbr-model` reads as a coherent essay covering F₀ derivation,
  metallic-roughness as artist reduction, height-correlated Smith, and Kulla-Conty
  multi-scatter — every code claim a live citation.
- The full existing test suite plus the five new tests pass.
- No page ships below the exemplar bar.

## 11. Risks

- **Author drift across agents.** Mitigated by the pbr-model exemplar as a concrete
  target and the build-enforced gates (a page can't ship shallow if it can't ship at
  all without resolving citations and explaining math).
- **Citation ambiguity churn** on large shaders (raygen is 1500+ lines). Mitigated
  by the ambiguity error listing all matches so the author extends the substring;
  substrings, not line numbers, are the source of truth.
- **Token cost** of the fan-out. Accepted; user opted in. Bounded by authoring in
  waves with human gates rather than one mega-run.
- **GitHub link base** for citation chips must match the repo's actual remote/branch;
  make it a single configurable constant in `cite.py`.
