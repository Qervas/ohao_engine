# Sub-project 2b, step 10: the capture path

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Capture budgets", "The frame after 2b" (`capture.preview`), "Build order" step 10, as built)
**Status:** Built and tested. Every number the step gates measured under its budget, but every `serve_bench` run at the step's end was flagged noisy by outside load, so the verdict is open (section 5).
- The temporal reset no longer idles the device. The next frame starts without history and clears what it keeps, ordered after the frames in flight by the queue.
- TAA's jitter walks all 16 of its Halton samples by frame index since the reset. It used to walk 3, by frame-in-flight slot.
- `capture.preview` makes an inline capture's preview on the GPU, straight into the staging buffer, and the CPU reads only that: 1.3 MB, not 3.7 MB.
- `engine_tests` 80/80 (4 new); render goldens 14/14, the four TAA scenes re-baselined in a commit of their own; `tools/check.py` 35/35.
- `serve_bench` `window_capture_inline` 8.78 ms against its 20 ms budget (34.3 ms when the budget was set, 10.2 ms after step 9).
- No budget or tolerance was changed.

## 1. Summary

- **Reset by clears.** `VulkanRenderer::resetAccumulation()` (deferred) only marks the temporal state reset. RT shadows and RT GI already dropped their history by flag. TAA clears its two history images in the next frame's command buffer, its barrier waiting on earlier frames' TAA writes and reads.
- **Why the idle existed.** TAA's jitter was indexed by the frame-in-flight slot, so a capture had to restart the slots (and idle the device to do it) to be deterministic. The jitter is now indexed by the frames since the reset, and nothing a frame renders depends on its slot.
- **GPU preview.** One compute dispatch box-filters the region or the whole final image to at most `maxSize` on the long side. It writes packed RGBA8 into the frame's host-cached staging buffer. The bytes equal the CPU filter's exactly in every case tested.
- **Preview-only readback.** A deferred capture that wants the inline image and nothing else asks for the preview alone; the frame copies nothing else.

## 2. What was built

- **`shaders/compute/capture_preview.slang`** (entry `downscale`, pass `capture.preview`): `runtime::fitWithin`'s area filter.
  - Each output pixel's footprint is [i·s/d, (i+1)·s/d) on each axis. Each source pixel weighs by its covered length over the footprint, the vertical sum first.
  - Each texel is turned back into its UNORM byte before it is weighted.
  - The workgroup is one row of 64, so a warp writes 128 contiguous bytes of host memory.
  - Push constants: `PreviewPush` (source rectangle, preview size) in `shaders/shared/layout.json`.
- **`CapturePreviewPass`** (`ohao/render/deferred/capture_preview_pass.{hpp,cpp}`): one descriptor set per frame slot, written at record time (the final image sampled, the staging buffer as storage). It records a barrier from the final image's writers, the dispatch, then a buffer barrier to host read.
- **`render::previewExtent`** (`ohao/render/frame/preview_extent.hpp`): the one size rule, shared by the GPU preview and `fitWithin`.
- **`VulkanRenderer`:**
  - `requestPreviewReadback({rect, maxSide})` and `readbackPreview()` (pixels, width, height); `readback()` returns nothing for a preview frame.
  - `previewOnGpu()`: Deferred mode with the pipeline built. A preview request in a path-traced mode reads back in full.
  - `framesInFlight()` (asks the fences, never waits), `framesSubmitted()` and `waitForSubmitted(n)`.
  - The staging buffers gained `STORAGE_BUFFER` usage.
  - `capture.preview` joins the pass registry between `gizmo` and `present`.
- **Capture procedure** (`ohao/runtime/frame_capture.{hpp,cpp}`):
  - `requestCaptureReadback` picks the preview alone when the request is inline, has no `path`, keeps no pixels, and the renderer can make it.
  - `readCaptureOutputs` waits for the frames submitted ahead of the read frame (queue order makes it wait for them anyway), then times the read frame's fence and copy as `readbackMs`.
  - `LiveHost::capture`'s realtime path uses both functions and checks the region against the window's size before rendering.
- **TAA** (`taa_pass.cpp`): the history clear's barrier waits on `COLOR_ATTACHMENT_OUTPUT | FRAGMENT_SHADER` (it had `TOP_OF_PIPE` with no access, correct only after an idle). The jitter comes from `DeferredRenderer::render` with the frames since the reset.
- `render_budgets.json` `currentStep` 10; `engine_tests` links `ohao_runtime` (the CPU filter for comparison).

## 3. Bugs found and fixed

- **TAA used 3 of its 16 jitter samples.** The Halton index was the frame-in-flight slot (0, 1, 2). Every TAA capture resolved from three sub-pixel offsets. The four TAA goldens changed: max_abs 83 on 31–35% of pixels, rmse 2.2, along edges.
- **TAA's history clear did not wait for earlier frames.** Its barrier started from `TOP_OF_PIPE` with no source access. That is safe only because the reset idled the device; without the idle, a frame in flight could still be sampling or writing the image being cleared.
- **The first preview kernel was 9x over its budget:** 0.49 ms at 768x432, in 8x8 workgroups that write four 32-byte runs of host memory a warp. Rows of 64: 0.056 ms.
- **`window_readback` absorbed the queue.** With the idle gone, a capture's frame queues behind the frames `world.step` just presented, and the fence wait included them: 2.61 ms in one run, against the 2 ms budget. The capture now waits for the frames ahead first (they bound the read frame anyway), and `readbackMs` is the read frame's own wait and copy: 1.18 ms.

## 4. Spec corrections

Recorded in the spec's step 10 as-built note:
- The preview-only path also excludes captures that keep the whole frame (tests) and path-traced modes (no `capture.preview`). A capture with a PNG `path` keeps the CPU preview from the full frame; the two filters give the same bytes in the tests.
- "Waiting for the frames already queued" sits before the read frame's fence, not inside the reset; `readbackMs` times the read frame alone, as `window_readback` was defined.
- The 0.2 ms "preview readback" line is the preview pass's own write: there is no separate copy.

## 5. Results

- **Tests.** `engine_tests` 80/80. New:
  - `sp2b_s10_a_capture_after_a_reset_is_the_same_from_any_frame_slot`: a 6-frame TAA capture after 1, 2 and 7 warm-up frames from another viewpoint equals a fresh renderer's, max_abs 0 each.
  - `sp2b_s10_the_reset_does_not_wait_for_the_gpu`: after three 1080p reference frames, up to 3 still in flight when the reset returns; slowest reset 0.0001 ms.
  - `sp2b_s10_gpu_preview_matches_the_cpu_downscale`: max_abs 0 and 0 bytes different at 1280x720 → 768x432, a 500x300 region → 200x120, 1920x1080 → 768x432, and 160x120 unscaled.
  - `sp2b_s10_a_preview_request_reads_back_only_the_preview`: 768x432x4 bytes, no `readback` pass, `capture.preview` timed.
- **Other suites.** `deferred_determinism_gpu_test` 65/65; `check_live.py` 26/26; `e2e_mcp.py` 20/20; `check_cli.py` 29/29; `check_validation.py` 6/6 clean; `tools/check.py` 35/35.
- **Goldens.** 14/14 after re-baselining. `damaged_helmet_deferred`, `cli_damaged_helmet_deferred`, `cli_helmet_game_deferred` and `cli_helmet_game_reference` (the TAA scenes) each changed by max_abs 83, rmse 2.2. Each was looked at first: sub-pixel edge resolve. Every other scene was unchanged, max_abs 0.
- **`capture.preview`** GPU time (RTX 5070 Laptop GPU): 0.056 ms at 1280x720 → 768x432, 0.056 ms at 1920x1080 → 768x432, 0.018 ms for a 200x120 region (spec 0.1).
- **`serve_bench --check`.** Three runs, all noisy (Unreal Editor at about a core, Chrome, the search indexer; calibration over the reference by more than 25% in the last).

  | op | budget | measured (last run) |
  |---|---|---|
  | `window_capture_inline` | 20 | 8.78 |
  | `window_readback` | 2 | 1.18 |
  | `window_capture_png` | 30 | 6.93 |
  | `window_step_present` | 8 | 0.28 |

- **`render_bench --check`** (noisy): `cornell_720p` frame 0.853 ms (1.50), `helmet_game_720p` 0.941 (3.0), `helmet_game_1080p` 2.069 (5.0).

## 6. Known gaps

- A capture with a PNG `path`, one that keeps the frame, and every path-traced capture still make the preview on the CPU from the full frame.
- The inline capture's largest remaining part is the JPEG encode of the preview on the CPU (stb, quality 85). The spec leaves it unchanged.
- `framesInFlight()` and `waitForSubmitted()` exist for the capture and its tests; nothing else uses them.

## 7. How to run the new tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ build/Release/serve_bench.exe --check tests/bench/serve_budgets.json > /dev/null   # slow
```
