---
module: systems
id: status
title: Status discipline
standard: v2
---

## The promise a status file has to survive

Every engine has a document that says what works, and almost all of them are
written by the person who wrote the feature, on the day they wrote it, from the
diff. OHAO's `STATUS.md` names that failure mode and its countermeasure in its
own header: the file is updated by rendering the examples and looking at the
pixels, not by reading commit messages.

{{cite STATUS.md "Updated by running the"}}

The way to test a promise like that is to look at what vocabulary the document
has for failure. A status page that can only emit ✅ is not measuring anything.
This one declares five states and spends four: ✅ on most rows, ⚠️ on two, 🗑️ on
two, 🧪 on one.

{{cite STATUS.md "Legend: ✅ works · ⚠️ works with caveats"}}

Declared and used vocabulary diverge in both directions. ❌ never appears below
the legend line — the strongest admission the document reserves for itself has
never been cashed. And the turntable row emits a ✅ / ❓ pair, ❓ being an
undeclared sixth state glossed only by the row's own note that the interactive
half needs a display.

{{cite STATUS.md "| **Turntable / interactive** |"}}

The 🗑️ row against the OptiX denoiser reports a completed removal, not a plan.

{{cite STATUS.md "Removed; `optix`"}}

Every OptiX source file is gone from the tree; what survives is one CLI branch
that prints a demotion notice and falls through to OIDN. That row's claim is
literally the assertion in a unit test: `optix` and `OptiX` both parse to
`DenoiseMode::OIDN`.

{{cite ohao/render/rt/denoise/denoise_types.cpp "is no longer supported — falling back to OIDN"}}

{{cite tests/renderer/denoise_parse_test.cpp "TEST(DenoiseTypes, OptixRemovedFallsBackToOidn)"}}

The subsurface-scattering row is sharper, because nothing external forced it: it
marks OHAO's own SSS 🧪 and calls it biased look-dev rather than a BSSRDF.

{{cite STATUS.md "Biased look-dev hacks; not a true BSSRDF"}}

The limitations list below the matrix restates that as an instruction — keep SSS
out of ground-truth claims — voluntarily disqualifying a shipped feature from
the offline-reference standing the rest of the engine rests on.

{{cite STATUS.md "keep out of"}}

## The gate that makes a status line cost something

Prose is free. The part of this system with teeth is a golden-image regression
gate: a two-scene manifest, a Python harness, and a `pre-push` hook that renders
both scenes on the local GPU and refuses the push if either drifts. The hook
lives at that layer for a reason stated in its own header — hosted CI has no
GPU, so the only machine that can produce a verification frame is the
developer's.

{{cite .githooks/pre-push "Cloud CI cannot do this (no GPU), so the GPU safety"}}

The manifest pins both scenes at 16 spp with the denoiser explicitly disabled,
and carries the contract that the corpus grows with every fixed bug:

{{cite tests/golden/manifest.json "./build/cornell_box {out} 16 --denoise=none"}}

{{cite tests/golden/manifest.json "Denoiser OFF (we pin raw deterministic beauty). Grow this list as features land — every fixed bug should add a scene so it can"}}

Turning OIDN off is not a convenience. A denoiser is a large, version-dependent
nonlinearity between the integrator and the PNG; leaving it in the loop would
mean a golden failure could be an OIDN upgrade rather than a renderer
regression, and the gate would train the team to ignore it.

## Why the comparison is a tolerance, not a hash

The obvious golden test is `sha256(render) == sha256(golden)`. A second place in
the repository reached for that invariant and dropped it: an NRD verification log
wanted a bit-exactness check, ran the same binary three times, got three
different hashes, and tracked the invariant by file size plus visual equivalence
instead.

{{cite tests/reference_scenes/custom/envlit_turntable/verification_log.md "pre-existing non-determinism at spp=1"}}

That is corroboration, not the goldens' own evidence, and the regimes differ:
those three runs were `env_demo` at **1 spp** in the NRD remodulation work, not
16-spp offline path traces of the golden scenes.

{{cite tests/reference_scenes/custom/envlit_turntable/verification_log.md "./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr /tmp/beauty_4d.png 1"}}

The harness header attributes the residual to non-associative floating-point
reduction order on the GPU, sizes it at about six pixels at one LSB on a 1080p
frame, and calls that floor irreducible.

{{cite tests/golden/render_golden.py "from non-associative reduction order"}}

:::why
Bit-exactness was rejected against a stated floor rather than for convenience:
naming the floor's size is what makes the tolerance a number someone can argue
with. What replaced it is a two-sided tolerance compare preceded by a bilinear
downscale to 640 px, which averages the per-pixel FP ghost away and keeps the
committed goldens at 640×360 instead of shipping two 1080p PNGs.

{{cite tests/golden/render_golden.py "DOWNSCALE_WIDTH = 640"}}
:::

## Two conditions, because either one alone is blind

A scene passes only when both bounds hold. Let $A$ and $G$ be candidate and
golden after the downscale, indexed by pixel $p \in \{1..N\}$ and channel
$c \in \{R,G,B\}$, with $N$ the downscaled pixel count, and let $\tau$, $\phi$ be
that scene's two tolerances:

$$\max_{p,c}\bigl|A_{pc}-G_{pc}\bigr| \le \tau \quad\wedge\quad \frac{1}{N}\Bigl|\{\,p \;:\; \exists c,\; A_{pc} \neq G_{pc}\,\}\Bigr| \le \phi$$

{{cite tests/golden/render_golden.py "def verdict(s, max_abs_diff, max_diff_frac):"}}

$\tau$ and $\phi$ are per-scene manifest fields, not constants of the gate: the
harness reads `max_abs_diff` and `max_diff_frac` off each scene entry, falling
back to `DEFAULT_MAX_ABS_DIFF = 4` / `DEFAULT_MAX_DIFF_FRAC = 0.01` only where a
scene omits them. Both committed scenes set them explicitly, to exactly those
defaults — so a scene added later with looser numbers would be held to a looser
contract than the one written here.

{{cite tests/golden/render_golden.py "DEFAULT_MAX_ABS_DIFF)"}}

The first is an $L^\infty$ bound: it catches a small region that moved a lot — a
broken material lookup, a flipped normal, a dropped light. The second bounds the
*support* of the difference: it catches a whole-image shift of a single LSB — a
tonemap constant nudged, a different RNG stream — which the max test can never
see, because 1 ≤ 4. Each closes the other's blind spot. Both committed goldens
are 640×360, so $N = 230{,}400$ and today's $\phi = 10^{-2}$ tolerates roughly
2,300 differing pixels and no more.

The harness also ships a `--selftest` mode that renders each scene twice and
compares the two renders to each other instead of to the golden. That is not
redundant: it measures the gate's own noise floor, which is the only way to tell
a tolerance that is too tight from a renderer that genuinely regressed.

{{cite tests/golden/render_golden.py "render a second time, compare the two renders to each other"}}

## Where the net is soft

- The hook is opt-in per clone — it runs only after someone types
  `git config core.hooksPath .githooks`. A fresh clone has no gate at all.
  {{cite .githooks/pre-push "git config core.hooksPath .githooks"}}
- It fails open. If `build/cornell_box` is missing or not executable (a docs-only
  checkout, or a build that just broke) the hook prints a notice and exits 0, so
  an unverified push is indistinguishable from a verified one.
  {{cite .githooks/pre-push "skipping golden checks."}}
- The corpus is still the two scenes it started with, against a manifest asking
  for one per fixed bug. The NRD YCoCg-packing bug and the deferred black-metal
  bug are both written up in `STATUS.md`; neither has a golden scene, so both can
  return silently.
  {{cite STATUS.md "REBLUR expects **YCoCg + normalized hit-distance**"}}
  {{cite STATUS.md "Deferred metals pure black (partial)"}}
- The gtest suites under `tests/` are not part of the gate. Most of them build by
  default, but nothing invokes them unprompted: the hook's only command is the
  golden harness, and the repository's single GitHub workflow publishes this
  site. They defend code — `parseDenoiseMode`, the environment CDF, the Sobol
  sequence — only when a human runs the binaries.
  {{cite CMakeLists.txt "option(BUILD_RENDERER_TESTS"}}
  {{cite .githooks/pre-push "python3 tests/golden/render_golden.py"}}
- `STATUS.md` lists the missing GPU-less cloud workflow as an open limitation
  rather than passing the pre-push hook off as CI.
  {{cite STATUS.md "**Cloud CI** — still no GPU-less build/unit workflow"}}

## Two bug writeups, and what a refactor did to their pointers

`docs/bugs_solved/` is the older half of the discipline: numbered writeups with
symptom, root cause, fix, files touched, verification. Two entries exist, and
both describe bugs whose *fixes are still load-bearing* and whose *file paths are
all wrong*. Bug 001 places its fix in `src/renderer/frame/frame_resources.hpp`:

{{cite docs/bugs_solved/001_staging_buffer_resize.md "src/renderer/frame/frame_resources.hpp"}}

There is no `src/` in this tree. The renovation moved everything under `ohao/`,
and `resizeStagingBuffers` — the method that stops per-frame staging buffers
from staying 64×64 after a resize to 1080p, which is what produced the original
viewport flicker — is alive at a different address entirely.

{{cite ohao/render/frame/frame_resources.cpp "bool FrameResourceManager::resizeStagingBuffers"}}

Bug 002 fared the same way. Its conclusion was that every pass touching shared
uniform data must bind the *same* per-frame descriptor set, because mixing a
per-frame set with the legacy one renders the shadow map with light matrix A and
samples it with matrix B — the reported symptom was shadows rotated 90°. The
parameterised signature that enforces it survives, still called from both the
per-frame and the legacy path:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "renderShadowPass(cmd, frame.descriptorSet);"}}

So the archive's invariants outlived the refactor and its pointers did not. That
is the general shape of rot here, and it is not confined to the bug log:
`CLAUDE.md` still documents a Skeletal Animation module with a file map, while
`STATUS.md` marks skeletal animation 🗑️ and `ohao/animation/` does not exist.

{{cite CLAUDE.md "## Skeletal Animation (Module D)"}}

:::key
`STATUS.md` is trustworthy in proportion to the gate behind it, not in proportion
to its own confidence. Two of its fourteen matrix rows are covered by the golden
gate — the repository's only check wired to run unbidden, and then only in clones
that installed the hook. One more, the removed OptiX backend, is asserted by a
unit test nothing runs automatically. Of the eleven rows with no gate at all, six
still read as a plain ✅. And the file does not say when a human last looked: its
freshness marker is a list of milestones, not a date.

{{cite STATUS.md "**Last verified:** after C++20"}}

OHAO has a determinism contract on two frames and a disclosure habit everywhere
else — and the disclosure habit is worth more than it sounds, precisely because
it names what it cannot verify.
:::

## Contracts

- Regenerating with `--update` overwrites the golden without comparing first.
  Nothing distinguishes an intentional refresh from laundering a regression;
  that judgement is entirely on the human running it.
- The compare is aspect-normalised, not resolution-normalised: the downscale runs
  on both sides, fixes width at 640 and derives height, so 4K, 1080p and 720p all
  land on the same 640×360 grid. A regression confined to sub-640 detail — fine
  specular aliasing, a one-texel UV shift — is averaged away, and a change to
  `model_viewer`'s default resolution trips the size check only if it changes the
  aspect ratio or falls below 640 px wide. That default is pinned at 1080p by a
  VRAM constraint recorded at the decision point.
  {{cite examples/model_viewer.cpp "at 4K OOMs on 8 GiB cards"}}
- Manifest commands are not shell strings. They are `shlex`-tokenised into an
  argv vector and spawned with no shell, so a pipe, glob, redirect or `$VAR`
  written into a manifest command reaches the binary as a literal argument.
  {{cite tests/golden/render_golden.py "subprocess.run(shlex.split(cmd)"}}
- Nor does the harness ever `chdir`: the manifest's relative `./build/cornell_box` and
  `tests/golden/*.png` resolve against the caller's cwd, and repo-root comes from
  the hook `cd`-ing to the worktree top first. Invoke the harness by hand from
  anywhere else and the missing executable escapes `render()` as an uncaught
  `FileNotFoundError`, not a scene FAIL.
  {{cite .githooks/pre-push "git rev-parse --show-toplevel"}}
- No `STATUS.md` claim outside the two golden scenes is guarded by anything that
  runs unprompted. Read ✅ on those rows as exactly what the header promises —
  someone ran the example and looked at the pixels — and note that it promises
  nothing about when.
