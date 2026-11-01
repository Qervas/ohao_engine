# The path tracer's 8x regression: one trace in raygen, and panes that shadow

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, F1 T6: real time at 1080p); the regression
came with OpenPBR's gaps (`docs/changes/2026-11-01-openpbr-gaps.md`, commit `ecae7d16`).
**Status:** fixed; `pt.trace` on the helmet room is 31.5 ms (213 ms before, 27.4 ms before `ecae7d16`). A pane's
transmission on shadow rays, which `ecae7d16` never applied, is applied.

## 1. Summary

`render_bench` after F5d read `pt.trace` at 213 ms on `helmet_rt_1080p` and 208 ms on `play_rt_1080p` (F3: 15.76 and
14.90 ms). Swapping `path_tracer.slang` alone between commits put the whole jump in `ecae7d16`. Neither scene has glass or
subsurface: the cost was static. Every `TraceRay` site inlined into raygen spills raygen's live state, for every pixel,
and `ecae7d16` added four: a loop of traces in `visible()` (panes), the subsurface walk's two, and the walk exit's two
shadow rays. Raygen now has one trace, one light shadow ray and one environment shadow ray, as before `ecae7d16`. On the way,
the panes' transmission on shadow rays turned out never to have been applied.

## 2. How the cost was split

`helmet_rt_1080p`, `pt.trace` median, the shader alone changed (30 frames unless noted; the GPU calibration within
x1.2 of its reference in every row; raygen's stack from `vkGetRayTracingShaderGroupStackSizeKHR`):

| Raygen | `pt.trace` | raygen stack |
|---|---|---|
| `93e2c979`'s shader (before `ecae7d16`) | 27.4 ms | |
| `ecae7d16` (HEAD before this fix) | 213 ms | |
| HEAD, the pane loop in `visible()` stubbed out | 98.7 ms | |
| Fix 1: panes in `anyHit`, `visible()` one trace | 90.5 ms | |
| Fix 1, the walk branch removed (no subsurface at all) | 24.6, 26.5 ms | 512 B |
| Fix 1, only the walk exit's light sampling removed | 52.6 ms | |
| The walk a mode of the path loop (its steps the loop's trace, its exit the loop's light sampling) | 41.2 ms | |
| ... its medium read back from the material rows, its channel probabilities from the weight | 35.0 ms | 608 B |
| ... its state in the MIS state a walk leaves dead, the exit's normal in the shading frame (built) | 29.8 ms; 31.3, 32.2 ms at 100 frames | 576 B |

A callable shader would have kept the walk off raygen's stack entirely, but Vulkan does not let a callable trace rays
(only raygen, closest hit and miss), and slangc refuses it.

## 3. What was built

- **`visible()` is one trace.** On a shadow ray (`kShadowRay`), `anyHit` meets a pane (`paneMaterial`), builds its hit
  with the closest hit's own code (`fillHit`, which `closestHit` now calls), multiplies its `transmissionAlbedo` at the
  ray's angle into the payload's `emission` (1 at the start) and ignores the hit; one that lets nothing through is a hit.
  `visible()` returns `T`, which each of the six light samples multiplies. The BLAS geometry is
  `VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR`, so each pane multiplies once (the spec allows duplicates
  without it).
- **The subsurface walk is a mode of raygen's path loop** (`walkSteps`, `kNoWalk` when none runs). Lobe 5 starts it: the
  loop's trace becomes the walk's step (tMin 0, the drawn distance, `kRayMaskScene`); a miss is a scattering event
  (`--depth`, not a bounce); a hit is the exit vertex, which skips emission, the AOVs, the cache and the bounce budget, and
  goes through the loop's light and environment sampling with a Lambertian BSDF about the exit's normal and the walk's
  PCG stream; the exit then sets `lastPdf`, `misOrigin` and roulette as before. The walk's arithmetic and random streams
  are the old `walkSubsurface`'s: `cli_subsurface_pt` is unchanged within the floor (14 px of 1 LSB).
- **Its state is small** (`WalkMedium` from rows 15-16 each step; `pj` from the weight), and lives in the MIS state a
  walk leaves meaningless: `misOrigin` holds its weight, `lastPdf` its step's distance, `lastLobe` its material (macros
  in raygen, undefined after it).
- **`OHAO_PIPELINE_STATS=1` covers the path tracer**: the RT pipeline is created with
  `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` and logs its executables' statistics (NVIDIA's driver reports none for a
  ray tracing pipeline) and raygen's stack, `pipeline stats path_tracer: raygen stack N bytes`.
  `rtc::logPipelineStats` and `rtc::pipelineStats()` are public for it.

## 4. Bugs found and fixed

- **Panes cast no light-sampled shadow** (since `ecae7d16`). `anyHit` flagged a pane in the shadow payload's `flags`
  (`kPaneSeen`), but a shadow ray that nothing blocks ends in the miss shader, which clears `flags`; `visible()` then
  never walked the panes, and every unblocked shadow ray through a pane returned full light. The transmission now rides
  in `emission`, which the miss shader leaves alone. The glass scene's thin-walled bubble now dims the light under it by
  what its two filmed surfaces reflect (`cli_glass_pt`, section 6).
- **`anyHit` read the energy tables through a descriptor it could not see.** Binding 38 (`openpbrTables`) was raygen's
  only; the pane's `transmissionAlbedo` in `anyHit` reads it (VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988 in every
  path-traced run under validation). It is raygen's and the hit stages' now. Before the fix the read was undefined: the
  bubble's shadow came out dark and speckled, and a golden captured then was wrong (re-captured after).

## 5. Results

`render_bench`, 1080p, medians of 100 frames (GPU calibration x1.03 to x1.17; `wait_quiet` never saw the machine quiet:
dwm.exe held 1.3 cores, so the CPU side is noisy and these are GPU timestamps):

| Scene | `pt.trace` before | `pt.trace` after | frame after |
|---|---|---|---|
| `helmet_rt_1080p` | 213 ms | 31.5 ms | 34.1 ms |
| `play_rt_1080p` | 208 ms | 30.5 ms | 33.3 ms |
| `play_rt_dlss_1080p` | | 14.5 ms | 19.8 ms (91.8 ms before) |

- **Gate:** `tools/check.py` 58 of 58 (the first run failed `check_validation`, `check_live`, `scene_view_gpu_test`,
  `deferred_determinism_gpu_test` and `check_docs` on one validation message, section 4; 58 of 58 after the fix).
- **Goldens:** 42 of 42 after the re-captures (section 6).

## 6. Goldens

42 scenes; 32 unchanged. Re-captured, each looked at and noted in the manifest:

- `cli_glass_pt`: the bubble's shadow (section 4): max_abs 13 on 1.7% of the pixels, all under the bubble; the frame's
  mean 167.22 to 167.15.
- `cli_transparency_pt`: the panes' stochastic opacity hashes the ray origin's bits, and the restructured raygen rounds
  a shadow ray's origin differently (FMA contraction): the noise reshuffled (22% of pixels, max_abs 14, rmse 0.45), the
  mean unchanged (50.796 and 50.794).
- The BVH floor, from the BLAS flag (a different build): `cornell_box`/`cli_cornell_box` 9 px (max_abs 4),
  `damaged_helmet`/`cli_damaged_helmet` 31 px (3), and four deferred scenes through RT direct light's and GI's rays:
  `cli_khronos_sponza_deferred` 1 px, `cli_blockout_play_deferred` 10, `cli_blockout_play_hud` 12,
  `cli_sponza_night_deferred` 214 (each max_abs 1).

## 7. Known gaps

- **The trace is still above F3's.** `pt.trace` on the helmet room is 31.5 ms against F3's 15.76 ms; before
  `ecae7d16` it was already 27.4 ms. OpenPBR steps 2 to 4 (layers, glass, subsurface) grew raygen without a timing; with no
  walk at all it is 24.6-26.5 ms. That is the render-to-budget step's first line.
- The walk's state shares variables with the MIS state (macros): a later change to raygen that reads `misOrigin`,
  `lastPdf` or `lastLobe` during a walk would read the walk's state.

## 8. How to run

```bash
build/Release/render_bench.exe --only helmet_rt_1080p
OHAO_PIPELINE_STATS=1 build/Release/ohao.exe run assets/scenes/cornell_box.scene.json --render out.png --mode rt_realtime
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_glass_pt
```
