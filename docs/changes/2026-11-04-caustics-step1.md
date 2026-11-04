# Caustics, step 1: light traced from the sun

**Spec:** `docs/specs/2026-11-03-caustics-design.md` ("The split", "The light pass (step 1)", "Step 1 as built")
**Plan:** `docs/plans/2026-11-03-caustics.md` (tasks 1-7)
**Status:** built and tested, **off by default** (ruling C1 waits on Frank). Branch `caustics`.

## 1. Summary

The offline path tracer only ever traced from the camera. Sunlight that reaches a shaded matte surface by way of a
smooth one (the helmet's metal, glass, a coat) was found by luck, as rare samples hundreds of times too bright: the
showcase's dots. Through glass it was not found at all: light sampling looks above a surface only, and no bounce ray hits
a delta sun, so the glow a glass ball focuses into its shadow was missing from the image, however long it rendered.

Step 1 adds light traced from the sun. Every surface's BSDF splits into a smooth part and a rough part. A new pass,
`causticTrace`, sends rays from each directional light at the objects that can be smooth, follows their smooth parts,
and connects to the camera through the rough part of each surface it reaches; `raygenCaustics` (the path tracer
compiled with the split) drops exactly those terms. Every path is counted once, so the image is the same exact estimate,
now with the paths each technique can find given to the one that finds them.

On the showcase (Sponza by day, the helmet, glass, amber, gold, car-paint and bubble balls) at equal time against an 8192
spp reference: the whole image's relMSE 0.00095 with the pass against 0.188 without (190x lower), typical pixels as
good, half the dots, the glass ball's dot (1,283x its neighbours) gone, the helmet's sun glints 33x their neighbours to
1.8x.

## 2. What was built

- **OpenPBR's smooth and rough parts** (`shaders/material/openpbr.slang`): `evalParts` (their sum is `eval`),
  `evalLobesSmooth` (light sampling's lobes with their smooth part, one evaluation), `sampleSmoothDirection`,
  `sampleSmooth`, `pdfSmooth`, `smoothShares`, `hasSmooth`; the CPU twin's ops 14-16.
- **The setting**: `render.capture {caustics}`, `ohao run --caustics`, the renderer's override restored after a capture,
  the `pt.caustics` pass, layout constants `kCausticRough` 0.25, `kCausticRaysPerPixel` 8, `kCausticSplatScale` 1024.
- **Casters** (`render/rt/caustic_casters.*`): instances whose material can be smooth (factor or the roughness
  texture's smallest texel, measured at upload), their world bounding spheres projected along each light to disks; an
  origin is drawn by light power, disk area and a uniform point, its density the sum over covering disks.
- **The camera side's split**: raygen is a generic over `CAUSTICS`; `raygen` compiles without it, `raygenCaustics` keeps
  `chainThroughput` (every factor `throughput` takes, `f_rough / pdf` at the first hit, `f_smooth / pdf` after) and
  subtracts the owned terms at directional-light samples. A second program (`path_tracer_caustics`) holds it and
  `causticTrace`, made the first time a frame needs it.
- **The light pass** (`causticTrace`, `rt/caustics.slang`, `render/rt/caustic_pass.*`): weights in the camera side's
  roles, Veach's shading-normal correction, the pinhole's importance for a box pixel, raygen's bounce limit, panes as
  null events, a fixed-point splat with stochastic rounding that `raygenCaustics` adds and clears.
- **Agents**: `render.stats` `caustics {casters, clamped}`; the pass logs the same when it ends.
- **The pipeline cache**: one file per shader version, `<name>@<hash>-<device>.bin`, pruned on save; compile times in the
  log.
- **Scenes**: `assets/scenes/caustics.scene.json` (a glass and a chrome ball in sun), `caustics_showcase.scene.json`.

## 3. Bugs found and fixed

- **Casters aimed at untransformed meshes.** `RenderScene::Draw`'s bounds are the mesh's own; the light pass took them as
  world bounds, so a moved caster's rays went where its mesh sits at the origin. A glass ball's caustic came out cut in
  half with a straight edge. Found against an independent numpy ray simulation of the sphere; the closed-form scenes
  (positions baked into their meshes) could not show it. Now `c3_sphere`.
- **The split cost the default tracer 8.5%** of `pt.trace` with caustics off (helmet_rt_1080p 69.0 to 75.0 ms): it moved
  into its own program, and the default raygen measures as before.
- **The caustics program never reached the pipeline cache**: two more inlined material evaluations than the default raygen,
  and the driver stored nothing for it; with them gone it cached (7 minutes to 3 s warm). Warm starts of a program this
  size still depend on NVIDIA's own shader cache (`%LOCALAPPDATA%/NVIDIA/DXCache`, 14 GB here); see Known gaps.
- **The pipeline cache file grew past its cap** (every shader version's entries in one file) and was then ignored, so every
  process compiled cold (414 s for one render). Now one file per version.
- **A capture's first frames had no casters**: they were built only when the setting was on as the scene was handed over.
- **A barrier** left the previous frame's clears of the splat buffer not visible to this frame's atomics.
- **The light pass counted paths the camera side never reaches**: it now applies raygen's bounce limit.

Found by the final review, each with a test that failed first:

- **Paths' seeds clustered**: a path's seed was `index ^ h(frame)`, so a frame's paths sat in one block of the seed space,
  which another frame's block could overlap. Now `h(h(index) ^ h(frame))`, held by `caustic_casters_test`
  `SeedsSpreadOverTheRange`.
- **A thin wall's back side was a front side seen from below**: a coated or fuzzed thin-walled material's eval, parts,
  pdf and sample are now the front's, mirrored (`material_tests` `t6_thin_symmetric`).
- **Light meshes shadowed the pass's sun**: its first ray met the emitter meshes, which the camera side's sun shadow
  rays never see. The first ray now skips them; later rays meet them as raygen's bounces do (`c1_emitter`: 0.0027
  against a closed form of 0.3376, now 0.3378).

## 4. Spec corrections

In the spec's "Step 1 as built": the camera side cannot render sun through glass (a bias, not noise); the split in its own
program; the pass applying raygen's bounce limit; casters through their transforms; panes as null events with their
refraction share's probability; `render.stats` reports casters and clamped splats, not rays and splats.

## 5. Results

`check_caustics` (gpu):

| test | measured | bar |
|---|---|---|
| c1_split_only: the split alone on a patch only the pass lights | 7e-12 | 0 |
| c1_mirror: a mirror's closed form | 0.3378 against 0.3376 (0.07%; 0.24% at 1 ray a pixel) | 2% |
| c1_emitter: the same with a lamp's mesh above the mirror | 0.3378 against 0.3376 (0.07%) | 2% |
| c3_slab: a glass slab, every internal reflection, closed form | 0.3844 against 0.3805 (1.0%) | 2% |
| c3_sphere: a moved glass ball against a ray simulation | 3.6% of its energy; halves 0.02% apart | 5%, 3% |
| c2_split: metal at 0.15 / coat at 0.15, split against the camera side alone | 0.13% / 0.06% | 1% |
| c4_through_glass: a patch seen only through glass | 0.0332 both, bit for bit | 1% |
| c6_repeat: two captures in one process | identical | identical |

`check_caustics_showcase` (slow): relMSE 0.00095 with against 0.188 without, 8 dots against 17, the glass dot 1.0x its
neighbours against 1,283x. `caustic_casters_test` 5/5, `pipeline_cache_files_test` 3/3, `material_tests` 31/31
(`t6_parts_sum`, `t6_parts_threshold`, `t6_sample_smooth`, `t6_thin_symmetric`), `check_material --caustics` 51/51, `check_analytic --caustics`
16/16, `check_validation` with a caustics capture: clean. Render goldens 42/42 unchanged.

Rulings measured: C3 (rays a pixel) 8; C2 (the cut) 0.25. The pass at 8 rays costs about 1.4x a showcase frame (29 against
20 ms at 480 x 270).

Full gate (`python tools/check.py`): 59/61 on its first run. The two failures were this step's: `reflect_tests`' lints
(the float-atomic lint matched the splat's index by name, a float `c` elsewhere in the file: renamed; the wall-clock lint
the compile timing: allowlisted as a timing statistic), which `check_docs` repeats. After the fixes `reflect_tests` 42/42
and `check_docs` 98/99, the one failure `.claude/skills/verify/SKILL.md:22`, known (the installed skills predate their
update in `docs/agents/skills/`).

## 6. Known gaps

- **Light seen through glass** (the floor's glow viewed through a ball) is neither technique's yet: step 2, specular
  manifold sampling.
- **Glints that reach the camera after another matte bounce** (helmet to floor to wall) stay with the camera side; step 3
  only if measured to matter (ruling C6).
- **Typical pixels on the dark floor keep their grain**: ordinary indirect noise, the same with and without the pass.
- **Cold compiles**: the caustics program compiles in about 3.5 minutes cold, and a second process is warm only while
  NVIDIA's own shader cache keeps it. A driver deferred operation on 1 or 5 threads measured the same (209 s, 212 s).
  A smaller raygen is on the roadmap's engine iteration items.
- **Defaults**: off until Frank rules C1; C4's bar as recommended (`c5_showcase`).
- **The final review's minors, not fixed**: a coated pane's null-event weight; the bounce limit when the first hit itself
  crosses; a saturated splat is counted, not saturated; under NRD the splat goes to the diffuse guide only; each ray's
  loop over the disks grows with the casters; `setScene` runs every frame; the view model's TLAS mask in the light pass;
  no test holds `evalLobesSmooth` to `evalParts`' smooth part; skinned instances cast from their bind-pose bounds.
- Found, not fixed here: `RenderScene::analyticLightPower` sizes the sun's share from the same mesh-local bounds (light
  selection only, unbiased); `check_validation`'s `cornell_box rt_offline` hit `vkResetFences` on a fence in use once,
  right after a cold compile (clean on reruns).

## 7. How to run

```console
python tests/renderer/check_caustics.py                 # c1-c4, c6 (gpu)
python tests/renderer/check_caustics.py --showcase      # c5 (slow, an 8192 spp reference)
build/Release/caustic_casters_test.exe
build/Release/pipeline_cache_files_test.exe
build/Release/material_tests.exe t6_parts_sum
build/Release/ohao.exe run assets/scenes/caustics.scene.json --render caustics.png --mode rt_offline --spp 256 --caustics
```

`OHAO_CAUSTIC_RAYS` overrides the rays a pixel; `OHAO_CAUSTICS_SPLIT_ONLY=1` drops the pass's terms without running it.
