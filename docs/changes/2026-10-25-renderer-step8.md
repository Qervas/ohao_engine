# Sub-project 2b, step 8: RT GI rebuilt

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("RT GI", "Reference quality", "Build order" step 8, as built; test plan test 10)
**Status:** Built and tested. The two budgets it gates are met by every measurement taken, but both runs at the step's end were flagged noisy (outside load), so `render_bench`'s verdict on them is open (section 5).
- RT GI is two Slang compute passes, a trace with inline ray queries and a denoiser, replacing a 32-ray full-resolution ray tracing pipeline.
- `cornell_720p`'s GPU frame fell from 4.64 ms to 0.87–0.90 ms; `rt.gi` from 3.95 ms to 0.064–0.066 ms (budget 0.30), and the new `rt.gi.denoise` costs 0.143–0.148 ms (budget 0.20).
- Reference quality has its own GI: full resolution, 32 rays a pixel, the running mean.
- `engine_tests` 73/73, 7 of them new; `deferred_determinism_gpu_test` 65/65 with two new configurations; render goldens 14/14, re-baselined in two commits of their own (5 deferred, then 3 reference).
- No budget or tolerance was changed.

## 1. Summary

- **Realtime: half resolution, 2 rays, denoised.**
  - The trace shades every second pixel in each direction with 2 cosine rays, stratified in azimuth: half a ray per output pixel instead of 32.
  - A temporal pass reprojects last frame's accumulation by the G-buffer's velocity, keeps only the taps that saw the same surface, and clamps the history to its neighbourhood's range.
  - Two à-trous iterations at half resolution, then a joint-bilateral upsample to full resolution.
- **Reference: full resolution, 32 rays, the plain running mean**, no denoiser; the converged answer realtime approaches.
- **Hit shading made consistent with the lighting pass** (same kind: the 4 brightest lights, no shadow ray), and lighting no longer counts the environment twice.
- **One bug fixed in step 7's code as well**: a reference running mean could blend into a stale or undefined image.

## 2. What was built

- **`shaders/rt/gi_trace.slang`** (pass `rt.gi`, entry `trace`, 8x8 workgroups).
  - **Inline ray queries** with `RAY_FLAG_FORCE_OPAQUE`, closest hit, 100 m rays from the surface offset 0.05 along its normal. The GLSL pipeline (`rt_gi.rgen/.rchit/.rmiss`) and its shader binding table are deleted. Without `VK_KHR_ray_query` the renderer has no RT GI.
  - **A hit** returns its bounce albedo (the instance table's `giAlbedoPacked.rgb`, read by byte offset at the TLAS custom index; `static_assert`s tie the offsets in `shaders/shared/layout.json` to `render::InstanceGpu`) over pi, times the direct light the hit point receives from the frame's 4 brightest lights (by intensity, emissive proxies out; colour x intensity, inverse square windowed by range, a spot's cone, as `deferred_lighting.frag` attenuates them). The hit's normal is taken as facing the ray; no shadow ray.
  - **A miss** returns the environment map in its direction, through the lighting pass's equirect mapping, or black without one.
  - **Realtime output:** the mean incoming radiance (RGBA16F, half resolution) and a guide word (R32UI: the distance from the camera as a bfloat16, the octahedral normal in 8+8 bits, 0 for sky), packed as RT shadows pack theirs.
  - **Reference output:** the running mean (RGBA32F) and, straight to the output lighting reads, mean x diffuse albedo.
  - **Sampling:** one hash of (render seed, frame index since the reset, pixel) per pixel, one PCG per (ray, dimension).
- **`shaders/rt/gi_denoise.slang`** (pass `rt.gi.denoise`, realtime only; three entries with their own binding numbers and set layouts).
  - **`temporal`** (16x16 groups, a 1-pixel apron in shared memory). Bilinear reprojection by velocity; a tap counts only where last frame's guide is within 10% of this pixel's distance and its normal within cos^-1 0.9, and less than 5% of a tap in all is no history. Two accumulations come through: a short one (weight down to 1/4) and a long one (down to 1/32), each with its frame count in alpha. The long one's history is clamped to the short one's range over the 3x3 before blending, so a moved light trails a few frames, not 32.
  - **`atrous`**, twice (steps 1 and 2): a 5x5 B3-spline kernel over the long accumulation, depth tolerance 3% of the distance growing with the offset, normals cos^16 where the packed normals differ.
  - **`upsample`** (full resolution): the 4 nearest half-resolution samples, weighted bilinearly and by depth (5% of the distance) and normal (cos^8); where none is the same surface, the closest match. Times the diffuse albedo, albedo x (1 - metallic), into an RGBA16F output.
- **`ohao/render/rt/rt_gi_technique.{hpp,cpp}`**, rewritten on the RT shadows pattern.
  - `render()` (the trace), `denoise()`, `denoises()`, `traceExtent()` and `raysPerPixel()` (test 10), `resetHistory()`.
  - Per-frame-slot light buffers in device-local host-visible memory; descriptor sets per frame in flight; ping-pong guide and accumulation images kept in `GENERAL`.
- **`ohao/render/rt/rt_compute.{hpp,cpp}`** (new): images that carry their layout, barriers, compute pipelines from SPIR-V, host buffers, descriptor boilerplate. RT shadows moved onto it; every golden's statistics were identical before and after.
- **`DeferredRenderer`**: `rt.gi` passes velocity, quality, the scene's lights and the environment view (kept by `setEnvMap`); `rt.gi.denoise` follows in realtime; lighting gets binding 11 from whichever ran last.
- **`deferred_lighting.frag`**: with RT GI in the frame (flag bit 3) the diffuse IBL term is dropped; specular IBL stays.
- **Pass registry:** `rt.gi.denoise`. **Layout:** `GILight`, `GITracePush`, `GIDenoisePush`, `kGI*`, `kInstanceGpu*`; `ShadowTracePush.history`. **Budgets:** `currentStep` 8.

## 3. Bugs found and fixed

- **A reference running mean read stale or undefined data** (RT shadows since step 7, and the new GI). The shader chose "start the mean" or "blend into it" by the frame index since the reset. A capture always starts at a reset, so no golden saw it; but a live session switching realtime to reference without a reset blended into the mean an earlier reference run left (weight k/(k+1)), and the first reference frame on new images read memory whose contents the barrier had discarded. Each technique now counts the reference frames in its mean (zeroed by a reset, new images and any realtime frame) and pushes the count. Test `sp2b_t10_a_reference_mean_restarts_after_realtime_frames`: max_abs 100 before, 0 after.
- **The old GI's bounce was about 3x too bright**: it omitted the 1/pi of a diffuse bounce and used 1/(1 + d^2) instead of the lighting pass's attenuation. Fixed with the hit shading above.
- **The environment was counted twice** where GI and IBL met: the GI's escaping rays and the lighting pass's unoccluded diffuse IBL. Lighting now drops the latter when GI runs.
- **The GI's environment read asked for mip level 4 of an image with one level** (caught by the monograph review): it reads level 0 and says why. No image changed.

## 4. Spec corrections

Recorded in the spec's step 8 as-built note:
- Hit shading: light colour, the lighting pass's attenuation and 1/pi; the miss reads level 0 of a one-mip environment.
- Lighting drops diffuse IBL when RT GI runs.
- The history clamp uses the range of a short accumulation, not of the 2-ray samples (in a dark corner most of those are zero, and their range would clamp a converged history to black).
- A reference running mean holds the reference frames in an unbroken run, not every frame since the reset.
- Without `VK_KHR_ray_query` there is no RT GI (it was a ray tracing pipeline).

## 5. Results

- **Tests.** `engine_tests` 73/73 (new: `sp2b_t10_gi_is_deterministic_across_fresh_renderers`, `_history_resets_on_capture`, `_multi_frame_captures_converge`, `_reference_is_full_resolution_32_rays`, `_realtime_converges_toward_the_reference`, `_a_red_wall_bleeds_onto_the_floor`, `_a_reference_mean_restarts_after_realtime_frames`). Measured there, on a 160x120 GI scene:
  - mean |frame 8 - frame 1| 0.344, |frame 16 - frame 8| 0.099;
  - mean |realtime - 16-frame reference|: 0.396 at frame 1, 0.193 at frame 16;
  - reference 160x120 at 32 rays, realtime 80x60 at 2;
  - a red wall's floor: mean R - G 3.32 levels, a white wall's 0.
- `deferred_determinism_gpu_test` 65/65: new configurations `cornell_gi_frames8` and `cornell_reference`, identical across two fresh renderers, a second process and a second capture, validation clean.
- `check_validation.py` 6/6 clean.
- **Render goldens** 14/14 after re-baselining, each looked at first: `cornell_box_deferred` max_abs 21 on 80.0% of pixels (smoother, slightly dimmer indirect light); `damaged_helmet_deferred` and `helmet_game_deferred` max_abs 82 on 95.2% (greyer walls where the room occludes the environment); `cornell_box_reference` max_abs 20 on 80.1%; `helmet_game_reference` max_abs 80 on 95.2%.
- **Cornell at 1920x1080**, against a 16-frame reference capture: mean |difference| 0.446 (realtime frame 1), 0.246 (frame 8), 0.221 (frame 16). Frame 1 vs frame 10: max_abs 10.
- **`render_bench`**, `cornell_720p` (RTX 5070 Laptop GPU), three runs:

  | pass | budget | 60 frames | gate run 1 | gate run 2 |
  |---|---|---|---|---|
  | `rt.gi` | 0.30 | 0.066 | 0.064 | 0.066 |
  | `rt.gi.denoise` | 0.20 | 0.148 | 0.143 | 0.147 |
  | `rt.shadows` | 0.30 | 0.260 | 0.250 | 0.258 |
  | `rt.shadows.filter` | 0.08 | 0.062 | 0.061 | 0.062 |
  | frame (gated from step 9) | 1.50 | 0.901 | 0.874 | 0.898 |

  Both gate runs were flagged noisy: the first by its CPU calibration, the second by other processes using 1.3 cores (`dwm.exe`, `Spotify.exe`, `UnrealEditor.exe`); `wait_quiet.py` never saw a quiet minute. Every number is under its budget, but the verdict waits for a clean run.
- `helmet_game_1080p` (unbudgeted until step 9): `rt.gi` 0.164 ms, `rt.gi.denoise` 0.339 ms, frame 2.95 ms.

## 6. Known gaps

- The bounce albedo is still the per-instance `GiMaterialLegacy` word, not the hit triangle's material; a multi-material mesh bounces one colour.
- The hit normal is taken as facing the ray, and the hit is lit without a shadow ray (the spec's "later" quality upgrades).
- The environment has one mip level, so a miss reads full-resolution texels; the lighting pass's diffuse IBL "lod 9" sample reads level 0 too.
- The denoiser has no luminance edge stop: converged GI is blurred by the à-trous kernel (up to 13x13 half-resolution pixels) wherever depth and normal agree.
- `rt.gi.denoise` at 1080p (0.339 ms) is mostly the full-resolution upsample; its budget there is the step-9 frame budget.

## 7. How to run the new tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json --only cornell_720p > /dev/null   # slow
```
