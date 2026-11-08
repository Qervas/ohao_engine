# Caustics, step 2b: the sun through a solid, by a seeded walk

**Spec:** `docs/specs/2026-11-04-caustics-step2-design.md` ("Step 2b as designed", ruling M12; "Tests" m4-m6)
**Status:** built and tested on branch `caustics-step2`; on with step 2 in `rt_offline` from 2026-11-06, **opt-in since
2026-11-08** (`OHAO_MANIFOLD_GLASS=1`; known gaps, "Runaway frames"); `OHAO_MANIFOLD_OFF=1` drops all of step 2. Not merged.

## 1. Summary

Sunlight that passes through a glass solid and lands on a matte surface the camera reaches after a bounce (seen in a
mirror, through a pane, through the ball itself) was rendered by nothing: the camera side's light samples look above a
surface, its shadow rays stop at solids, and no ray meets a delta sun; step 1's light pass connects only to the camera
ray's first hit. Step 2b solves for those paths. At each request step 2a already makes (a rough point X at camera depth
1 or 2), for the owned solid chosen by its solid angle, it draws a GGX slope at each of the two refractions, walks from a
seed in the cone the solid subtends to the entry and exit points whose refraction half vectors have those slopes
(Zeltner et al. 2020's seeded walk, uncertified, counted by its re-find trials), checks the path as the camera side
would trace it, and adds the sun through it. The camera side renders none of it, so nothing is split.

A slab's light seen in a mirror: 0.3840 against the closed form 0.3799 (1.09%); without step 2b 0.0031. A glass ball's
caustic seen through a periscope mirror: 0.18% of a ray simulation's energy. A floor seen through a pane under the slab:
0.59% of its closed form.

## 2. What was built

- **The walk on the GPU** (`shaders/rt/manifold_cert.slang`, `manifoldWalkPair<S, false>`): the seeded walk over a
  solid's entry and exit triangles in the pair's own extended parameters, every call of the dual-number map at one site
  (modes seed, walking, polishing). An uncertified root is polished by two Newton steps and kept only inside both
  triangles and where the pair is physical. The slope Jacobian's determinant comes back with the root (`slopeDet`).
- **`manifoldGlass`** (`shaders/rt/path_tracer.slang`): the estimate for one request, light and solid. The two slopes
  are drawn from GGX's whole distribution at the draw's alpha (`manifoldGgxSlope`, its density
  `manifoldGgxSlopeDensity`, `rt/manifold.slang`); one frame reference for every walk of the request (perpendicular to
  the sun and to the solid's direction from X); seeds uniform in the cone of the solid's bounding sphere grown 2%; the
  path counted by the walks it takes to find the same pair and root again, at most 64. A found path is retraced as the
  camera side would (X to y2 and y2 to y1 meet the solid first at the walk's distances; the sun visible from y1's outer
  side, panes multiplying it), its inside absorbs by `transmission_color` at `transmission_depth`, and a dispersive solid
  draws the wavelength the path does not have yet (that channel alone, x3). The three BSDFs (X's rough part, both
  refractions over their whole lobes, in the camera's roles) are evaluated at one OpenPBR site.
  The walk refracts at the IOR the BSDF refracts at (`modulatedIor` of the dispersed specular IOR and the specular
  weight, clamped, as `OpenPBRBase`). The first walk's path is checked and weighed (`manifoldGlassPath`) before any
  re-find walk, so a refused path costs none; the walk keeps one call site.
- **The walk's scene** (`ManifoldInstanceScene`): the instance's own triangles, through rays forced non-opaque that
  commit only its hits, clipped to the instance's bounding sphere (grown 2%), in world space by the gather's
  `manifoldTriangleFrom`.
- **The call site**: in `manifoldConnect`, after step 2a's connections for the same chosen draw, when the draw's
  material transmits, is not thin-walled or a metal, has a smooth-capable lobe (`alphas.x > 0`), no coat and no
  specular-weight texture, and the draw is the first gathered of its (instance, material) (the host's mark in the draw
  record's word 7, `ManifoldCasters::prepare`). The pass's counters add walks, paths found and connections
  (`causticStats[8..10]`, the teardown log). Under NRD the manifold pass stays off (it adds to the running mean after the
  denoiser's inputs are packed).
- **The CPU twin** (`tools/caustics/manifold_kernel.slang` ops 12/13, `manifold_cpu`): `manifold_tests` t15 holds the
  GPU form to light traced through a rough glass ball within the reference's own spread.
- **The GPU tests** (`tests/renderer/check_caustics.py`): `m4_slab_seen`, `m5_ball_seen`, `m6_through_glass`, and after
  the review `m7_split_ball` (m5's ball as one glTF model whose two halves are draws of one glass material, with a 1 mm
  opaque triangle between them) and `m9_ball_weight` (m5's ball at specular IOR 1.8 and weight 0.5: the BSDF refracts at
  1.506).

## 3. Bugs found and fixed

- **A root counted in two pairs at a focus** (t15, +10% on the ball's axis): the walk's root, not polished in its own
  pair's parameters, landed on a shared edge of two pairs. Polished, and kept only inside both triangles.
- **The manifold gather's groups pipeline outlived its layout** (found on the first GPU runs, 62bca315): on NVIDIA's
  Linux driver the groups dispatch then faulted, wrote nothing or hung the GPU, run to run (`m4` dark in 2 of 12 runs, a
  segfault in the driver, one hang). Kept until teardown: 40 of 40 runs right. Step 2a shares the gather.
- **`manifoldConnect` read bindings 4, 5, 8 and 36 the layout gave the hit stages only**
  (VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988, `check_validation`'s caustics scene): the walk reads a solid's
  triangles from the raygen. They carry the raygen stage.
- **A negative constant index** (`kDispersionLambda[-1]`, 54cff06c): `manifoldSurface` with channel -1 inlined into dead
  code; spirv-val refuses it. Clamped.

Found by a review before step 2b went on by default (three lenses, each finding checked by a second reader; 17 held, 2
did not), fixed:

- **Counted once a draw, not once a solid**: the walk takes the whole instance and accepts any path on the draw's
  material, so an instance with N draws of one glass (a merged model's runs of one material) got N times the light.
  `m7_split_ball` read 6157.8 against 3116.7 with the old behaviour (+97.6%), 3090.3 now (0.85%).
- **The walk refracted at the material's raw IOR** while the BSDF refracts at the specular weight's modulated one (and,
  under a coat, the coat-relative one): its paths missed the BSDF's peak. `m9_ball_weight` read 16.4 against 3084.0
  with the old behaviour, 3077.8 now (0.20%). Coated glass and specular-weight textures are refused (no single IOR).
- **Under NRD the pass's light never reached the frame** (it is added to the running mean after raygenCaustics packs
  NRD's inputs) while step 2a's share was dropped from the camera side: the manifold pass stays off under NRD.
- **Cost**: every walk ray traversed the whole scene non-opaque along 10 km (now clipped to the solid's sphere), and up to
  63 re-find walks ran before the checks that then refused the path (now after them; the reviewer showed every accepted
  path keeps its seeds and count). The showcase's step 2b went from +33 to +21 ms.
- **`ManifoldCasters::prepare`'s failure paths** (step 2a's): a failed growth leaked the buffer that had grown and
  retried with a device idle every frame; a failed draws buffer was written through a null mapping. Both buffers are
  released before the smallest pair, the pass stays off after a failure, and the draws buffer falls back to 64 bytes
  holding no draw.
- **Metals and a zero-length axis**: metalness 1 (no refraction) no longer walks; X at the sphere's centre returns
  nothing instead of a NaN seed.

## 4. Spec and test corrections

- **What step 2b owns** (the spec, while building T4): the camera side renders none of the term at any roughness, so
  there is no `chain₂` to drop and the slopes come from GGX's whole distribution, not the core.
- **m6's closed form** (49295a6a): the pane also reflects the sunlit floor behind the camera into every pixel the test
  reads (2F/(1+F) of it); the camera side renders that, so the pass-off check is that only the reflection is left. The
  pass-off render is 10% above that form (0.0357 against 0.0325): not the pane, whose OpenPBR reflectance is the exact
  Fresnel within 0.4% (the CPU twin, 4M samples), but one more real path, the floor under the slab lit by sunlit floor
  whose light reflected inside the slab on the way (glass events are no bounces, so `--bounces 1` lets it through):
  0.0031 by a numpy path simulation of the scene, inside the checks' bars; m4's 0.0031 without the pass is the same term.
- **c3_sphere renders direct light only** (`--bounces 1`, as `c3_slab`): its reference is a simulation of the direct
  caustic. With every bounce the ball's outside reflects the floor under it into its shadow, and with step 2b on that
  reflection carries the caustic too: +1.8% of the shadow's energy (5.54% against the 5% bar). At one bounce step 2b
  changes nothing there (3233.5 with and without), and the test reads step 1's recorded 3.5%.
- **`check_caustics.py` no longer needs scipy**, which the Linux environment lacks (`c5_showcase`'s 5 x 5 median at one
  pixel, in numpy: the same number).

## 5. Results

- `check_caustics.py` (all but the showcase), 2b on: 16 of 16. m4 0.3841 against 0.3799 (1.12%, bar 2%), without the
  pass 0.0031; m5 3111.0 against 3116.7 (0.18%, bar 5%); m6 0.3848 against 0.3825 (0.60%, bar 2%), without the pass
  0.0357 against the reflection alone 0.0325; m7 3090.3 against 3116.7 (0.85%); m9 3077.8 against 3084.0 (0.20%).
- `manifold_tests` t15 (CPU): the GPU form within 3% of light traced through a rough glass ball at three points (+1.2%,
  +0.3%, -3.0%), within 2.3% over 3,000 draws and two seeds (the reference's own spread).
- Cost, `caustics_showcase.scene.json` at 1280 x 720, 16 spp, the capture frame's GPU timings (`render.capture
  {timings}`), twice each: `pt.manifold` 295.2 / 297.2 ms without step 2b, 316.3 / 317.6 ms with it (+21 ms, +7% of the
  pass); 2,014,945 walks, 76,611 paths found, 22,456 connections over the 16 frames. Before the review's fixes: +33 ms,
  2,369,802 walks, the same 76,611 paths. `pt.trace` varies 121-131 ms between
  runs with what the frames before it ran (the GPU's clocks), not with the flag: with the manifold pass stopped early it
  reads 130-131 ms either way.
- `c5_showcase` (480 x 270, the reference at 8,192 spp with every pass; measured before the review's fixes, which leave
  its materials' paths as they were): with the passes at 512 spp relMSE 0.00108, 8
  dots, the clear ball's pixel 0.9x its neighbours; without caustics at 704 spp 0.18830, 18 dots, 1,283x. Step 2b's own
  share, the 512 spp frame without it against the same reference: relMSE 0.00099, 7 dots. Its light
  is 0.044% of the image (22 pixels above 0.01); at 512 spp it adds a little more
  noise than the bias it removes. The sampler is deterministic, so the two frames differ by step 2b's light alone.
  Kept on as step 2a was (ruling M9): the offline profile is the referee and must converge to the light, which nothing
  else renders.
- `manifoldConnect` is 36,527 SPIR-V instructions (slangc 2026.20; 36,306 before the review's fixes).
- Validation (`check_validation.py`): 20 of 20 scenes clean.
- Goldens: none changes. No golden scene has both a directional light and a transmissive solid (`glass.scene.json` is
  lit by an area light; step 2 runs only with a sun).

## 6. Known gaps

- **Two refractions only** (K = 2): paths with reflections inside the solid (about F² of the light, 0.2% at IOR 1.5)
  are no one's.
- **Spiky where X is near a caustic's focus**: the walk's Jacobian goes to zero there, so a connection to a focused
  caustic seen after a bounce is rare and large. In c3's scene step 2b's light in the ball's shadow (the caustic in the
  ball's reflection) puts 59% of itself in 10 pixels at 1,024 spp. Unbiased; it converges slowly.
- **Dark where the solid focuses onto X: found, the re-find trials' cap** (fixed after this record, uncommitted
  verification below). m5's ball through the periscope read, against the simulation, -0.5% at IOR 1.5, -1.1% at 1.45,
  -5.3% at 1.42, -17.7% at 1.39, -27.9% at 1.36 and -29.4% at 1.33 (512 spp; at 1.329 -30.9%, -29.1%, -30.0% at 512,
  2,048 and 8,192 spp: not noise). Paths near the fold are found again by few seeds and weigh much; the cap of 64 cut
  them (the average, 1.2 walks to re-find a path, hid them). Loosening the path checks, the walk's steps or its tolerance
  changed nothing; a cap of 1,024 read -1.44%, and 4,096 and 16,384 the same bits. `kManifoldGlassTrials` is now a
  ceiling of 4,096 and paths reaching it are counted (stats word 11, the teardown log); the showcase's `pt.manifold`
  315.2 / 325.7 ms against 313.8-315.4 at 64. `check_caustics` `m10_ball_focus` (IOR 1.33, bar 5%, none at the ceiling)
  holds it; it, and the whole of `check_caustics` with the ceiling, have not yet run to the end: the GPU's firmware
  crashed during that run (driver 615.78.08, AGENTS.md).
- **Runaway frames: why step 2b is opt-in (2026-11-08).** m9's ball (`m9_ball_weight`, 2,048 spp) did not finish in
  15 minutes with step 2b, at the ceiling of 4,096 and at the old cap of 64 alike, and took 41.7 s without it; at 256 spp
  with the cap of 64 it took 14.4 s with 2,565 paths at the cap. A lane that walks thousands of times holds its warp, and
  a GPU dispatch is not preempted: such frames ran for seconds, and two firmware crashes (2026-11-07 17:23 and 20:29, Xid
  109 then 120) came during them. Why m9 passed in seconds on 2026-11-06 (driver 615.78.08, cap 64) is not known. Until
  the re-find work is bounded per dispatch (trials continued across frames, or a queue of walks dispatched in chunks:
  the estimator unchanged), step 2b runs only with `OHAO_MANIFOLD_GLASS=1`, which its tests set; without it nothing
  renders the sun through a solid seen after a bounce (the frame is darker there, as before step 2b).
- **The polish's acceptance** is an absolute slope tolerance, 10 alpha at roughness 0.01: within about 1e-4 m of a fold,
  0.05-0.15% of roots are a few alpha off (+0.05% to +0.4% there, as occasional large samples; the review's CPU runs).
- **Coated glass and specular-weight textures** are refused (above), so their light through the solid is no one's.
- **Rough solids** (roughness at or above `kCausticRough`, 0.25) and X inside a solid or behind a pane between it and the
  solid are no one's at depth 1-2.
- **One solid a path**: the sun through two solids, or a solid behind another, is no one's.
- **Normal-mapped solids** stay unowned (ruling M7): step 2c (Specular Polynomials, ruling M11).

## 7. How to run

Step 2b's cases set `OHAO_MANIFOLD_GLASS=1` themselves. The ball cases (m5, m7, m9, m10) are out of the default run since
2026-11-08 (the runaway frames above): `--ball` or their names run them.

```console
$ python tests/renderer/check_caustics.py m4_slab_seen m5_ball_seen m6_through_glass m7_split_ball m9_ball_weight   # gpu
$ build/Release/manifold_tests --gtest_filter=Manifold.t15_estimator_brightness > /dev/null
$ python tests/renderer/check_caustics.py --showcase   # gpu # slow
```
