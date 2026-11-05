# Caustics, step 2a: manifold connections from the sun's smooth reflections

**Spec:** `docs/specs/2026-11-04-caustics-step2-design.md` ("The terms step 2 owns", "The manifold connection", "Step 2a as
designed", rulings M3, M7)
**Plan:** `docs/plans/2026-11-04-caustics-step2a.md` (tasks 1-7)
**Status:** built and tested on branch `caustics-step2`; on with caustics in `rt_offline` (`OHAO_MANIFOLD_OFF=1` turns it
off; ruling M9, Frank 2026-11-05: on, and look for ways to make it cheaper; M8: the roulette weight 0.1). Not merged.

## 1. Summary

Step 1 traced light from the sun to the camera's first hit. Sunlight reflected by a smooth surface onto a matte one the
camera reaches only after a bounce stayed with the camera side, which finds it by luck (a glossy glint) or never (a
mirror: a delta sun reflected by a lobe of alpha 1e-4). Step 2a solves for those paths. At a rough reflection at camera
depth 1 or 2, a request records the ray that met it; a separate pass retraces it and, for one smooth caster draw chosen
by its solid angle, draws a microfacet slope from GGX truncated to the lobe's core, finds every point of the draw whose
half vector between that point and the sun has that slope (Newton on each triangle a hierarchy of cones keeps), and
connects them. The camera side drops exactly those terms, so every path is counted once.

A mirror's sun spot seen in a second mirror: 0.3376 against the closed form 0.3376; without step 2a 0.0048 (the light was
missing, not noisy). A glossy mirror (roughness 0.1) against OpenPBR's own quadrature: 0.40-0.66%, and the pass's light
alone equal to the core's share of the integral.

On the showcase it does not pay: its remaining dots are glints of normal-mapped surfaces (the helmet's metal, Sponza's
own), which step 2a leaves to the camera side (ruling M7), and at equal time step 1 alone is better (section 5).

## 2. What was built

- **The core** (`shaders/material/openpbr.slang`): `evalSmoothCore` / `evalLobesSmoothCore` keep a smooth reflection lobe
  where the half vector's slope stretched by the lobe's alphas is within sqrt(`kManifoldCoreSlope2`) 99 (0.99 of GGX's
  normals) or its plain slope within 0.1 (`kManifoldCoreMinSlope2`); `smoothAlphaBounds` the largest alpha a lobe has
  while smooth. CPU twin ops 17, 18.
- **The manifold module** (`shaders/rt/manifold.slang`): the half vector's slope on a triangle in a frame of reference
  its cluster fixes, its analytic Jacobian, Newton from the centroid, GGX's slope density truncated to the core (a
  mixture for a smooth coat over a smooth base), the ring test (groups) and the disc test (clusters, sub-clusters,
  triangles) with their node terms. Its CPU twin (`tools/caustics`) and `manifold_tests`.
- **The casters' triangles** (`render/rt/manifold_casters.*`, `shaders/rt/manifold_gather.slang`, `pt.manifold.gather`):
  owned draws (a material that can be smooth, no normal map, every draw of it static, not an emitter or a gun, with
  meshlets), gathered every frame into world space a meshlet a cluster (128 triangle slots), 8 sub-clusters of 16, groups
  of 8 clusters, each node a box, a cone of shading normals, its frame and its disc term; a record a draw with its lobes'
  alpha bounds; the draws buffer with each draw's world bounding sphere and a bit word a material.
- **The camera side** (`raygenCaustics`): `chainOne`, the request write (3 float4: the ray, the throughput), the
  subtraction at an owned material's sun sample; `OHAO_PT_CLASS` widened to 8 bits (128: the pass's light).
- **The pass** (`manifoldConnect`, its own program `path_tracer_manifold`, `pt.manifold`): a thread a pixel over its two
  requests; a draw chosen by solid angle, Russian roulette on requests whose casters' summed solid angle is under
  `kManifoldRouletteWeight` 0.1; per root the sun's shadow ray first, then X -> y as the camera's bounce ray; its sum added
  to the running mean and the displayed pixel redrawn. The pipeline builder takes a program without a second raygen.
- **Tools**: `manifold_reference` (m2's quadrature through OpenPBR's CPU twin); `OHAO_MANIFOLD_OFF`,
  `OHAO_MANIFOLD_SPLIT_ONLY`, `OHAO_MANIFOLD_STAGE` (1, 2, 3: stop after the candidates, the roots, X; profiling); the
  pass's counters in the log when it ends.

## 3. Bugs found and fixed

- **A mirror's tail.** With the core at GGX's 0.99 alone, a mirror's remaining 1% sat within 0.06 degrees of the mirror
  direction: the camera side could never find it, so m1 would have read 1% dark. The core reaches a slope of 0.1 at
  least (ruling M3, recorded in the spec); a mirror's tail past it carries 5.5e-6 of its light.
- **The ring.** The first pruning tested only the half vector's angle to the normal, which every point of a ring round a
  sphere meets: 3,192 triangles solved a request. A frame of reference fixed per cluster makes a slope point one way
  across it, and the disc test keeps only the triangles near one point (99.5% of a sphere pruned, `t8_triangle_test`).
- **Strip clusters.** Clusters of 32 triangles in index order were strips round a sphere's rings; clusters are meshlets.
- **The material in the wrong field** after the draws buffer changed layout: every draw read material 0, unowned; m1
  caught it (the mirrored view read 0).
- **c4's premise.** Step 1's c4 held R seen through glass equal with caustics on and off: no term was the light
  pass's. R is lit by the mirror, and seen past the slab it is step 2a's (depth 2), which the camera side never
  found: off read 0.0332, on 0.3111. c4 now holds R to the closed form times the slab's transmittance.
- **Cost.** The pass took 340 ms a showcase frame (480 x 270) against `pt.trace`'s 21. Measured by stage: walking all six
  draws a request (to 116 ms by choosing one by solid angle), requests far from every caster (to 70 ms by roulette, to 60
  at the weight 0.1), the closest-hit trace for roots on a solid's far side (the shadow ray first). Loosening the disc
  test's asin and cos to polynomials made it 1.4x slower: it passed more nodes.

## 4. Spec corrections

In "Step 2a as designed" and the rulings: one draw a request chosen by solid angle and roulette on requests (both exact);
the core's fixed slope (M3); clusters are meshlets with a frame of reference each (M4 is now the disc test); the pass adds
to the running mean itself.

## 5. Results

`check_caustics` (gpu, 320 x 180):

| test | measured | bar |
|---|---|---|
| m1_mirror_seen, R seen directly (step 1) | 0.3375 against 0.3376 (0.05%) | 1% |
| m1_mirror_seen, R through a second mirror (step 2a) | 0.3373 against 0.3376 (0.11%); without step 2a 0.0048 | 1% |
| m2_glossy_mirror, roughness 0.1, direct / mirrored | 0.47% / 0.66% against the quadrature | 1% |
| m2: the pass's light, the camera side's | 0.3337 against the core's 0.3346; 0.0030 against the tail's 0.0044 | 1% of the whole |
| m8_repeat: two captures in one process | identical | identical |
| c4_through_glass: R lit by the mirror, seen through a clear slab (step 2a at depth 2) | 0.3111 against 0.3116 (0.19%); without caustics 0.0332 | 1% |
| c1-c3, c6 (step 1's) | as before (c1 0.07%, c3_slab 1.01%, c2 0.13% / 0.05%, c6 identical) | |

m2's direct and mirrored views both read about 0.0014 under the quadrature, which takes the mirror as infinite (its wide
tail past the real mirror's edges is missing from the render, not from the reference).

`manifold_tests` 7/7 (the Jacobian against central differences in double, every root of a 96 x 48 sphere against a
21-start search, the flat mirror's point, the slope draw's density and chi-square, the ring and disc tests never pruning a
root, 200,000 cases each); `material_tests` t7 5/5; `caustic_casters_test` 9/9.

The showcase (480 x 270, against the 8192 spp reference): the pass costs 60 ms a frame against `pt.trace`'s 21 (GPU
timings, 8 spp). At 512 spp with the pass relMSE 0.00096 and the same 7 dots as step 1; at equal time (2048 spp, step 1
alone) relMSE 0.00019 and no dot. The dots, removed one caster group at a time: two the helmet's (normal-mapped), one the
helmet and the glass ball together, one with the plinth as a rough vertex on the way, one none of the casters (Sponza's
own surfaces). The manifold program compiles cold in 4.4 s; the caustics program in about 8 minutes.

Render goldens 42/42 unchanged.

## 6. Known gaps

- **Normal-mapped glints stay with the camera side** (ruling M7): a normal map gives a triangle a root per texel's slope.
  They are the showcase's dots; they need their own step (the map's cells as Newton's domains).
- **The cost.** On by default (M9): it removes light that is missing without it (mirrors seen past the first hit), and
  costs about 3x `pt.trace` on the showcase without improving it at equal time.
- **Chains of two** (light through glass seen past the first hit): step 2b.
- **The far side of closed casters**: their roots are solved and then shadowed; skipping them needs to know a mesh is
  closed.
- **Under NRD** the pass's light goes to the running mean only, not the denoiser's guides.

## 7. How to run

```console
python tests/renderer/check_caustics.py m1_mirror_seen m2_glossy_mirror m8_repeat
build/Release/manifold_tests.exe
build/Release/manifold_reference.exe 0.1
build/Release/material_tests.exe t7_core_min_slope
```
