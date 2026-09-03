# `ohao_diff` — the differentiable renderer

Path Replay Backpropagation (Vicini, Speierer & Jakob 2021) on a wavefront
ray-query integrator, with the interior/boundary split of the design spec as
the top-level structure.

Design: [`docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md`](../../docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md)
Stage plans: `docs/superpowers/plans/2026-08-2*-diff-renderer-*.md` and `2026-09-0*`

---

## State, in one paragraph

Stages 0 through 3 of the spec's staging table are built and gated: forward
parity with the path tracer, interior gradients for albedo / roughness /
metallic / emission / textures, the optimisation loop (L2 loss, Adam,
multi-view batching), and the boundary term (edge adjacency, silhouette
marking, vertex gradients, geometry recovery). **Nothing is wired into the
engine yet** — `ohao_diff` is linked by the two test targets and by nothing
else, so every gate runs against geometry the test harness builds itself.
Closing that is the next piece of work, and everything the spec calls an
application track sits behind it.

---

## Building and running

The module builds as part of the normal configure; the tests are behind
`BUILD_DIFF_TESTS` (default `ON`).

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Two binaries. On Linux they land in `build/`; on Windows (a multi-config
generator) in `build/Release/`.

| Binary | Needs a GPU | What it is |
|---|---|---|
| `diff_unit_tests` | no (bare-instance device queries only) | GoogleTest, 58 tests. CPU-side logic: edge adjacency, the silhouette invariant, the boundary integrand's closed form, the parameterisation and projection pullbacks. |
| `diff_gpu_probe` | **yes** | A standalone executable, 69 numbered checks, exit 0 or 1. Every GPU claim in the module is one of these. |

`diff_gpu_probe` requires `VK_KHR_ray_query` and
`shaderBufferFloat32AtomicAdd`; it checks both at init and says so if they
are missing. Shaders are compiled by the `shaders` target via `glslc`, which
CMake locates with `find_program` — the Vulkan SDK must be on `PATH`.

### The regression tool

`tests/diff/tools/probe_normalise.py` compares two probe runs, masking only
the values that come out of a float `atomicAdd` accumulator (the probe is not
bit-deterministic; everything else is).

```bash
./build/diff_gpu_probe > after.txt
python tests/diff/tools/probe_normalise.py before.txt after.txt
```

**Run `--selfcheck` after adding any check that prints a gradient or a loss.**
It runs the comparison across several runs of one unmodified binary, where
every difference is by definition a mask that is missing:

```bash
for i in 1 2 3 4; do ./build/diff_gpu_probe > run$i.txt; done
python tests/diff/tools/probe_normalise.py --selfcheck run1.txt run2.txt run3.txt run4.txt
```

Use four or more runs, not two. The masks have gone stale three times, and
the last time they had passed `--selfcheck` on three runs before a fourth
disagreed — agreeing on a given day is not evidence.

---

## Layout

```
ohao/diff/
  param/      ParamRegistry — the central abstraction (spec 4.2)
  grad/       GradientArena, arena layout; both terms sum into one arena
  geom/       edge adjacency, silhouette set, boundary integrand,
              vertex parameterisation, pinhole projection
  rng/        PathRng — the seed invariant (spec 4.5)
  device_caps.{hpp,cpp}   the float-atomics / ray-query capability check
  wavefront/  compute pipeline, stages, buffers, the fused loop
shaders/diff/ 14 compute shaders: the forward and replay traversals, the
              BSDF adjoints, loss, Adam, silhouette marking, boundary sampling
tests/diff/   diff_unit_tests.cpp, diff_gpu_probe.cpp, probe/, context/
```

The chain a geometry gradient travels, each arrow a separate object with its
own finite-difference oracle:

```
theta --[parameterisation]--> world --[projection]--> screen
dL/dtheta <--[pullback]-- dL/dworld <--[pullback]-- dL/dscreen
```

The boundary pass sits at the right-hand end and knows about none of it: it
is handed screen positions and returns screen gradients. That is what makes
a different parameterisation (Laplacian, iso-surface) a substitution rather
than a rewrite.

---

## What is not done

In the order it probably matters.

1. **No engine integration.** `ohao_diff` is linked by `diff_unit_tests` and
   `diff_gpu_probe` and by nothing else. Spec 4.3 says the optimiser writes
   *live engine resources*; today it writes probe-owned buffers. Nothing in
   the renderer, the GDExtension or GDScript can start an optimisation.

2. **The three application tracks of spec 10 are untouched** — and the spec
   says all three land at stage 2. Renderer fitting (10.1, "the distinctive
   one"): register the deferred pipeline's hand-tuned knobs as `DiffParam`s
   and fit them against the path tracer. Sensitivity maps (10.2). SVBRDF
   rewritten as a client (10.3). All three are blocked only by (1) — they
   need no new architecture, only the registry with different parameters
   registered.

3. **Gate 4, the Mitsuba 3 oracle, has never been run**, at any stage. It was
   deferred each time on the grounds that the closed-form edge test is the
   stronger oracle, which is true, but stage 1's gate as written is "gates
   1–4".

4. **The boundary term's radiances are not traced.** Each side is an affine
   field of screen position, so the jump varies along an edge and is
   integrated exactly; a traced radiance is not affine, so the two-point
   exactness becomes a two-point quadrature with a truncation term. That
   wants a convergence-order gate rather than a fixed tolerance — checks
   46–49 are the pattern.

5. **Stage 4 entirely**: warped-area reparameterisation, edge importance
   sampling (Li et al. 2018), camera and pose parameters, and unifying the
   BSDF includes with the production path tracer.

6. **Laplacian preconditioning** (Nicolet et al. 2021), which the spec calls
   close to mandatory for usable geometry optimisation. The parameterisation
   layer that makes it a drop-in exists; only the affine one is written.

---

## Conventions worth keeping

These were paid for, mostly by defects that survived a check that looked
rigorous.

- **A check whose expected value derives from the same source as the measured
  value cannot fail.** The boundary term's sign error was invisible to a
  sampled estimator that shared its minus sign; only a supersampled oracle
  with no edge, no chord and no normal in it disagreed.
- **An invariant is only as strong as the case it is evaluated on.** A
  conservation test could not fail because its single chord was symmetric
  about the segment midpoint, where the integrated weight and a flat
  half-share happen to be the same number.
- **Pre-register the criterion** — parameter, `theta*`, `theta_0`, iteration
  count, tolerance — before the first run, and say so in the check's output.
  Retuning a tolerance after seeing the number is the tell.
- **Give each gate a control that must fail.** Several checks run a
  deliberately broken variant beside the real one and assert it misses; a gate
  with no control cannot distinguish "correct" from "insensitive".
- **Say in the check's own output what it cannot see.** Check 62 states that a
  magnitude error in its Jacobian is invisible to it, and names the unit test
  that does catch one.
