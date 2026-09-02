# Stage 3 — The boundary term

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** silhouette gradients. Adjacency, silhouette set, edge sampling, vertex gradients, per-iteration BLAS refit.

**Gate to pass (spec §9):** closed-form edge test + finite differences on positions + a Mitsuba silhouette scene.

---

## What Stages 1–2 leave standing

Interior gradients are complete and gated (checks 37–49), and the optimisation loop closes on them: **Gate 5 passes for a scalar and for a 36-element texture** (checks 54–55). 60 `OK:` lines, exit 0.

`ParamKind::VertexPositions` already exists in `ohao/diff/param/param_registry.hpp`, annotated `// stage 3`. Nothing constructs one.

## THE STRUCTURAL FACT THIS STAGE RESTS ON

Spec §4.1:

```
dI/dtheta  =  INTEGRAL_over_interior ( df/dtheta ) dx   +   INTEGRAL_over_boundary ( f * (v . n) ) dl
```

> **For appearance-only parameters the boundary term is exactly zero.** Albedo, roughness, and light intensity do not move discontinuities. This is not an approximation — it is mathematically absent.

That sentence is the whole reason this stage is *additive* rather than disruptive, and it hands the stage its regression gate for free, exactly as Stage 2 Task 1's all-ones seed did:

**With the boundary kernel dispatched, every check from 37 to 55 must produce its original numbers.** Their parameters have no boundary term, so a boundary kernel that changes any of them is writing where it must not. `tests/diff/tools/probe_normalise.py` is what makes that comparison possible against the arena's non-determinism, and `--selfcheck` is what keeps its masks honest — **run it after adding any check that prints a gradient or a loss**, because it has silently gone stale twice.

## Inherited hazards — read all of these before Task 1

1. **`Config::maxBounces`, the fused-loop survival argument, and the box scene.** `tests/diff/context/probe_scene.hpp`'s four `static_assert`s encode a survival induction that assumes a **closed box with a camera strictly inside**. Moving vertices breaks every one of those hypotheses. Stage 3 needs its own scene, and must not perturb the box the interior checks depend on — `wf_intersect.comp` cites those asserts by name three times.
2. **The BLAS must be refit per iteration.** `animated_rt_manager.cpp` already does GPU-skinning-then-BLAS-rebuild, which is structurally the same problem; look there before inventing a second mechanism.
3. **Detached sampling (spec §6.3) still applies to the interior term.** The boundary term is a *different* integral and does not inherit that bias, so a Stage 3 recovery failure has two candidate causes, not one. Say which.
4. **The `>1 spp` decision (Stage 2, `WavefrontLoop::Config`'s header) is 1 spp per dispatch.** Edge sampling is a **1-D** domain along silhouettes, not the pixel domain, so it does not inherit that decision — it needs its own statement about how many edge samples per dispatch and what accumulates them.

## Global Constraints

- **No check weakened, deleted, silenced or renumbered.** New checks take numbers from 56 up.
- **Vertex positions are never the parameter.** Spec §9: Stage 3 parameterises *something that produces* vertex positions. Direct positions are the trivial case; the indirection leaves room for Laplacian preconditioning (Nicolet et al. 2021) and FlexiCubes later.
- Build clean at `-j8`; `diff_unit_tests`; `renderer_test`; `python site/tools/generate_tree.py` exits 0.
- Every new check gets a **demonstrated failure** with pasted output.

---

### Task 1: `EdgeAdjacency`, built once — DONE (unit tests)

**Files:** `ohao/diff/geom/edge_adjacency.{hpp,cpp}`, `diff_unit_tests`.

Topological, CPU-side, built once per mesh ever — moving a vertex changes geometry, not connectivity (spec §7.1).

- [ ] **Step 1: Write the failing check first**, in `diff_unit_tests` because this is pure CPU logic and needs no device. On a closed box: every edge has **exactly two** adjacent faces, the edge count satisfies Euler's `V - E + F = 2`, and every edge's two faces list it with **opposite orientation**. On an open mesh: boundary edges have exactly one.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5:** Assert adjacency is **invariant under vertex motion** — build it, move every vertex, rebuild, require an identical structure. That is the claim the per-iteration cost argument rests on, and nothing else tests it.
- [ ] **Step 6: Demonstrate failure. Step 7: Commit.**

---

### Task 2: `SilhouetteSet`, recomputed per view — DONE (unit tests + check 58)

**Files:** `shaders/diff/silhouette_mark.comp`, `ohao/diff/geom/silhouette_set.{hpp,cpp}`, the probe.

A GPU pass over the adjacency list marking edges where one adjacent face is front-facing and the other is not, plus open boundary edges, then compaction.

- [ ] **Step 1: Write the failing check first.** For a convex closed mesh viewed from outside, the silhouette is a **single closed loop** — every marked edge shares each of its endpoints with exactly one other marked edge. That is a topological invariant, checkable without knowing which edges they are.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5:** Assert the count is **view-dependent** — two cameras must give different silhouette sets, or the pass is not reading the view at all and the loop invariant above would hold vacuously. (Stage 2's check 53 needed exactly this guard and caught its own first attempt failing it.)
- [ ] **Step 6: Demonstrate failure. Step 7: Commit.**

---

### Task 3: The boundary integrand, at one edge — DONE (unit tests + check 57)

**Files:** `shaders/diff/boundary_sample.comp`, the probe.

Each edge sample evaluates radiance on **both sides** of the edge (two rays); the integrand is their difference, weighted by the edge's velocity under theta, scattered into the two vertices' gradients (spec §7.2).

- [ ] **Step 1: Write the failing check first, and make it CLOSED FORM.** A straight edge crossing a pixel under a box filter has an analytic boundary integral (spec §8.2's fourth bullet). Derive it, state it, and compare — this is the one place in the stage where an oracle exists that is not another implementation.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5:** Assert the two-sided difference is **antisymmetric**: swapping which side is "inside" negates the integrand exactly. A one-sided evaluation would pass a magnitude check and fail this.
- [ ] **Step 6: Demonstrate failure. Step 7: Commit.**

---

### Task 4: Vertex gradients and the BLAS refit — DONE (checks 56, 59)

**Files:** `ohao/diff/geom/`, the wavefront loop, the probe.

- [ ] **Step 1: State the parameterisation in code.** What produces the positions, and why not the positions themselves. See the global constraint.
- [ ] **Step 2: Write the failing check first** — a finite difference on a **vertex position**, under common random numbers, exactly as check 37 does for the albedo. This is the stage's real gate.
- [ ] **Step 3: Confirm it FAILS. Step 4: Implement, refitting the BLAS per iteration. Step 5: Verify.**
- [ ] **Step 6: THE REGRESSION GATE.** With the boundary kernel dispatched, **every check 37–55 produces its original numbers** — the boundary term is exactly zero for their parameters, so anything else is the boundary kernel writing where it must not. Normalised diff against a pre-Task-4 baseline, and `--selfcheck` first.
- [ ] **Step 7: Demonstrate failure. Step 8: Commit.**

---

### Task 5: Gate — recovery of a geometric parameter — DONE (check 60)

- [ ] **Step 1: Pre-register the criterion** before running anything, as Stage 2's checks 54 and 55 do: which parameter, `theta*`, `theta_0`, iterations, and what counts as recovered.
- [ ] **Step 2: Recover.** If it fails, **attribute it** — interior bias (hazard 3), boundary integrand, or the loop. Say which, with the measurement that decided it.
- [ ] **Step 3: Demonstrate failure. Step 4: Commit.**

---

## Exit criteria

- [ ] `diff_gpu_probe` exits 0; report the `OK:` count — a description, never a target.
- [ ] **Checks 37–55 unchanged with the boundary kernel dispatched**, verified through `probe_normalise.py` after `--selfcheck`.
- [ ] Adjacency is invariant under vertex motion, demonstrated.
- [ ] The silhouette of a convex closed mesh is a single closed loop, and is view-dependent.
- [ ] The boundary integrand matches a closed form on a straight edge, and is antisymmetric under side swap.
- [ ] A vertex-position finite difference agrees with the scattered gradient.
- [ ] A geometric `theta*` recovered against a pre-registered tolerance.
- [ ] Every new check has a demonstrated failure with pasted output.

---

## Where this stands (2026-09-02)

**The gate passes.** A triangle's six vertex components recovered from a synthetic target by descending the **boundary term** — started 0.5 from `theta*` at its worst component, finished **0.000565** away against a pre-registered 0.15 (check 60).

| Piece | Where | Oracle |
|---|---|---|
| `EdgeAdjacency` | unit tests | Euler's `V-E+F=2`, two faces per edge, opposite traversal |
| `SilhouetteSet` | unit tests, check 58 | single closed loop; GPU set compared to host **exactly** |
| boundary integrand | unit tests, check 57 | **supersampled area/image derivative**, sharing no formula |
| two-vertex scatter | unit tests | `(1-u) + u = 1` against an independently computed rigid translation |
| `dJ/d(vertex)` target | check 56 | measured, with 2200 trace mismatches against 0 for every earlier parameter |
| the two passes connected | check 59 | **exact-zero null test** on off-silhouette vertices |
| Gate 5 for geometry | check 60 | pre-registered recovery, interior term exactly zero |

**Three errors this stage produced, and what caught each.** All three were invisible to the check that shared their assumptions:

1. A **sign error** in the closed-form integrand. The sampled estimator agreed with it perfectly at every sample count — they carried the same minus. Only the supersampled oracle disagreed.
2. A **3x orientation error** when summing an image: the two edges touching a moved vertex partly *cancel*, so negating one makes them *add*. Correct sign, wrong magnitude — a sign check passes it.
3. A **conservation test that could not fail**: its single edge had a chord symmetric about the segment midpoint, where the integrated `u` weight and a flat half-share are the same number.

## Deviations from this plan's own constraints — recorded, not glossed

*Closed after the stage shipped; each entry keeps what it said and adds what closed it, because a deviation quietly deleted reads as one that never happened.*

**Vertex positions ARE the parameter.** The Global Constraints above say they never should be, and check 60 optimises screen-space positions directly. Spec §9 calls direct positions "the trivial case", so this is the trivial case rather than a wrong one — but the constraint as written is not met. The mitigation is structural: the boundary pass takes a position array and does not care what produced it, so a parameterisation layer is additive rather than a rewrite. It is still owed. **CLOSED** by `AffineVertexParameterisation` (three parameters driving six components) and check 62, which recovers them through the pullback with a sign-flipped control that must not. The additive claim held: the boundary pass was not touched.

**The radiances are pushed, not traced.** The boundary pass takes `lIn`/`lOut` as push constants, so the term is exact only where radiance is constant on each side of an edge. That is the case the gate uses — and deliberately, because it is the case where the interior term is exactly zero and a recovery is therefore *attributable*. Substituting two ray traces is the next layer and everything beneath it is checked. **HALF CLOSED.** Each side is now an affine *field* of screen position, so the jump varies along every chord and the kernel integrates a moment rather than a weight times a scalar — exactly, for an affine field. The oracle survived the generalisation because the fields depend on screen position but not on θ, so the interior term is *still* exactly zero; check 64 measures it against the supersampled derivative, with the old constant form as the control it must beat. What remains is the trace itself: a traced radiance is not affine, so the two-point exactness becomes a two-point quadrature with a truncation term.

**Orthographic only.** Screen space is world xy, so no projection Jacobian enters. That derivative is real and belongs to the perspective case. **CLOSED** by `PinholeProjection` and check 63, which recovers a world-space translation *including its depth component* — recoverable only because the third Jacobian column is nonzero, with an orthographic control that leaves that parameter exactly where it started. Two things the first drafts got wrong are recorded in the check: identifiability is a property of the shape rather than of any one vertex, and a projection can reverse the winding the boundary pass's normal convention depends on.

**The boundary gradient does not reach the interior arena.** Spec §4.1 says the two contributions are "summed into the same arena"; here the boundary pass writes its own buffer. Nothing yet computes an interior *and* a boundary gradient for one parameter and adds them. **CLOSED** by check 61: an appearance block and a `VertexPositions` block in one arena, disjoint and exactly so, with all 250 other floats asserted to be exactly 0.0f. It found a real defect on the way — `GradientArena::build` does not zero, and the check first read a destroyed position buffer's contents back as a gradient.

**The regression gate the plan asked for holds** — checks 37–55 produce their original numbers, verified through `probe_normalise.py` after `--selfcheck`. That is easy here for a reason worth naming: the boundary pass never touches the interior arena, so the two cannot interfere. It will mean more once they share one. **It since did.** Check 61 put both terms in one arena, and check 64 rewrote the boundary kernel's integrand — after which every check from 37 to 63 still reproduced byte for byte, which is the first time that gate could have failed. `--selfcheck` also caught two of its own masks that had been stale by luck across three earlier runs.

## What this stage deliberately does not do

- **No hierarchical edge importance sampling.** Uniform first; Li et al. 2018 is an optimisation added only once a correct uniform version exists to check it against (spec §7.2).
- **No warped-area reparameterisation.** Stage 4.
- **No topology change.** FlexiCubes is the designated escape hatch, not this stage's problem.
- **No Mitsuba comparison yet** unless it is cheap: `pip install mitsuba` plus a script. The closed-form edge test is the stronger oracle and comes first.

## Next plan

**Stage 4 — scale.** Warped-area reparameterisation, edge importance sampling, camera/pose parameters, and unifying the BSDF includes with the production path tracer. Gate: no regression on gates 1–5.
