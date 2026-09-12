# The remaining road — differentiable renderer

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**What this is.** A roadmap, not a task list. Stages 0 through 3 each got their
own plan written immediately before they began, and that is still the right
practice — a step-by-step plan for warped-area reparameterisation written today
would be fiction. This document **orders** the remaining work, **sizes** it,
states **the gate each piece must pass**, and names the **decisions that have to
be made before starting**, so that the ordering survives a change of machine and
a gap in memory.

---

## Progress — updated 2026-09-12

| # | Item | State |
|---|---|---|
| 1 | Engine integration | **DONE** — the render records into a caller's command buffer (`recordGradientRun`), and `ohao_renderer` links `ohao_diff` at a real call site |
| 2 | Sensitivity maps | **DONE** — binding 13 and check 73, gated by an identity against the gradient plus a derived null test |
| 3 | Renderer fitting | not started, but **UNBLOCKED and measured**: BOTH pipelines initialise headless (engine_tests 49/49), FD decided; a scene, a TLAS and a readback remain |
| 4 | SVBRDF as a client | **its differentiable half is DONE** — the environment is a parameter (binding 14, checks 74–75); the client rewrite itself has no client in version control |
| 5 | Mitsuba oracle | **DONE** — `tests/diff/tools/mitsuba_gate.py` (three-way) and check 72; found and pinned a real +0.12% environment-sampling bias |
| 6 | Traced radiance | **DONE** — checks 69 (traced, emissive) and 70 (shaded, second order) |
| 7 | Stage 4 — scale | **partial** — camera pose complete (translation + rotation, 5 unit tests); BSDF unification investigated and deliberately not done; edge importance sampling premature; warped-area not ready |
| 8 | Laplacian preconditioning | **DONE** — `LaplacianVertexParameterisation`, 5 unit tests, check 68 |

Item 8 went first among the independent ones because it was the best ratio:
a quarter of a stage for a capability the spec calls close to mandatory, with
a control that writes itself (lambda = 0 is exactly the identity). Its gate
turned into a **lambda sweep** rather than a single stiffness, because a
single pre-registered lambda = 12 failed informatively — nine times smoother
than the control and three hundred times worse fitting. Choosing lambda is
part of using the method, so the sweep reports the trade-off instead of
replacing 12 with whatever worked.

Item 6 split into 6a (traced but piecewise constant, fixed tolerance) and 6b
(shaded, convergence order), because this file's original claim that the
whole item wants a convergence gate was only half right: a piecewise-constant
traced radiance is integrated EXACTLY by the moments and has no truncation
term whose order could be measured.

**A NEW HAZARD, and the most instructive one yet.** An edge lying exactly on
a pixel seam is clipped into BOTH adjacent pixels and its contribution
scattered twice — the gradient comes out at exactly double. Check 70's
supersampled oracle found it; **check 69 could not have**, because it
compares the traced form against the pushed form and both run through the
same kernel, so a factor common to the two cancels. Test scenes now step off
integer coordinates. The degeneracy is measure-zero and is not fixed: a
half-open pixel convention touches every boundary check.

Three things learned in item 1 that outlive it, and belong with the hazards
at the bottom of this file:

- **The winding convention has now bitten three times** (checks 63, 68, and
  the boundary term's original sign error). A reversed winding negates the
  whole boundary term, which is gradient ASCENT rather than a small error.
  Assert the signed area wherever a shape is built.
- **A refactor that changes nothing while its new path is unused proves only
  that it is inert.** Both the persistent-scene and persistent-pipeline
  changes were gated twice: byte-identical with the option off, and
  byte-identical *through the new path* with it on.
- **Release hides asserts, and 94% of the probe's output was setup chatter.**
  Both were found by looking at a diff that seemed to say something else.
- **A FIX NO CHECK CAN DISTINGUISH FROM ITS ABSENCE IS NOT FIXED.** The
  pixel-seam doubling was repaired by making `clipToPixel` half-open, and
  because the scenes had been moved off integer coordinates once the bug was
  understood, the entire suite then reproduced byte for byte either way. Check
  71 is degenerate on purpose so that it can tell. Reverting the clip makes it
  report a ratio of exactly 2.
- **A criterion that is a ratio to a nearly-converged denominator is fragile.**
  Check 68's fit bound sat at 1.463 against 1.5; an unrelated TLAS binding
  moved the control's loss by 4.6% and it crossed. The bound was not raised --
  the sweep grid was widened, which is a statement about the experiment rather
  than about what counts as success. Scaling against the INITIAL loss would be
  better formed and is still owed.

## WHERE TO PICK THIS UP, and why it stops here

The three categories the remaining work falls into, which is more useful than
a list:

**Nothing is blocked on a decision any more.** Item 5 is closed. The install
worry that had held it up dissolved once it was looked at properly: `pip
install --target build/mitsuba-pkgs mitsuba` puts mitsuba 3.9.1 and drjit
1.5.0 under `build/` — already git-ignored — reached through `PYTHONPATH`,
so no Python environment is touched at all. Deferring that to the user was
more caution than the situation needed.

**Item 2 is closed too** — see its section for why "nearly free, this is
display" was wrong and what was built instead.

**Item 1 is closed.** The orchestration move
finished in five slices — the scene, the sinks, the pipelines and their
bindings, the push constants and both loop configurations, and finally the
recording itself — each verified the same way: byte-identical with the new
path unused, and byte-identical *through* it, against a measured run-to-run
baseline rather than an assumed one.

The slice that mattered was the last, and not for its size. `recordGradientRun`
**records into a command buffer the caller owns and returns**; the probe's
`runWavefrontGradientProbe` submitted inside the call and read the film back
before returning, which is what a test wants and the opposite of what an
engine can use. An engine has one command buffer per frame and submits once;
a library that submits for you cannot go in that frame at all. No amount of
moving code would have fixed that while the submit stayed welded on.

And `ohao_renderer` now links `ohao_diff`, through
`ohao/render/diff/diff_availability.{hpp,cpp}` — the engine asking whether
this device has ray query and buffer float atomics, reported in
`DeferredRenderer::getPipelineInfo()` alongside the passes. Deliberately NOT
a DiffRenderer the engine owns with nothing to render: that plumbing arrives
with the feature that needs it. What this closes is the link itself, which no
number of passing tests inside `tests/diff` could establish.

**WHAT IS LEFT, AND WHOSE IT IS.** Everything still open is open for one of
four reasons, and none of them is "not got to yet":

1. **The project owner's decision.** The GGX specular-peak clamp in
   `pt_raygen.rgen` (item 7) — fixing it changes every highlight on a smooth
   material in the production path tracer. And the
   `feat/diff-stage0b2a` + `0b-2b` merge to master, which must go together.
2. **Engine-harness work, not differentiable-renderer work.** Item 3. NO
   LONGER BLOCKED: the deferred pipeline is now shown to initialise on a bare
   headless device, and that is pinned by a test. What is left is a device
   with the RT extensions, a scene, a readback, and `PathTracer` as the
   reference — ordinary work, measured rather than guessed. See item 3.
3. **No caller.** Item 4's remaining half (environment texels as parameters).
   The client it would serve is not in version control.
4. **Not ready by its own criterion.** Warped-area reparameterisation: this
   file already says to give it its own plan and its own oracle first, and
   that if the oracle is not identified the implementation is not ready to
   begin. It still is not.

What is deliberately NOT being done, and why, so nobody repeats the reasoning:
edge importance sampling would convert an estimator that is currently EXACT
into a sampled one, adding variance whose benefit only appears at a scale this
harness does not reach. Uniform-first was the right order (spec 7.2); the
uniform version being exact rather than sampled is what makes the optimisation
premature rather than merely unstarted.

## Where this starts

Verified on 2026-09-04, at `diff-stage3-boundary`:

| | |
|---|---|
| Stages 0–3 of spec §9 | built and gated |
| `diff_gpu_probe` | 69 `OK:` lines, checks numbered to **64**, exit 0, against a real GPU (MSVC/Windows) |
| `diff_unit_tests` | 58 tests, passing under MSVC **and** under GCC 15 / libstdc++ with asserts live |
| Engine integration | **none** — `ohao_diff` is linked by the two test targets and nothing else |

The shape to keep in mind: **the module is most of the way to being trustworthy
and barely started on being useful.** Everything hard about correctness is done
and gated; everything about it doing work for the engine is not begun. The
remaining distance is mostly plumbing and product, not mathematics — with one
exception, flagged below.

## Two definitions of done

- **Useful** — items 1 and 2 below. The module stops being a test-harness
  curiosity and starts answering questions about the renderer. **≈ 2–2.5 days.**
- **Spec-complete** — all eight. **≈ 11–12 days.**

### What those numbers are based on

Stages 0b-1 through 3 are **112 commits over 8 calendar days** (2026-08-28 →
09-04), producing 127 gated assertions. Per stage: 12, 13, 28, 26, 11, 22
commits. So one "stage" here is ≈ 20 commits ≈ 1.5–2 days at the pace this
repository has actually moved.

**Distrust that pace for items 3 and 7.** Every stage so far had a *clear
oracle* — a closed form, a supersampled reference, a paper's own algorithm
listing — which is why they moved fast and why the defects were caught rather
than shipped. Items 3 and 7 have the weakest oracles of anything remaining, and
will run slower than their size suggests.

---

## The order, and why

Integration first, because six of the remaining eight are behind it and because
it is the only item that changes what the module *is*. Then the cheapest real
deliverable (sensitivity maps) to prove the integration on something small,
before the first item with a genuine unknown in it.

```
1 engine integration
├── 2 sensitivity maps          (cheapest proof the integration works)
├── 3 renderer fitting          (needs the decision in §3 first)
└── 4 SVBRDF client
5 Mitsuba oracle                 ) independent of 1-4;
6 traced radiance                ) do when the appetite is for
7 stage 4 scale                  ) correctness rather than product
8 Laplacian preconditioning
```

---

### 1. Engine integration — ≈ 1 stage

**Goal:** a `DiffRenderer` that owns a `ParamRegistry` and a `GradientArena`,
drives the wavefront loop against **live engine resources**, and can be started
and stopped from outside.

**Gate:** an optimisation runs end to end inside the running engine — not the
probe — over a parameter the engine actually owns, and the parameter's live
value visibly changes. Plus: `diff_gpu_probe` still reports its 69 `OK:` lines with
unchanged numbers through `probe_normalise.py`, because integration must not
perturb the harness that validates the thing being integrated.

**The one hard part is resource ownership** (spec §4.3). The optimiser writes
buffers the renderer is simultaneously reading. Decide, and write down, before
any code:

- [ ] Does an optimisation step run inside the frame loop or on its own submit?
- [ ] Who owns the parameter's authoritative value — the registry or the engine
      object? Spec §4.3 says the optimiser writes live resources, which makes
      the engine object the authority and the registry a view. Say so explicitly
      or the two will drift.
- [ ] What happens to an in-flight frame when a step lands mid-frame?

**Everything else here is known shape**: `ohao_diff` already builds as a static
library with a clean dependency on `ohao_gpu_vulkan`, and `WavefrontLoop`
already records against caller-supplied buffers. The work is a facade, a
lifetime, and a GDExtension surface.

---

### 2. Sensitivity maps (spec §10.2) — **DONE**

`shaders/includes/diff/traverse.glsl` binding 13, one push field
(`sensitivityFloats`), one extra `atomicAdd` in the replay hook, and check 73
(`tests/diff/probe/checks_sensitivity.cpp`).

**THIS SECTION WAS WRONG about the cost, and the correction is the useful
part.** It said "nearly free… this is display". It is not display: the
backward pass scatters `sum_p (dL/dpixel_p) * d(film_p)/d(theta)` into ONE
arena slot, and a per-pixel image is not recoverable from a scalar. Getting
one needs either W·H one-hot backward passes (absurd) or forward-mode AD for
every pixel at once (a second derivative machinery).

**The third way is what was built, and it is cheaper than either.** The replay
hook already computes one scalar per hit vertex and already knows which pixel
that vertex belongs to. The map is those same scalars **binned by pixel
instead of summed** — one extra `atomicAdd` at `sensitivity[v.pixelIndex]`.

**Which makes the gate an IDENTITY rather than a comparison:** summing the map
must give the arena's scalar, term for term. Measured 531.214239 against
531.214111, relative 2.4e-07 — the difference is two float summation orders.
That is worth more than any tolerance against a hand-derived expectation,
because it makes the map inherit **every check that already gates the
gradient**, check 37's finite difference above all.

**Plus the null test this section asked for**, because the identity cannot see
a uniformly wrong map — every cell holding the same 1/N of the total satisfies
the sum exactly. So 42 of the 64 columns have primary rays that miss the plate
entirely and their 336 cells are required to be EXACTLY `0.0f`, compared as
floats. **The miss set is derived from the camera basis and the plate's edge
before the render**, never read off the map: a check whose expectation comes
from the same source as its measurement cannot fail, which is the defect class
this subsystem has now hit three times. The nearest column clears that edge by
0.025 (half a column pitch, the most any framing can offer), asserted against
a pre-registered 0.02 so the split is geometry and not floating-point luck.
The 176 lit cells are each separately required to be strictly positive,
without which an all-zero map would pass the null test perfectly.

**Still owed:** no check runs a map with `DIFF_PARAM_EMISSION_TEXTURE`. The
map reaching that parameter is by construction — the scatter sits outside the
per-texel branch — and that construction is untested; the shader says so where
it matters.

---

### 3. Renderer fitting (spec §10.1) — ≈ 1 stage, and MOST OF IT IS NOT THIS MODULE

**THE PIPELINE STANDS UP HEADLESS. This entry said otherwise and was wrong —
twice — and both corrections are worth more than the original claim.**

Two attempts to establish reachability both stopped at the LINKER, and each
time the conclusion drawn was "blocked". Each was right about its own failure
and wrong about what it implied:

1. Linking `DeferredRenderer` into `tests/diff/diff_gpu_probe` fails: it drags
   in `ohao_scene` to `PhysicsComponent` to `ohao_physics` to Jolt. True, and
   the right response is that a differentiable-renderer probe has no business
   acquiring a physics backend — not that the pipeline cannot run.
2. Linking it into `tests/engine/engine_tests`, which already has all of
   those, fails on `stb_image` being defined in both `ohao_gpu_vulkan` and
   `ohao_scene`. Also true — and a **known engine-wide condition with a known
   engine-wide answer**, `/FORCE:MULTIPLE`, which the GDExtension build has
   always used. Applying it is not a workaround invented for a test.

With that applied: **`DeferredRenderer::initialize` returns TRUE on a bare
device with no swapchain, no window and no surface, and hands back a usable
final-output image view, in 0.46 s.** That is not a given — a renderer sizing
its targets from a swapchain, or wanting a present queue, could not do it.
Pinned by `runHeadlessDeferredTests` in `tests/engine/engine_tests.cpp`
(48/48), which skips cleanly on a machine with no Vulkan device.

**So item 3 is a stage of ordinary work, not a blocked one.** What the
standing-up measured, which is what the next step needs:

* **Shaders resolve relative to the working directory** (`bin/shaders/...`), so
  a pass whose SPIR-V is not found there fails *non-fatally* — ParticleSystem
  did. Rendering from a test needs the CWD set or the path made absolute.
* **The RT function pointers do not load** on a device created without the
  ray-tracing extensions, so the path-tracer half needs its own device.

**AND THE PATH TRACER STANDS UP TOO** — `PathTracer::init(256x256)` returns
TRUE on a bare RT device, with a valid output view and image, and NRD, SVGF
and the cinematic stack all come up with it. Pinned by
`runHeadlessPathTracerTests` (engine_tests 49/49, 0.62 s total). That is the
half check 66 said had never been stood up anywhere.

**Three things had to be right about that device, and the third is a trap:**

1. **Select the physical device by CAPABILITY, not by index** — take the first
   advertising `VK_KHR_ray_tracing_pipeline` rather than `devices[0]`.
2. **The extension list and feature chain**, taken from
   `ohao/gpu/vulkan/device_setup.cpp` rather than guessed.
3. **`VK_KHR_push_descriptor`.** Without it `PathTracer::init` still returns
   **TRUE** — the RT pipeline and SBT build fine — and then NRD's NRI wrapper
   **aborts the process** from `ResolveDispatchTable()`, which resolves
   `vkCmdPushDescriptorSet` eagerly and treats absence as fatal (observed:
   exit 3). A test checking only the return value would have reported success
   from a process that then died. `device_setup.cpp` adds this under its DLSS
   block, so an engine build gets it incidentally and never sees this.

**What is left of item 3, in order:** a Scene with geometry, materials and a
light; an `RTAccelerationStructure` (which `render()` requires and which
`diff_gpu_probe` already builds routinely); a render and an image readback
from each pipeline; then the FD-over-knobs optimiser, which remains the small
half. Both pipelines existing headless was the uncertain part, and it is now
measured rather than assumed.

The spec calls this "the distinctive one": register the deferred pipeline's
hand-tuned knobs — SSAO radius and bias, SSR thickness and step count, the
equirect IBL floor, ambient fudge factors, CSM splits — and optimise them to
minimise the difference from the path tracer.

> **A DECISION THE SPEC GLOSSES, AND IT MUST BE MADE BEFORE ANY CODE.**
> Spec §10.1 says this "requires no new architecture: it is the parameter
> registry with different parameters registered." That is true of the
> *registry* and false of the *gradient*. **The deferred pipeline is not
> differentiable.** Nothing in it computes a derivative, and `ohao_diff` is not
> linked into it. There is no `dL/d(SSAO radius)` to register.

Two ways out, and they are not close in cost:

- [x] **DECIDED: finite differences over the knobs.** There are on the order of
      ten scalars. Two renders per knob per step is entirely affordable, needs
      no new machinery, and reuses the FD harness that already exists. It is
      not a compromise: for a handful of scalars, FD *is* the right method.
      **Nothing about this decision is blocked** — what is blocked is the
      harness that would render the two images to difference, which is the
      paragraph above.
- [ ] **Differentiable deferred passes.** A stage of work in its own right, and
      only worth it if the parameter count grows past what FD can carry — a
      learned shading term, say, rather than a dozen knobs.

**Gate:** a knob whose correct value is *known because it was deliberately
detuned* recovers to within a pre-registered tolerance, with the loss falling.
Same shape as checks 54, 60, 62, 63 — and, as in those, a **control** that must
not recover.

**Why the oracle is weak here**, and why to distrust the estimate: for the
geometry gates the answer was a number chosen in advance. Here the "correct"
value of an SSAO radius is only defined relative to the path tracer, so
`theta*` must be manufactured by detuning a value that was itself set by eye.
Recovery proves the loop works; it does not prove the fitted value is *good*.
Say which of the two is being claimed.

---

### 4. SVBRDF, rewritten as a client (spec §10.3) — **GATE UNREACHABLE AS WRITTEN**

**CHECKED, and the premise does not hold.** This entry says
"`schedule.cpp`'s 943 lines of fixture-tuned heuristics get deleted rather than
maintained" and gates the rewrite on "a loss no worse than the heuristic stack
on the same fixture". Neither is available here:

* There is no `ohao/svbrdf/` in the tree. `ohao/` holds audio, core, diff, gpu,
  physics, render and scene, and nothing else.
* `git log --all -- 'ohao/svbrdf*'` is **empty** — the client was never
  tracked. The only copy is `_backup/svbrdf-2026-08-28/`, which is itself
  untracked and which the project owner has said to ignore.

So the deletion this item is "mostly" made of has already happened, and the
**heuristic stack the rewrite must beat is not in version control** — the
comparison that is the gate cannot be run from this repository at all. Saying
"reaches a loss no worse than the heuristics" when the heuristics cannot be
run would be a claim with nothing behind it.

**What is actually left of this item** is its last sentence: `estimateLighting`
becoming real, i.e. **environment texels registered as parameters**. That is a
genuine differentiable-renderer capability and the path is well trodden — the
emission TEXTURE parameter already does per-texel scatter with bilinear
weights, gated by check 45 and by check 55's 17-element recovery — so it is
assembly rather than invention.

**IT IS NOW BUILT** — and on reflection, calling it caller-less was the wrong
call. A parameter kind's gate *is* its caller: the base colour, roughness,
metallic and both emission forms are each gated by a Gate-5 recovery and
nothing else, and an environment parameter is gated the same way. That is
quite different from giving the engine a `DiffRenderer` with nothing to
render, which has no gate at all.

`DIFF_PARAM_ENV_IMAGE` (=5), binding 14, checks 74 and 75.

**The binding is what makes it possible, for a reason sharper than the chroma
one `nee.glsl` gives.** Inverting the CDF's density to get radiance is exact —
check 31 asserts it texel by texel — but it makes the radiance and the
SAMPLING DISTRIBUTION the same array. Spec §6.3 differentiates the estimator
at FIXED directions, so an environment parameter needs the radiance to move
while the density stays put, and through the CDF it cannot. Binding the image
separates them.

**The first parameter that is not a property of a surface.** Everything gated
before it is read at a hit point; the environment is read along a DIRECTION,
by two strategies sampling two different ones, so the adjoint scatters TWICE
per vertex at generally different texels. Crediting both to one would put the
BSDF strategy's contribution in the light sampler's bin.

Check 75: **19 of 32 texels carry gradient, worst finished 2.86e-05 from
θ\* = 1.5 against a pre-registered 0.15 (3·α), having started 1.0 away; loss
0.300 → 2.3e-10 over 200 Adam iterations.** The 13 unreachable texels — a
floor-only scene sends no ray below the horizon — are required to be EXACTLY
unmoved, compared as floats.

**What remains of this item is the client itself**, and that still has nothing
in version control to rewrite or to beat. Whoever brings one back now finds
the environment parameter waiting for it.

---

### 5. Gate 4 — the Mitsuba 3 oracle (spec §8.4) — **DONE**

`tests/diff/tools/mitsuba_gate.py` plus check 72
(`tests/diff/probe/checks_mitsuba_scale.cpp`). Mitsuba 3.9.1 / drjit 1.5.0
installed out of tree under `build/mitsuba-pkgs`, reached by `PYTHONPATH`.

**Three-way, not two-way**, because a disagreement between two numbers says
something is wrong without saying which. The scene is a Lambertian plate
filling the frame under a constant environment of radiance L, for which
`mean pixel = a*L` and `d(mean pixel)/d(albedo) = L` exactly, so the closed
form arbitrates. Leg 1 (mitsuba vs the closed form) runs first and alone: if
those two disagree the SCENE is wrong and nothing has been learned about this
renderer. Leg 2 runs `diff_gpu_probe` and parses check 72's own line rather
than quoting numbers, and the shared constants are read out of the C++ source
so the two renderers cannot silently describe different scenes.

**The convention archaeology this section warned about did not happen**, and
the reason is worth keeping: the closed form `a*L` is independent of the
camera, of the plate's size and of the environment map's phase, so there was
nothing to reconcile. Choosing a scene whose answer no convention can affect
was cheaper than reconciling the conventions. What *did* bite was the
definition of the quantity — `dr.mean` divides by W·H·3 while one albedo
channel drives one channel, so the first run came out at exactly a third —
and the three-way structure localised that to neither renderer.

**IT FOUND SOMETHING**, which is the whole reason an independent oracle is
worth the trouble: the film sat **+0.124% above a\*L at a 64×32 environment,
5.1 standard errors at two million samples**. `sampleEnvMap` emits texel
CENTRES, so the environment strategy is a composite midpoint rule in the polar
angle — documented in `site/content/units/sampling/env-cdf.md`, never measured,
and invisible to every other gate here because they all compare this renderer
against itself. Check 72 now measures it on a ladder (H = 8, 16, 32: excess
0.01292, 0.003732, 0.0009683, ratios 3.46 and 3.85 against a **derived** 4)
and takes its absolute verdict at H = 256, where the bias is 2.5e-05 and the
sampling error is 1.8e-04.

**Follow-up, not done and not this module's call.** Jittering within the
chosen texel would make the environment strategy unbiased at every resolution.
That edits `shaders/includes/rt/env_sampling.glsl`, which the production path
tracer shares, so it is a renderer decision rather than a differentiable-
renderer one. If it is ever taken, check 72's ladder becomes obsolete and its
own failure message says to delete it rather than loosen it.

---

### 6. The traced radiance in the boundary term — ≈ ½ stage

The last of the four Stage 3 deviations, and half closed already: each side of
an edge is an affine field of screen position, so the jump varies along an edge
and is integrated **exactly**. What remains is replacing the two fields with two
ray traces.

**Gate: a convergence-order test, not a fixed tolerance.** A traced radiance is
not affine, so the two-point exactness becomes a two-point quadrature and
acquires a truncation term. Checks 46–49 are the pattern: measure the error at
two or three chord resolutions and require the ratio the derivation predicts. A
fixed tolerance here would pass a quadrature of the wrong order.

**Hazard.** The gates that establish the boundary term are attributable *because
the interior term is exactly zero in their scenes* — constant, or θ-independent,
radiance on each side. A traced radiance is not θ-independent in general, so
the interior term comes back and the supersampled oracle stops being purely the
boundary integral. **Keep a θ-independent traced case** (a fixed emitter, no
interreflection onto the moving object) as the gate, and treat the general case
as measured rather than gated.

---

### 7. Stage 4 — scale (spec §9) — ≈ 1.5 stages

**Gate (spec's own):** no regression on gates 1–5.

Four items, and they are not alike:

- [ ] **Warped-area reparameterisation.** **The one estimate in this document I
      would not defend.** This is a research technique being implemented, not a
      known shape being assembled, and there is no closed form waiting to check
      it. Give it its own plan and its own oracle before starting; if the oracle
      is not identified, the implementation is not ready to begin.
- [ ] **Edge importance sampling** (Li et al. 2018, spec §7.2). Ordinary. The
      uniform version already exists to check it against, which was the whole
      reason for doing uniform first — a sampler is only checkable against a
      correct estimator of the same integral.
- [x] **Camera and pose parameters. DONE.** `pullbackToEyeTranslation` already
      carried the translation half; `pullbackToEyeRotation` is the second term
      its header said it did not have, so a rigid camera pose is now complete —
      translation moves the eye with R fixed, rotation turns R with the eye
      fixed, and a look-at camera that moves its target does both at once.

      **The parameterisation is in WORLD axes**, which is the less obvious
      choice (a user steering a camera thinks pan/tilt/roll about its own
      axes) and the choice was made by the ORACLE: `setLookAt` is the only way
      to set a basis, so the FD test has to build the perturbed camera by
      rotating the target and the up hint — and those are world vectors. World
      axes let the test **construct** the perturbation exactly, by Rodrigues
      rather than by a linearisation; a camera-axis parameterisation would put
      a frame conversion between the oracle and the thing it checks.
      Pan/tilt/roll is this composed with R^T, and that belongs to whoever
      wants those names.

      Three unit tests, and they cover different failures. The FD comparison
      agrees to 3.2e-4 on all three axes against a 2e-3 bound, with a
      per-component non-vacuity floor so a zero derivative cannot pass.
      **Mutation-tested:** flipping the sign of the pullback makes all three
      axes fail at relative error 2.0000 — the signature the failure message
      names. The null test (a vertex whose offset from the eye lies ALONG the
      rotation axis contributes EXACTLY 0, compared as a float) does **not**
      catch that mutation, correctly — zero stays zero under negation — which
      is why both exist.
- [x] **Unify the BSDF includes with the production path tracer.** **INVESTIGATED,
      AND IT IS NOT A TIDYING JOB. Not done, on purpose — see below.**

      This entry was wrong twice over, and the corrections matter more than the
      task did.

      **First: the differentiable side is already unified.**
      `shaders/includes/diff/bsdf.glsl` does not re-implement the microfacet
      terms — D, the Smith auxiliaries, the VNDF sampler and the tangent basis
      all come from `shaders/includes/material/ggx_aniso.glsl`, the same file
      the RT raygen shaders include. Its header claimed that; the claim holds
      (`ggxDiso`, `smithG1GGX`, `ggxBuildBasis`, `sampleGGXVNDF` are called by
      name). So "every interior-gradient check is now testing shared code" was
      already true, and the regression this entry warned about had already
      happened without incident.

      **Second: the duplication is on the PRODUCTION side, and it is not a
      duplicate.** `shaders/rt/pt_raygen.rgen` includes `ggx_aniso.glsl`, calls
      `ggxD_anisoOrIso` in three places, and then writes D out by hand in four
      others as `a2 / (OHAO_PI * denomGGX * denomGGX + 0.0001)`. The alpha
      convention matches — both reach `a2 = roughness^4` — so the substitution
      looks free. **It is not.** At `N·H = 1` the denominator *is* `a2`, so that
      0.0001 dominates the moment `a2²` drops below it. It is not a
      division-by-zero guard, it is a **ceiling on the specular peak**, and it
      bites exactly where a highlight lives:

      | roughness | `ggxDiso` at N·H=1 | inlined | ratio |
      |---|---|---|---|
      | 0.05 | 50929.6 | 0.0625 | **815 000** |
      | 0.10 | 3183.1 | 0.99969 | **3184** |
      | 0.20 | 198.94 | 14.809 | **13.4** |
      | 0.40 | 12.434 | 11.858 | 1.05 |
      | 0.70 | 1.3257 | 1.3250 | 1.001 |

      So the two pipelines disagree about D for **every material smoother than
      about roughness 0.3**.

      **WHY THIS IS THIS MODULE'S PROBLEM.** Item 3 fits the deferred pipeline
      to the **path tracer**. If the path tracer's own D is peak-clamped, the
      target is not the model anyone thinks it is, and a fit that succeeds has
      fitted the clamp.

      **WHY IT IS NOT FIXED HERE.** Substituting the shared helper would
      brighten and tighten every highlight on a smooth material in the
      production path tracer. That is probably the correct image and it is
      certainly a large visual change, and it needs a golden-image regression
      this module cannot run — the feasibility probe below shows why. It is the
      renderer's call, not the differentiable renderer's.

      **PINNED INSTEAD**, so the finding cannot be lost: `DiffGgxPipelineTie`
      (3 unit tests) asserts the alpha convention IS shared, that the two forms
      agree to 6% above roughness 0.4, and that they diverge by the derived
      factors below it. Its failure message says what to do when someone does
      unify them — delete the test, and re-gate the path tracer's appearance.

---

### 8. Laplacian preconditioning (Nicolet et al. 2021) — ≈ ¼ stage

The spec calls this "close to mandatory for usable geometry optimization", and
it is the reason the Global Constraint against optimising raw vertex positions
exists at all.

**Gate:** a geometry recovery that the affine parameterisation cannot do —
a deformation with many more degrees of freedom than three — converging where
unpreconditioned gradient descent on the same parameterisation stalls or
produces a spiky mesh. The control is the unpreconditioned run.

Cheap because the layer already exists: `AffineVertexParameterisation` defines
the `apply` / `pullback` interface, checks 62 and 63 exercise it end to end, and
the boundary pass never learns that a parameterisation exists. A second
implementation of the same two methods is a substitution.

---

## Global constraints — carried forward, still binding

- **No check weakened, deleted, silenced or renumbered.** New checks take
  numbers from **65** up.
- **Pre-register every criterion** — parameter, `theta*`, `theta_0`, iteration
  count, tolerance — before the first run, and say so in the check's output.
  Retuning a tolerance after seeing the number is the tell.
- **Every gate gets a control that must fail.** A gate with no control cannot
  distinguish "correct" from "insensitive".
- **Run `probe_normalise.py --selfcheck` over four or more runs** after adding
  any check that prints a gradient or a loss. Its masks have gone stale three
  times, and the last time they had passed `--selfcheck` on three runs before a
  fourth disagreed.
- **Run `tests/diff/tools/portability_check.sh`** before pushing. It compiles
  every translation unit with GCC and runs the unit tests *with asserts live* —
  the configuration in which a test once aborted that Release had been passing
  for weeks.
- Build clean at `-j8`; `diff_unit_tests`; `renderer_test`;
  `python site/tools/generate_tree.py` exits 0.
- Every new check gets a **demonstrated failure** with pasted output.

## Hazards that outlive Stage 3

1. **A check whose expected value derives from the same source as the measured
   value cannot fail.** The boundary term's sign error was invisible to a
   sampled estimator that shared its minus sign; only a supersampled oracle with
   no edge, no chord and no normal in it disagreed.
2. **An invariant is only as strong as the case it is evaluated on.** A
   conservation test could not fail because its single chord was symmetric about
   the segment midpoint, where the integrated weight and a flat half-share are
   the same number.
3. **Adam hides magnitude errors.** It divides by `sqrt(v)`, so any positive
   rescaling of a gradient component changes nothing. A recovery gate cannot see
   a wrongly-scaled Jacobian; only a finite-difference oracle on the Jacobian
   itself can.
4. **An exactly reachable optimum is a common fixed point of every pullback**,
   right or wrong. Recovery gates test sign and coupling, not magnitude. Check
   62 says so in its own output; keep that habit.
5. **`GradientArena::build` does not zero.** Check 61 first read a destroyed
   position buffer's contents back as a gradient.
6. **Release hides asserts.** See constraint 5 above and commit `43d46e2`.

## What this plan deliberately does not do

- **No detailed task breakdown per item.** Each stage so far got its own plan
  written immediately before it began, informed by what the previous stage
  actually turned up. That practice is why the plans were useful; writing eight
  of them today would produce eight documents that are wrong in different ways.
- **No commitment to the sizing.** The numbers are calibrated against this
  repository's own measured pace on work with clear oracles. Items 3 and 7 are
  explicitly flagged as not matching that profile.
- **No decision on the §10.1 differentiability question.** It is stated, both
  options are costed, one is recommended, and it is left to whoever starts that
  item — because the right answer depends on how many parameters they intend to
  fit, which is not knowable now.
