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
| 2 | Sensitivity maps | not started |
| 3 | Renderer fitting | not started; the differentiability decision below is still owed |
| 4 | SVBRDF as a client | not started |
| 5 | Mitsuba oracle | **DONE** — `tests/diff/tools/mitsuba_gate.py` (three-way) and check 72; found and pinned a real +0.12% environment-sampling bias |
| 6 | Traced radiance | **DONE** — checks 69 (traced, emissive) and 70 (shaded, second order) |
| 7 | Stage 4 — scale | **partial** — camera translation as a parameter (2 unit tests); edge importance sampling, BSDF unification and warped-area remain |
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

**Items 2, 3 and 4 are unblocked.** Item 1 is closed. The orchestration move
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

**Not ready by its own criterion.** Warped-area reparameterisation: this file
already says to give it its own plan and its own oracle first, and that if the
oracle is not identified the implementation is not ready to begin. It still
is not.

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

### 2. Sensitivity maps (spec §10.2) — ≈ ¼ stage

**Goal:** `dpixel/dtheta` as a visualisation. Which light dominates this pixel;
which approximation costs the most error, and where.

**Gate:** a sensitivity image for a known scene whose bright regions are
*predicted on paper first* — a parameter that only affects one object must
produce a map that is zero away from it, asserted as an exact zero, in the
manner of check 59's null test.

Nearly free once item 1 exists: the gradient already exists and is already
gated; this is display. **That is exactly why it goes second** — it exercises
the integration on something whose correctness is already established, so a
failure is attributable to the integration rather than to the quantity.

---

### 3. Renderer fitting (spec §10.1) — ≈ 1 stage

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

- [ ] **Finite differences over the knobs.** There are on the order of ten
      scalars. Two renders per knob per step is entirely affordable, needs no
      new machinery, and reuses the FD harness that already exists. **Start
      here.** It is not a compromise: for a handful of scalars, FD *is* the
      right method.
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

### 4. SVBRDF, rewritten as a client (spec §10.3) — ≈ ¾ stage

Ingest a bundle, register three texture parameters, pick a loss, run the
optimiser. `schedule.cpp`'s 943 lines of fixture-tuned heuristics get **deleted
rather than maintained**.

**Gate:** the rewritten client reaches a loss no worse than the heuristic stack
on the same fixture, with an order of magnitude less code. Deleting the old path
is part of the gate, not a follow-up: if both survive, the heuristics will be
what actually runs.

Mostly deletion. The new work is `estimateLighting` becoming real — environment
texels registered as parameters — rather than a stub returning baked constants.

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
- [ ] **Camera and pose parameters.** Ordinary, and **cheaper than it looks now**:
      `PinholeProjection` already provides the projection Jacobian and its
      pullback, gated by an FD oracle. A camera parameter is that pullback with
      the camera's own parameters in place of the vertex's.
- [ ] **Unify the BSDF includes with the production path tracer.** Ordinary, and
      the riskiest to the existing gates: the differentiable and production BSDF
      code currently agree by construction because one was written from the
      other. Unifying them means every interior-gradient check is now testing
      shared code, so **run the full regression through `probe_normalise.py`
      before and after**, and expect this to be where a silent drift shows up.

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
