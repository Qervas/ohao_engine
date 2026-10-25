# Sub-project 2b, step 13: clustered lighting

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Clustered lighting", "Frame cost", "Build order" step 13, as built; test plan test 8)
**Status:** Built and tested. Every step-13 budget passed in the full `render_bench` runs. Those runs were flagged noisy by CPU load (a Defender scan after a restart; the GPU calibration was clean), so the gate's verdict stays open (section 5). One per-pass budget moved inside `cornell_720p` under the spec's "Frame cost" rule (section 4).
- Deferred lighting is clustered. A compute pass, `lights.cluster`, lists for each of 3,456 froxels (16x9 screen tiles x 24 exponential depth slices) up to 256 lights that reach it. Each pixel sums its froxel's list.
- The raster light table is uncapped (up to 65,536), in a storage buffer per frame slot. Cornell's 12 lights now all render; before, the 8-light cap dropped four.
- The lists light every pixel exactly as summing every light does (PNGs byte for byte) wherever no froxel overflows. Overflow is counted and `render.stats` reports `lightsOverflow`.
- Test 8: `light_cluster_gpu_test` 25/25, validation clean.
- Goldens: two Cornell images re-baselined in their own commit for the 4 newly drawn lights; the other 12 scenes are unchanged (max_abs 0).

## 1. Summary

- **Light table** (`LightClusters`, `ohao/render/deferred/light_clusters.*`).
  - One host-visible storage buffer per frame slot of `LightGpu` records (64 bytes: position, direction, color, params).
  - A slot is rewritten after its fence, and only when the RenderScene's `rasterLightsVersion()` changed. A still scene of 1,000 lights uploads nothing.
  - It grows by doubling from 64 records. Lights past `kLightCapacity` (65,536) are dropped with one warning.
- **`lights.cluster`** (`shaders/compute/light_cluster.slang`). It runs after `gbuffer.late`, one 64-thread workgroup per froxel.
  - It bounds the froxel in view space: the tile's four corners at the slice's near and far depths, through the jittered projection the G-buffer used.
  - It tests every light's sphere against that box in light-index order and compacts hits through a groupshared bit mask (ordered, no atomics across workgroups).
  - It writes the count and up to 256 indices. Overflow statistics go to a per-slot buffer: lights dropped, froxels that dropped any, the largest count.
- **Lighting** (`shaders/core/deferred_lighting.frag`).
  - It reads `LightGpu lights[]` (binding 5) and the lists (binding 16), finds the pixel's froxel from `gl_FragCoord` and view depth, and loops over the list.
  - It skips a light whose attenuation or shadow is exactly 0.
  - Flag 64 (`DeferredRenderer::setClusteredLighting(false)`) sums every light: the test's reference.
- **RT shadows** keep 8 channels, for the first 8 shadow-casting lights in table order. Casters past them render unshadowed; `RenderScene::shadowCastersWithoutChannel()` counts them.
- **`render.stats`** gains `lightsOverflow`: the lights the newest covered frame left out of full froxels. It is absent when none was.
- **Bench scenes** (`tools/bench/bench_world.hpp`, `buildRenderBenchWorld`): 1,000 point lights over the grid (40 x 25, 15 m range, 4 shadowed) plus the sun.

## 2. What was built

- `shaders/shared/layout.json`: `kLightCapacity`, `kClusterTilesX/Y`, `kClusterSlices`, `kClusterCount`, `kClusterMaxLights`, `kClusterGroupSize`, `kClusterNear`; the structs `LightGpu` and `ClusterPush` (view, proj terms P00/P11/P20/P21, screen size and slice scale/bias, light count). The generated `.glsl`/`.slang`/`.hpp` were regenerated.
- **Slice mapping**: `slice = floor(log(d) * S + B)`, with `S = 24 / log(far / 0.1)` and `B = -log(0.1) * S`. `build()` computes `S` and `B` once a frame and hands the same numbers to the kernel and to the lighting pass.
- **Conservative box**:
  - the tile is padded by 1.5 px;
  - the slice depths by x0.999 / x1.001 (slice 0 reaches back to the eye);
  - light ranges by x1.0001 + 1e-4.
  
  Directional lights (`position.w < 0.5`) are in every froxel.
- `VulkanRenderer::setFrameLights()` (`buffer_setup.cpp`) hands the scene's raster lights and their version to `DeferredRenderer::setLights`; with none, the fallback sun (sky/moon blend and day/night ambient). `VulkanRenderer::lightsOverflow()` collects finished slots.
- `DeferredLightingPass::setLights` / `setClusters`; binding 5 is now a storage buffer and binding 16 is new. Push constants: `ambientIntensity`, `sliceScale`, `sliceBias`.
- `pass_registry.hpp`: `lights.cluster` (compute), between `gbuffer.late` and `ssao`.
- `LightTable`: raster lights uncapped, `rasterVersion()` (bumped on any byte change), `shadowCastersWithoutChannel()`. `MAX_LIGHTS` is 65,536; `kMaxShadowLights` is 8 on its own.
- `DeferredRenderer::setClusterFarPlane`: the slices end at the camera's far plane. An orthographic projection (`proj[2][3] == 0`) builds no lists and sums every light.
- Removed:
  - the light UBO (`LightUniformBuffer`, `m_lightBuffers*`, `createLightBuffer`, `updateLightBuffer`, `calculateLightSpaceMatrix`, `lightBuffer()`/`lightUniforms()`);
  - three dead GLSL includes (`includes/common/types.glsl`, `includes/lighting/blinn_phong.glsl`, `includes/shadow/shadow_pcf.glsl`);
  - the `MAX_LIGHTS` define in `constants.glsl`.

## 3. Bugs found and fixed

- **Froxels stopped at 1,000 m.** The slices first took the far plane `renderDeferred` hands `setCameraData` (a literal 1,000, used by CSM). Lighting clamps a pixel's slice to the last one, so under a camera whose far plane is farther, a pixel past 1,000 m sat in a box ending at ~1,001 m and missed its lights. Fixed with `setClusterFarPlane(camera far)`. Test: a wall 1,500 m away under a 3,000 m far plane; with the fix reverted, clustered and every-light PNGs differed.
- **Orthographic projections.** The froxel corners invert a perspective projection, so an orthographic view built boxes far from the pixels they hold. Fixed by skipping the lists. Test: an orthographic camera 40 m over 16 small lights, max_abs 130 with the guard reverted, 0 with it.
- **The sky painted over far geometry** (older than this step). `sky.frag` treated depth >= 0.9999 as sky. At a 0.1 m near plane that is every surface past ~1,000 m. The G-buffer clears depth to exactly 1.0, so `depth < 1.0` is geometry now. No golden changed.
- **Monograph:** citations to the removed light UBO and GLSL includes (about 35 across 13 units) were rewritten; a new unit, `deferred/clustered-lighting`, was added.

## 4. Spec corrections

- Particles do not shade with lights, and RT GI keeps its own 4 brightest lights (its hits lie outside the camera's froxels). Neither reads the lists, though the spec said both would.
- RT shadow channels stay 8, for the first 8 casters; casters past them render unshadowed.
- The slices take the camera's far plane; orthographic views build no lists.
- **Budget move (the "Frame cost" rule).** `cornell_720p`'s `lighting` budget of 0.12 ms cannot hold all 12 lights with the BRDF held exact by test 8.
  - The pass costs about 0.027 ms plus 0.022 ms per light at 720p: 0.188 ms for the 8 lights it drew before this step, 0.27 ms for 12.
  - The cost was measured by setting the light count to 0 (the base) and from the 8-light baseline.
  - 0.20 ms moved from `rt.gi` (0.30 -> 0.10; it measures 0.065) to `lighting` (0.12 -> 0.32), in `tests/bench/render_budgets.json` with a `moved` note.
  - The frame budget (1.50) and the scene's total are unchanged.

## 5. Results

- **Test 8**, `light_cluster_gpu_test`: 25/25 under `OHAO_VALIDATION=1`, 0 validation messages. Measured during the run:
  - Cornell: 12 lights; 8 with a shadow channel, 4 unshadowed.
  - The bench lights (1,001): every froxel non-empty (the sun), 4.6 lights on average, at most 97, no overflow.
  - 1,000 packed lights: 1,508 full froxels, 734,066 lights dropped, the largest froxel reached by all 1,000; `render.stats` reports `"lightsOverflow":734066`.
- **Other suites**: `engine_tests` 80/80, `cull_parity_gpu_test` 45/45, `deferred_determinism_gpu_test` 65/65 (all under validation). `check_serve` 166/166 and `check_cli` 29/29 after the `schemaHash` goldens moved (the `render.stats` signature).
- **`tools/check.py`**: 37/37 (quick, gpu, docs), `check_docs` 67/67.
- **Goldens**: 14/14.
  - `cornell_box_deferred` and `cornell_box_reference` were re-baselined in their own commit: max_abs 96 over ~98% of pixels, the newly drawn lights 8-11.
  - Every helmet golden (<= 8 lights) is max_abs 0 against the images made before the step: the exactness the spec asks for.
- **`render_bench`** (full run, GPU calibration x1.06, CPU x1.94: flagged noisy):

| Budget | Measured | Budget |
|---|---|---|
| `cornell_720p` frame | 0.969 ms | 1.50 |
| `cornell_720p` lights.cluster | 0.010 | 0.03 |
| `cornell_720p` lighting | 0.271 | 0.32 (moved) |
| `cornell_720p` rt.gi | 0.065 | 0.10 (moved) |
| `bench100k_1080p` frame | 4.155 | 8.0 |
| `bench100k_1080p` lights.cluster | 0.061 | 0.10 |
| `bench100k_1080p` cpu | 0.312 | 0.5 |
| `bench100k_1080p` cpu / bench10k | 1.125x | 1.2x |
| `helmet_game_720p` / `_1080p` frame | 0.871 / 1.890 | 3.0 / 5.0 |
| `meshlet_build_1m` | 47.4 | 60 |

  `bench100k_1080p`'s `lighting` with 1,000 lights is 0.465 ms at 1080p.

  A second full run (GPU x1.04, CPU x1.49, also noisy) gave the same GPU numbers to within 0.01 ms: `cornell_720p` frame 0.962 ms, `lighting` 0.270, `lights.cluster` 0.010; `bench100k_1080p` frame 4.152, `lights.cluster` 0.059. Its CPU ratio was 1.353x (bench100k 0.193 ms), against 1.125x in the first run. That is the CPU-side throttling step 11 recorded; both scenes carry the same 1,000 lights.
- **`serve_bench`**, two runs, both flagged noisy (other processes used 0.7 and then 4.8 cores; UnrealEditor up to 3.7 during the window part):
  - First run: every budget passed, including `window_readback` 1.32 ms (2) and `window_capture_inline` 8.22 ms (20).
  - Second, heavier run: `window_readback` 2.51 ms and `window_capture_inline` 13.4 ms.
  - Neither run is a verdict.

## 6. Known gaps

- CSM still splits its cascades over the fixed 0.1-1,000 m `renderDeferred` passes, not the camera's planes.
- Orthographic projections lose geometry nearer than their near-far midpoint (`glm::ortho` keeps GL's -1..1 depth). Nothing in the engine uses them; the cluster guard only keeps lighting right where they do render.
- The build tests every light per froxel. Past a few thousand lights a hierarchical pre-pass is the spec's later work.
- Particles do not shade with lights, and RT GI shades its hits with 4 lights.

## 7. How to run the new tests

```
build/Release/light_cluster_gpu_test.exe            # test 8; --keep leaves the compared PNGs in build/
OHAO_VALIDATION=1 build/Release/light_cluster_gpu_test.exe
build/Release/render_bench.exe --only cornell_720p --check tests/bench/render_budgets.json
build/Release/render_bench.exe --only bench100k_1080p --check tests/bench/render_budgets.json
```

## 8. Commits

- `38fa235` feat(render): clustered lighting: froxel light lists, an uncapped light table, overflow in render.stats
- `5807bbd` golden(render): re-baseline the Cornell deferred and reference goldens: all 12 lights render
- `c00ef05` fix(render): froxel slices reach the camera's far plane; no froxels for orthographic views; sky only over cleared depth
- this record, the spec as built, the budget move, AGENTS.md, ARCHITECTURE.md, the CLI and serve guides, and the monograph
