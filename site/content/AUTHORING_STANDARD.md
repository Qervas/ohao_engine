# OHAO Monograph — Authoring Standard

You are writing one page of a technical monograph about a real Vulkan 1.3
engine. The reader is a senior graphics engineer or a rendering PhD. They will
check your claims against the code. Write accordingly.

Reference exemplar — read it before you write:
`site/content/units/materials/pbr-model.md`

---

## 1. The one rule that matters most

**A citation resolving is NOT proof your prose is true.**

The build verifies that a cited substring exists in a file. It cannot verify
that the file is on the pipeline your sentence claims, or that the function is
even called. Two earlier drafts of the exemplar cited real, uniquely-resolving
lines and were still wrong:

- They attributed the deferred pipeline's multi-scatter compensation to the path
  tracer. In fact `brdf_ggx.glsl`'s `evaluateBRDF` is called only from
  `forward.frag` / `deferred_lighting.frag`; the path tracer includes
  `ggx_aniso.glsl` instead and has no such term.
- They called a `(r+1)²/8` remap "dead code" because the *named* function
  `geometrySchlickGGX` has no callers — while the identical formula is inlined
  in every NEE branch of all three raygen shaders. Function unused ≠ formula unused.

**Therefore, before you claim anything:**

- `grep -rn "thing" shaders/ ohao/` — who actually calls it? Zero callers is a
  fact worth stating, but check for inlined copies of the same math first.
- For a shader claim, trace the `#include` graph: which stage pulls this file?
  `grep -rn "include.*<file>" shaders/`
- For "the path tracer does X", know *which* raygen. The default offline
  `PathTracer` binds `pt_raygen.rgen` (see `ohao/render/rt/path_tracer.hpp`);
  `pt_raygen_realtime.rgen` and `pt_raygen_offline.rgen` are different profiles
  and genuinely differ in behaviour.
- If you cannot verify a claim, **do not make it**. A shorter true page beats a
  longer plausible one. Say "not verified here" or omit.
- Where the code has a real limitation, say so. Honest gaps are the most
  credible thing on the page.

## 2. What "top tier" means

Not length. A page earns depth by answering questions a smart reader actually
has. The failure mode you are replacing looks like this:

> **What** — GBuffer pass rasterizes scene into MRT: albedo, normals, material
> params, motion vectors, depth. **How** — gbuffer.vert: world pos, normal…
> **Why** — Decouple geometry bandwidth from light count.

That is a bullet list wearing a monograph's clothes. It states *that* things
exist and never *why they are that way*, what the alternatives were, what breaks.

A good page answers, where the unit genuinely raises them:

- What problem does this exist to solve, and what happens without it?
- What is the actual algorithm/math, explained — not just named?
- What did the engine choose, and what did it choose *against*? Name the
  rejected alternative and its cost.
- Where are the sharp edges: precision limits, ordering constraints, packing
  tricks, a legacy branch, a known bias, a perf cliff?
- What silently breaks if someone "cleans this up"?

Concrete beats abstract: real formats (`R10G10B10A2`), real binding numbers,
real constants read from the code, real function names.

## 3. Length and proportion

Let substance set length. Do NOT pad to a quota.

- A rich unit (a BRDF, an integrator, a denoiser, a packing scheme) may run
  700–1400 words.
- A genuinely small unit (a math-constants header, a debug helper) may be
  200–400 words and should be. Padding it re-creates the problem we are fixing.

Every sentence must carry information. Cut anything that restates the heading.

## 4. Grammar (the build parses exactly this)

Frontmatter — `standard: v2` is required, it turns on the quality gates:

```markdown
---
module: deferred
id: gbuffer
title: GBuffer pass
standard: v2
---
```

Body constructs:

| Construct | Syntax |
|---|---|
| Movement heading | `## Title` (these become the on-this-page nav) |
| Prose | plain paragraphs, blank line between |
| Bullets | lines starting `- ` |
| Math | `$$ ... $$` (KaTeX). MUST have explanatory prose in the same `##` section |
| Code citation | `{{cite path/to/file.ext "unique substring"}}` on its own line |
| Why callout | `:::why` … `:::` — the design rationale, names the rejected alternative |
| Key idea | `:::key` … `:::` — the one thing to remember |
| Figure | `{{figure figure-id "Caption stating conceptual vs measured."}}` |
| Illustrative code | triple-backtick fence (use sparingly — prefer `{{cite}}` to real code) |

**Citations are the backbone.** Rules:

- The substring must appear **exactly once** in that file, or the build fails.
  Extend the substring to disambiguate — never hand-write a line number.
- Path is relative to repo root: `shaders/rt/pt_raygen.rgen`, `ohao/...`.
- Cite the line your sentence is *about*. Put the citation right after the
  sentence it supports.
- Do not cite a line you have not read in context.
- 4–12 citations is typical for a substantial page. Do not spray them.

**Headings**: write real movement titles, not the old `What`/`How`/`Why` boxes.
Good: `## Why 32 bits of depth is not enough`, `## The packing that buys a channel`.
Duplicate headings auto-suffix (`foo`, `foo-2`) but avoid them anyway.

## 5. Math, figures, code — use when needed, never as decoration

- **Math**: include when the unit *is* mathematical. Define every symbol, say
  where the formula comes from, and connect it to the code. A bare formula is a
  build error. Do not invent formulas the code does not implement.
- **Figures**: only when the figure carries information the prose cannot — a
  layout, a lifetime, a geometry, a curve. Do not draw a box containing the
  sentence next to it. Most pages need zero or one.
- **Fenced code**: prefer `{{cite}}` (verified, linked, live) over pasted code.
  Use a fence only for something not in the tree — a formula sketch, a memory
  layout diagram in text.

### Figure authoring

If a figure genuinely helps, write it as standalone SVG to
`site/assets/units/<figure-id>.svg` and reference it with `{{figure ...}}`.
Match the house ink palette and be readable at 720px wide:

```
background #0f1419   panel #1a222c    stroke #3d4a58
text       #e8e2d6   label #c4a574    accent #c4784a
muted      #6e685c   good  #7dba8a
fonts: "IBM Plex Mono,monospace" (labels), "DM Sans,sans-serif" (body)
viewBox="0 0 720 H", class="unit-diagram", role="img", aria-label="..."
```

The caption **must** state whether it is conceptual or measured. If it plots
numbers, say where they came from. Never imply a diagram is a captured render.

## 6. Anti-patterns — automatic rejection

- `Sources of truth: <path>` or any file-list dressed as invariants.
- A dump of function names ("API / symbols…"). Name symbols only with what they do.
- Restating the page title as the first sentence.
- Marketing adjectives: "robust", "comprehensive", "powerful", "seamless".
- Inventing performance numbers, spp counts, or timings. Cite or omit.
- Claiming a feature the code does not have; describing intent as if shipped.
- A `## Contracts` section of vague nouns. Contracts are things that *break*:
  "X must happen before Y or the descriptor set is stale."

## 7. Validate before you finish (required)

Run this on your unit and iterate until it prints `PASS`:

```bash
python site/tools/check_unit.py <module>/<id>
```

It verifies citations resolve uniquely, math is explained, no filler, frontmatter
is right, figures exist. **Do not run `generate_tree.py`** — the controller runs
the site build once, centrally; running it in parallel corrupts output.

You are done when `check_unit.py` prints PASS **and** every claim you made
survives a hostile reader with `grep`.
