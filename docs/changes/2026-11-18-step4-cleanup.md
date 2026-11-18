# Step 4's cleanup: the frame's known gaps

**Spec:** the system design's 4.5 and 4.6 (step 4); `docs/changes/2026-11-17-step4-frame.md`, "Known gaps"
**Status:** built on branch `step4-cleanup` (from master `b887d3d0`), 2026-11-18. Not merged.

## 1. Summary

Step 4 ended with five gaps:
- CPU recording slower than before the step;
- ReSTIR DI's images at full resolution in its half-resolution mode;
- no transient aliasing;
- 33 device waits;
- the texture table's barriers outside the graph's derivation.

The cleanup closes or settles each:
- **CPU recording:** the regression was the driver's time in image barriers that changed no layout. It is now at or
  below master everywhere, and within noise of the pre-step-4 build except about +0.03 ms on the helmet at 1080p.
- **Memory:** the play scene's targets at 1080p fell from 1,099 MB to 843 MB in deferred, and from 1,634 MB to
  914 MB after path tracing.
- **Aliasing:** measured and not built.
- **Device waits:** two removed.
- **Barriers:** every barrier in the engine now comes from one derivation.

## 2. What was built

- **`gpu::Barriers`** (`ohao/gpu/vulkan/barriers.{hpp,cpp}`): the derivation from each resource's `gpu::State` to its
  next use, used by `render::Pass` and by the texture table's uploads (per mip). It lives in `gpu/vulkan`, which
  includes nothing of `render/`. A dependency with no layout change joins one `VkMemoryBarrier2` per pass; an image
  barrier is recorded only where a layout changes. The engine has one `vkCmdPipelineBarrier2` call site.
- **ReSTIR DI's images sized by the mode:** the 10 reservoir and surface images (RGBA32F) are sized to the trace and
  made again when the shift changes.
- **Images retired when a mode stops reading them:** in reference mode the denoiser's images; in realtime the reference
  means, RT GI's too.
- **The idle path tracer's targets** (394 MB at 1080p) are retired by `releaseTargets` and made again on its next frame.
  The deferred frame's images are kept while path tracing: they are the game's frame and come back at once.
- **Two device waits removed** (33 -> 31): `unloadTexture` retires the texture as `releaseContent` does, and the
  emissive lights' growth uses per-slot reduce sets that each slot's next frame rewrites.

## 3. Bugs found and fixed

- **The CPU recording regression.** Bisected over worktree builds: most of it came with `185ee851`, the rest within
  `66d478b2`..`66d67383`.
  - The helmet's frame recorded 182 image barriers and 44 buffer barriers. 100 of the image barriers changed no layout.
  - The driver spent 0.092 ms a frame in `vkCmdPipelineBarrier2`; the old code used global memory barriers. That time
    is 0.068 ms after the fix.
- **An upload's final barrier waited only for fragment shaders.** Compute and ray-tracing shaders sample the table too;
  it waits for all commands now.
- **A `quality: reference` capture stuck to every later capture**, because headless `render.settings` reads the
  renderer's quality. `frame_capture.cpp` restores it after the capture, as it already did for guiding.

## 4. Spec corrections

None.

## 5. Results

**CPU recording, ms** (render_bench, alternated rounds; ranges are the spread):

| Scene | Before step 4 | Master | This step |
|---|---|---|---|
| cornell_720p | 0.285-0.295 | 0.317-0.332 | 0.305-0.311 |
| helmet_1080p | 0.397-0.439 | 0.452-0.496 | 0.437-0.458 |

**GPU frame against master** (4 rounds, every range overlapping):

| Scene | Master (ms) | This step (ms) |
|---|---|---|
| helmet_1080p | 9.92-9.97 | 9.89-10.02 |
| play_hud | 8.68-9.25 | 8.68-8.71 |
| bench100k | 11.06-11.92 | 11.50-12.04 |

**Targets on the play scene at 1080p, MB** (`render.stats`):

| Mode | All targets, master | All targets, now | ReSTIR DI, master | ReSTIR DI, now |
|---|---|---|---|---|
| Deferred | 1,098.8 | 843.3 | 637.0 | 381.4 |
| DI at full resolution | 1,098.8 | 1,098.8 | 637.0 | 637.0 |
| Reference | 1,240.4 | 1,028.1 | 707.8 | 495.5 |
| After path tracing | 1,634.1 | 914.0 | 707.8 | 381.4 |

The helmet and Cornell follow the same pattern.

**Aliasing, measured and not built.** Each image's first and last use was taken from the passes' declarations across
two frames:
- **Strict saving** (discarded in both frames): 51 MB, 6.1% of targets, on the play scene, the helmet and Cornell
  deferred.
- **Upper bound** (any write first): 161 MB, 19%, which holds only if every keep-write covers every pixel, unproven.
- **Path traced:** 0, because its passes keep everything live.

The bar was a guaranteed saving above 10% of targets: there is no memory budget, a stale read from aliasing is silent,
and the image sizing above saved 255 MB without that risk.

**Proof:**
- `tools/check.py` passed 6/6, with the goldens exact.
- `check_validation` under synchronization validation passed 21/21.
- The mode-switch sequence under synchronization validation gave zero messages: half, full, reference and realtime,
  the path tracer released and recreated, three scene loads.
- No Xid.

## 6. Known gaps

- **CPU recording:** the helmet at 1080p is about 0.03 ms over the pre-step-4 build, from its 80 real layout changes
  a frame (attachments, the bloom chain).
- **31 device waits remain:**
  - 21 at shutdown and 5 at a resize;
  - the sky's replace: its sequence caused the 2026-11-11 Xid, and proving the wait unneeded means running it again;
  - three readbacks, which wait for data;
  - DLSS's release, which cannot run here.
- **Aliasing:** not built; 6.1% strict, up to 19% if keep-writes are proven to cover every pixel.
