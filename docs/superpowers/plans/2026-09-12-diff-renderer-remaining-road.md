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

### 5. Gate 4 — the Mitsuba 3 oracle (spec §8.4) — ≈ ½ stage

**This is owed.** Stage 1's gate as written is "Gates 1–4", and Gate 4 has never
been run at any stage. It was deferred each time on the grounds that the
closed-form edge test is the stronger oracle, which is true and remains true —
but "stronger oracle exists" is a reason to *order* it later, not to skip it.

**Gate:** agreement with Mitsuba 3 on a scene both can express, to a tolerance
pre-registered before the first comparison.

Small code — `pip install mitsuba` plus a script — and **fiddly reconciliation**.
Coordinate handedness, units, the environment map's phase, and RNG differences
make a first-run disagreement the norm rather than the exception. Budget the
time for convention archaeology, not for the script. Expect to discover that one
of the two is right and to have to prove which.

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
