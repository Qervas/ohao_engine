# Sub-project 2b, step 2: readback on request everywhere

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Readback: the frame just rendered", "Test plan" test 3, "Build order" step 2, as built)
**Status:** Built. Every functional test passes: `tools/check.py` quick + gpu 32/32 and docs 2/2. Render goldens 11/11: the two deferred example goldens were re-baselined in their own commit, and all five deferred scenes are exact. Validation is clean, and the presentation gate reports 0 unrequested readbacks. `render_bench` was noisy twice (another process's `python.exe` at 98–100% of a core, Unreal Editor at about 40%) and is reported as noisy; no budget is gated before step 7. No budget or tolerance was changed.

## 1. Summary

A frame reaches the CPU one way now: `requestReadback()` before the `render()` you want, `readback()` after, waiting on that frame's fence alone.

- **Deleted:** the headless ring, which copied every headless frame and handed it back three `render()` calls later, together with `getPixels()`, `getPixelSpan()`, `getPixelBufferSize()`, `readbackCopyCount()` and the CPU pixel buffer.
- **Moved to `readback()`:** the path tracer's CPU-side denoisers (OIDN, NRD), which now run on the requested frame.
- **Readers moved to requests:** the examples, `ohao doctor`, the capture procedure and the tests. They write the frame they render last. The two deferred example goldens therefore move, from frame 7 to 10 and from 27 to 30, and the CLI's matching manifest entries move with them.

## 2. What was built

- **`VulkanRenderer` (`render_dispatch.cpp`, `renderer.{hpp,cpp}`):**
  - A headless deferred frame copies its final image into its slot's staging buffer only when requested (`copyDeferredOutputToStaging`, formerly `copyDeferredOutputToPixelBuffer`), and is timed as `readback` then. Otherwise it ends with its image.
  - After the copy the image goes back to `SHADER_READ_ONLY_OPTIMAL`, as the window's blit already did, so a frame ends in the same state whether or not it was read.
  - The path tracer copies its output only for a requested frame.
  - Neither frame body memcpys a staging buffer any more.
  - `readback()` returns the requested frame's staging copy. On a path-traced frame under OIDN or NRD it first runs `denoiseRequestedFrame()` (formerly the `getPixels()` branches, no longer `const`/`mutable`) and returns the denoised image, falling back to the noisy frame with an error logged.
  - `requestedReadbackCount()` counts copies, all of which were requested.
- **`LiveHost`:** it counts its own requests (each realtime capture, each `captureFrame`). Its `RunReport::readbackCopies` and the shutdown line's "unrequested" are the copies beyond those requests, which `ohao_game --frames`, `check_live.py` and `e2e_mcp.py` require to be 0.
- **`runtime::captureFrame`:** one read path, `readback()`, whatever the denoiser.
- **Examples:**
  - `cornell_box`, `model_viewer` (its video loop included), `turntable` and `env_demo` (its `--seq` dump included) request their last frame and read it back.
  - The RT loops render `spp` frames, not `spp + 3`.
  - `env_demo --pan-x` renders `samples - 1` frames, pans, then renders and reads one more.
- **`ohao doctor`:** the `gpu.wedged` frame is requested and read back, so the check times the 64x64 frame to its fence. It used to pass on any submitted frame, since the pixel buffer always existed.
- **Tests moved to requests:**
  - `engine_tests`: the resize test, and the two path-tracer tests (which now also check the readback's size).
  - `instantiate_gpu_test`, `scene_view_gpu_test` and `renderer_test` (whose canary now checks the size).
- **Test 3:** `engine_tests`' `sp2b_t03_frame_n_readback_is_frame_n_and_only_requested_frames_copy`.
  - **Scene:** a cube spinning 600°/s (10° a step), a light, the camera.
  - **Exactness:** a renderer stepping the world 9 times, resetting its temporal state before frame 9 and reading frame 9 back, equals a fresh renderer's single capture of step 9.
  - **Guard:** the test first checks that step 9 and step 6 differ by more than 32, so a 3-frame-late readback cannot pass.
  - **Counts:** 9 frames with one request make exactly 1 copy; an unrequested frame after it reads back empty and copies nothing.
  - **Timings:** they stay in frame order across the reset.
- **Goldens:** `cornell_box_deferred` and `damaged_helmet_deferred` re-baselined. `cli_cornell_box_deferred` moves to `--frames 10` and `cli_damaged_helmet_deferred` to `--frames 30`.

## 3. Bugs found and fixed

1. **`resetAccumulation()` dropped pass timings out of order.** It idles the device and restarts the frame slots at 0 without collecting the pass timer's pending slots (`resize()` does collect them). Their older frames then entered the 120-frame ring after newer ones, and every capture resets. It now collects first; test 3 checks the order across a reset.
2. **The headless copy left the final image in `TRANSFER_SRC_OPTIMAL`, and the window's blit restored it.** This was harmless only because both producing render passes start from `UNDEFINED`. Both now leave it in `SHADER_READ_ONLY_OPTIMAL`.
3. **`ohao doctor`'s wedged-GPU check could not fail on a frame.** Its "image" was the CPU pixel buffer, which always existed. It now reads the frame back.
4. **Monograph:**
   - `graph/frame-resources` said the staging buffers never request `HOST_CACHED`; they have since sub-project 4 step 9.
   - `systems/tests` called the deferred helmet golden "looser, not bit-exact"; it has been exact since sub-project 4 step 1.
   - Both corrected, with the units that described the ring, `getPixels()` and the `+ 3` (ten in all, plus `denoise/nrd`, `denoise/types`, two module pages and three generator strings).
   - The two figures of the deleted ring were removed.

## 4. Spec corrections

- **CPU denoisers:** they run inside `readback()` on the requested frame, rather than as a separate capture path (spec "One way to read").
- **`readbackCopyCount()`:** removed along with `getPixels()`/`getPixelSpan()`. The gates' "0 unrequested" is the window host's copies minus its requests.
- **Path-traced goldens:** they do not move. `spp` frames read at the last is the frame the ring delivered after `spp + 3` (both carry `spp` samples), so only the two deferred example goldens are re-baselined. The spec had listed "the RT examples' last sample pass" among those that change.

## 5. Results

### Goldens

Re-baselined after looking at them:

| Scene | Change | Measured |
|---|---|---|
| `cornell_box_deferred` | frame 10 instead of 7 | max_abs 3 on 36.8% of pixels (84,765 px) |
| `damaged_helmet_deferred` | frame 30 instead of 27 | max_abs 9 on 39.4% (90,667 px) |

The helmet's worst region shows scattered few-LSB noise on the lit wall, where the RT GI and TAA histories are three frames further on. Both new images equal `ohao run --frames 10` and `--frames 30` bit for bit (checked before re-baselining).

After the re-baseline, all 11 scenes pass. The deferred scenes and their CLI twins are exact (max_abs 0). The path-traced scenes are unchanged within their tolerances: helmet max_abs 1; `rt_realtime` 2 and 3, the realtime tracer's known run-to-run race.

### Tests

- `engine_tests` 60/60, with the new `sp2b_t03`: max_abs 0 against step N, 243 against step N−3.
- `check.py` quick + gpu 32/32 (`check_cli` 29/29 and 25/25 CPU, `check_live` 26/26, `e2e_mcp` 20/20, `deferred_determinism_gpu_test` 43/43, `instantiate_gpu_test` 5/5, `scene_view_gpu_test` 7/7, `renderer_test` 4/4).
- Docs 2/2 (`check_docs` 65 commands), monograph citations 98/98 units.
- `check_validation` 2/2 clean.
- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200`: 0 unrequested readbacks, 0 validation messages through teardown.
- NRD through `readback()`, by hand: `cornell_box out.png 8 rt_realtime --denoise=nrd` wrote a denoised Cornell frame (values 0–247).

### Benchmarks

`render_bench`: noisy twice, both times with `python.exe` at 98–100% of a core and Unreal Editor at about 40%. These are no verdict.

What changed is structural:

- `readback` now runs on 1 frame of a scene's 100, the requested last one.
- A frame's span equals its rendering: `cornell_720p` 4.862 ms both, where step 1's clean baseline had span 5.076 against frame 4.866, a 0.21 ms copy on every headless frame.
- GPU calibration ×0.93 and ×0.96, uncontended.

## 6. Known gaps

- OIDN is not in this build (`OpenImageDenoise: not found`), so its path through `readback()` was exercised only as far as its fallback, the noisy frame. NRD was exercised by hand.
- Scratch files from this step remain in `build/`: `t03_debug.png`, `nrd_check*.png` and `golden_check_*.png`. They are untracked; `rm` was refused.

## 7. Running the tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200 > /dev/null   # gpu
```
