# Sub-project 2b, step 7: RT shadows rebuilt

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("RT shadows", "Reference quality", "Build order" step 7, as built; test plan test 9)
**Status:** Built, tested, and every budget it gates passes after Frank's ruling of 2026-10-25 02:40, which moved budget inside `cornell_720p` (section 6).
- RT shadows are two Slang compute passes: a trace with inline ray queries and a filter with temporal accumulation. CSM renders only as the fallback.
- Reference quality is reachable from the renderer, `render.capture`, `ohao run` and the examples.
- `tools/check.py` (quick + gpu + docs): 35/35. `engine_tests` 66/66, 5 of them new. Render goldens: 14/14, three of them new reference goldens. Five deferred goldens were re-baselined in their own commit.
- `render_bench` (a clean run, after the ruling): `cornell_720p` `rt.shadows` 0.252 ms (budget 0.30), `rt.shadows.filter` 0.061 ms (budget 0.08), `csm` not run (budget 0): 3/3 pass. Before the ruling the same work was over the original 0.20 / 0.05 ms.
- Budgets moved by Frank's ruling only, inside `cornell_720p`, frame total unchanged at 1.50 ms. No tolerance was widened.

## 1. Summary

- **One ray per light, the 4 most important a pixel.**
  - Each pixel ranks the frame's shadow-casting lights (up to 8, those in the deferred light buffer) by intensity over squared distance, ties by light index.
  - It traces one ray to a sampled point on each of the top 4.
  - A pixel with more lights worth tracing counts once. `render.stats` reports that count as `shadowLightsDropped`.
- **Filtered and accumulated.**
  - A 5x5 depth- and normal-aware filter, skipped for any light that all 25 pixels agree on.
  - A temporal accumulation, reprojected by the G-buffer's velocity. Its weight is 1/(n+1), down to 1/16, so a still camera averages the frames since the reset evenly.
- **Reference quality** (Frank's ruling, 2026-10-24 18:01): every light, 8 rays a frame, the plain running mean since the reset.
  - Reached through `VulkanRenderer::setRenderQuality`, `render.capture {quality}`, `ohao run --quality` and the examples' `reference` flag.
- **CSM is the fallback only.** It renders when RT shadows do not, so `cornell_720p`'s `csm` budget of 0 passes.
- **Two bugs fixed on the way:**
  - CSM never shadowed anything.
  - A scene with exactly one light was treated as the fallback sun.

## 2. What was built

- **`shaders/rt/shadow_trace.slang`** (pass `rt.shadows`, entry `trace`, 8x8 workgroups).
  - **Inline ray queries.** The GLSL ray tracing pipeline (`rt_shadow.rgen/.rmiss/.rahit`) and its shader binding table are deleted. A device without `VK_KHR_ray_query` keeps CSM.
  - **Light records.** The CPU writes each light's centre and bounding radius into `ShadowLight` (a rect's half diagonal; a spot's direction and outer-cone cosine), so the per-pixel importance takes no square root.
  - **Importance** is 0 out of range, wholly behind the surface, or outside a spot's cone.
  - **Top 4:** each light's key is its importance with the index in the low bits; the top 4 are the keys at or above the 4th largest.
  - **Sampling:** one hash of (render seed, frame index since the reset, pixel) per pixel, then one PCG per (light, ray, dimension).
  - **Realtime output:** `meta`, an RG32UI image.
    - x: a guide word: the distance from the camera as a bfloat16, and the octahedral normal in 8+8 bits. bfloat16 because `f32tof16` made Slang declare Float16/Int16 capabilities the device does not enable.
    - y: traced and visible bits per light, and a surface bit.
  - **Reference output:** the running means, kept in two RGBA32F images and written packed.
- **`shaders/rt/shadow_filter.slang`** (pass `rt.shadows.filter`, entry `filter`).
  - **Tile:** 16x16 groups load a 20x20 tile of `meta` into shared memory.
  - **Agreement test:** a light every neighbour that traced it saw lit (or all saw occluded) takes that value exactly.
  - **Disputed lights:** at most 4 a pixel. Their indices go in one `uint4` and one float4 accumulates them.
  - **Temporal step:** rejected where the history pixel is another surface (depth within 10%, normals within about 25°) or did not trace the light.
  - **Output:** one RG32UI image of eight 8-bit channels (x: lights 0-3, y: lights 4-7), rounded with a hash dither so the mean stays unbiased. Lighting reads it with one `texelFetch`.
- **Light table:** every shadow-casting raster light owns a channel, 0..7, in `params.w`.
  - `kMaxShadowLights` = `MAX_LIGHTS` = 8. `kShadowNoSlot` is deleted.
  - Before, the first 4 casting lights owned a channel and the rest shaded unshadowed everywhere.
- **Shared layout** (`shaders/shared/layout.json`): `ShadowLight`, `ShadowTracePush`, `ShadowFilterPush`, and `kShadowMaxLights` / `LightsPerPixel` / `ReferenceRays` / `HistoryFrames` / `FilterTile` / `FilterRadius`.
- **`RTShadowTechnique`:**
  - `render()` is the trace; `filter()` and `getOutput()` hand over the visibility image.
  - Each frame slot has a host-visible buffer for the lights and the dropped counter. It is device-local when the device has resizable BAR: in system memory every pixel's reads crossed the bus, and the first version took 9.4 ms.
  - `collect()` / `latestDropped()` read the counter once the slot's fence has signalled. Ping-pong pairs hold the meta and visibility images.
- **Renderer:**
  - `DeferredRenderer` decides `rtShadows` before building the graph, records `csm` only without them, then records `rt.shadows` and, in realtime, `rt.shadows.filter`.
  - `setQuality`, `setShadowLightsPerPixel` and `setFrameNumber` are new.
  - `VulkanRenderer::setRenderQuality`, `setRTShadowsEnabled` (no longer forced on every frame) and `shadowLightsDropped()` are new.
  - The pass registry gains `rt.shadows.filter`.
- **API and CLI:**
  - `render.capture` takes `quality` (realtime | reference; the `Quality` enum in the params registry).
  - `render.stats` returns `shadowLightsDropped` when a covered frame traced RT shadows.
  - `ohao run --quality`; the examples take a `reference` flag.
  - A lockstep window capture restores the live quality afterwards, as it does the post settings.
- **Slang build:** a `.slang` file that uses `RayQuery<` compiles with `-capability spvRayQueryKHR`. The GLSL dependency glob now covers `shaders/shared/*.glsl`.

## 3. Bugs found and fixed

- **CSM never shadowed anything.**
  - The cascades were projected with `glm::ortho`, OpenGL's [-1, 1] depth. The near half of every cascade landed below 0, which the lighting pass reads as unshadowed.
  - The lighting pass also sampled the comparison sampler through a plain `sampler2DArray` and treated the result as a depth.
  - Nobody saw it: RT shadows were forced on every frame on RT hardware.
  - Now `glm::orthoRH_ZO` and `sampler2DArrayShadow` with a reference depth (3x3 hardware PCF). Test: `sp2b_t09_csm_is_the_fallback_when_rt_shadows_are_off` (138 shadow pixels from CSM, against 0 before).
- **A scene with exactly one light was treated as the fallback sun.**
  - `renderDeferred` tested `numLights == 1` instead of "the scene has no lights". That light's direction was replaced by the sky's (and at night its colour and intensity), and the ambient was set to the sun's 0.12 instead of the scene's 0.35.
  - No golden has one light; the engine tests' one-light scenes are brighter now (the shadow test's darkened pixels 885 → 440, still a pass).
- **A spot light outside its cone took one of a pixel's 4 slots.** Its importance is now 0 there (its own commit and test).
- **An unexplained timing effect, not kept.** Aggregating the dropped-pixel atomic per wave (`WaveActiveCountBits`) made the *filter* pass measure 0.22 ms instead of 0.07 ms in three runs, with the trace unchanged. The per-pixel atomic measured no slower, so it stays. Cause not found.

## 4. Spec corrections

Recorded in the spec (build order step 7, as built, and the "RT shadows" bullet):
- **Compute, not a ray tracing pipeline.** Shadow rays need no hit shaders, and inline ray queries drop the shader binding table.
- **Output format.** One RG32UI image of eight 8-bit channels, one per shadow-casting light, instead of "an RGBA8 image, one channel per light slot". Lighting needs no per-pixel slot table, and the filter's memory traffic halves.
- **No neighbourhood clamp on the temporal history.** A moving light's shadow trails for up to 16 frames (known gap).
- **Reference GI is still the realtime GI until step 8.**

## 5. Results

- **`tools/check.py`** (quick, gpu, docs): 35/35.
  - New tests: `engine_tests` 66/66 (+5, `sp2b_t09_*`).
  - `deferred_determinism_gpu_test` 55/55, `check_validation` 6/6, `check_live` 26/26, `e2e_mcp` 20/20, `check_serve` 166/166, `check_cli` 29/29, `check_docs` 67/67.
  - The serve and CLI protocol goldens took the new schema hash (5f8a096bdcd40c23) and the `quality` param.
- **Test 9 numbers:**

  | Test | Result |
  |---|---|
  | RT shadows from the scene's lights | 440 shadow pixels each side, mirrored |
  | A capture after a reset, two fresh renderers | identical: 6 realtime frames and 4 reference frames |
  | CSM fallback | CSM 138 shadow pixels, RT 118; CSM absent from an RT frame, RT absent from a CSM frame |
  | Reference with 6 lights | reference darkens 317–318 pixels, realtime 0 |
  | Realtime dropped pixels (6 lights) | 17,814 |
  | Spot outside its cone | 0 dropped pixels |
  | Convergence toward a 16-frame reference | mean difference 0.44 at frame 1, 0.14 at frame 16 |

  In the 6-light rig each shadow falls where its light ranks lowest.
- **Render goldens:** 14/14 (deferred exact, path-traced at the 1-LSB floor).
  - Re-baselined, looked at first: `helmet_game_deferred` and `damaged_helmet_deferred`, with their CLI twins: max_abs 16 on 2.2% of pixels, rmse 0.233, the shelf's penumbra on the box top. `cornell_box_deferred` and its twin: max_abs 1.
  - New: `cornell_box_reference`, `cli_cornell_box_reference` (the same pixels as the example) and `cli_helmet_game_reference`. Against realtime: max_abs 15, 56 of 230,400 pixels more than 4 apart.
- **`render_bench --check`**, clean run (CPU calibration ×0.82, GPU ×1.14, other processes 11.6% of a core):

  | Scene | `rt.shadows` | `rt.shadows.filter` | `csm` | frame |
  |---|---|---|---|---|
  | `cornell_720p` | 0.255 ms (budget 0.20): **over** | 0.062 ms (budget 0.05): **over** | not run (budget 0): pass | 4.701 ms |
  | `helmet_game_720p` | 0.094 ms | 0.046 ms | not run | 5.463 ms |
  | `helmet_game_1080p` | 0.198 ms | 0.100 ms | not run | 12.711 ms |

  - After Frank's ruling (budgets 0.30 / 0.08), a fresh clean run (CPU calibration ×0.99, GPU ×1.12, other processes 47.8% of a core): `cornell_720p` `rt.shadows` 0.252 ms (p95 0.255), `rt.shadows.filter` 0.061 ms (p95 0.062), `csm` not run: 3/3 pass. `helmet_game_720p` 0.092 + 0.045 ms; `helmet_game_1080p` 0.193 + 0.098 ms.
  - Before this step, `rt.shadows` was 0.454 ms (step 5) and 0.516 ms (step 1).
  - `rt.gi` (4.0 ms) dominates every frame until step 8.
- **Optimization record, `cornell_720p` `rt.shadows`:**

  | Change | `rt.shadows` |
  |---|---|
  | Lights in host-visible system memory, ray tracing pipeline | 9.43 ms |
  | Lights in device-local memory, light loops unrolled | 0.331 ms |
  | Inline ray queries in compute | 0.266 ms |
  | Precomputed light records, key-based top 4, cheaper sphere sampling, one hash a pixel | 0.255 ms |

  - The trace's fixed work, with no ray traced, fell from 0.103 to 0.061 ms. Each ray a pixel costs about 0.048 ms at 720p.
  - The filter went from 0.81 ms (local-memory arrays) to 0.233 ms (unrolled), 0.068 ms (the agreement test), 0.066 ms (per-pixel channel groups) and 0.061 ms (8-bit packed output). With the disputed path switched off it measures 0.056 ms: what remains is memory traffic.
- **Reference capture cost** (`cornell_720p`, 8 frames, `render.stats`): `rt.shadows` 3.505 ms a frame (8 lights × 8 rays). Realtime is 0.277 + 0.066 ms. On Cornell, 851,027 of 921,600 pixels reported over the 4-light cap.
- **`serve_bench --check`:** noisy twice (other processes 1.4–1.5 cores), so no verdict.
  - Measured anyway: `window_readback` 5.16 ms and `window_capture_inline` 30.0 ms. Both are gated from steps 9 and 10, and the frame is still `rt.gi`-bound.
  - `tools_list_bytes` 15,278 B (budget 24,576).
- **Validation:** zero messages in `engine_tests` and `check_validation` under the layers.

## 6. Known gaps, and the budget ruling

- **Frank's ruling (2026-10-25 02:40):** option 1 below. `rt.shadows` 0.20 → 0.30 and `rt.shadows.filter` 0.05 → 0.08, from `ssr` 0.10 → 0.04, `gbuffer` 0.15 → 0.10 and `sky+particles` 0.05 → 0.03; the frame stays 1.50 ms; no light cut, no half-resolution shadows. `tests/bench/render_budgets.json` and the spec's "Frame cost" table carry it, each moved entry marked. What led to it:
- **`cornell_720p` `rt.shadows` (0.20 ms) and `rt.shadows.filter` (0.05 ms) were over their original budgets**, at 0.255 and 0.062 ms in a clean run.
  - Cornell has 12 lights; 8 cast through the light buffer, so every pixel has 8 worth tracing and traces 4.
  - The spec's design fixes 4 rays a pixel. At about 0.048 ms a ray-pixel (near this GPU's traversal rate) plus 0.06 ms of fixed work (the G-buffer reads, the ranking, the meta write), the trace cannot reach 0.20 ms on this scene.
  - The filter is bound by memory traffic: history and output, 8 B each a pixel, plus the tile.
  - The helmet scenes (4 lights) cost 0.094 + 0.046 ms.
  - The spec lets a step move budget between passes of a scene when an allocation proves infeasible, never raising the frame budget. An attempt to do that within the step was refused by the session's permissions, in line with "budgets are never relaxed", so it went to Frank with these options:
    1. **Move budget within `cornell_720p`, frame total unchanged at 1.50 ms.** For example: `rt.shadows` 0.20 → 0.30 and `rt.shadows.filter` 0.05 → 0.08, from `ssr` 0.10 → 0.04 (measured 0.022 at full resolution, half resolution at step 9), `gbuffer` 0.15 → 0.10 (measured 0.065) and `sky+particles` 0.05 → 0.03 (measured 0.011).
    2. **A realtime default of fewer lights a pixel** (`rtShadows.maxLights`, step 15's setting). At 2 a pixel the trace would be about 0.16 ms. Fewer shadowed lights.
    3. **Half-resolution shadow rays with an upsample.** Not in the spec.
- **No neighbourhood clamp on the temporal history:** a moving light's shadow trails for up to 16 frames. Light motion does not reset the history.
- **Visibility is 8 bits a light.** Dithered, so means are unbiased, but a single frame carries ±1/255 dither noise in the penumbrae.
- **Reference quality doubles only the shadows.** GI is the realtime GI until step 8.
- **`serve_bench` had no clean run this step.**
- **The monograph figure `hybrid-shadow-technique-penumbra` was removed from its unit:** its legend names the deleted GLSL shader. `graph-render-graph-layout-timeline.svg` still draws `csm` beside the RT passes; its caption says so.

## 7. How to run the new tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json --only cornell_box_reference > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json --only cli_helmet_game_reference > /dev/null   # gpu
$ build/Release/render_bench.exe --only cornell_720p --check tests/bench/render_budgets.json   # gpu
```
