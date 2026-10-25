# Sub-project 2b, step 14: path-traced frames in a window

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Path-traced frames in a window", "Build order" step 14, as built; test plan test 11)
**Status:** Built and tested. `window_capture_rt_inline_ms` is recorded, not gated (it gets a budget by Frank's ruling). Every `serve_bench` run was flagged noisy by outside load, so the window budgets have no fresh verdict; their medians stayed within budget in every run (section 5).
- A window presents path-traced frames: `renderRTPipeline` acquires a swapchain image, blits the tracer's output into it and presents, as the deferred path does.
- `LiveHost::capture` takes `rt_realtime` and `rt_offline`:
  - in lockstep, any mode, presenting every sample pass, equal to a headless capture (max_abs 0 measured);
  - in realtime, the next presented frame in the window's live mode.
- A window's live mode is a launch option. `ohao serve --window --mode rt_offline --spp N` presents its accumulation until N samples after each change.
- Captures are cancellable between frames and report progress, in both hosts.
- Test 11 (`check_live.py`): 42/42 with validation on (14 new checks). Goldens 14/14 unchanged.

## 1. Summary

- **RT present path** (`render_dispatch.cpp`):
  - The RT frame rebuilds a dirty swapchain before taking its slot, and acquires after the fence wait but before the fence reset (so every early return leaves the fence signalled).
  - It records `blitRTOutputToSwapchain`: the tracer's RGBA8 output, already in `TRANSFER_SRC_OPTIMAL`, is blitted into the swapchain image. A requested readback copies the same source into the slot's staging buffer in the same pass.
  - It submits with the slot's image-acquired and the image's render-finished semaphores, then presents.
  - Headless frames are unchanged: they copy only when asked (`pt.readback`).
- **Pass registry:** `pt.present` (transfer), after `pt.render`. The window's RT blit is timed there, not under `present`, because the registry is in frame order and `present` precedes the tracer.
- **Window host** (`LiveHost`):
  - `LiveConfig::mode` and `spp` set what the window presents; without ray tracing it stays deferred, with a warning.
  - A lockstep capture saves the live mode, denoiser, quality and post settings, runs `captureFrame` in the requested mode, then restores them all. Switching back resets the live mode's accumulation.
  - A realtime capture must name the live mode. A path-traced realtime capture is undenoised: `denoise` other than `none` is `E_COMMAND`.
  - When the world changed, the live loop calls `notifyCameraChanged()` so the path tracer restarts its accumulation.
  - A lockstep or paused `rt_offline` window renders until `spp` samples have accumulated since the last change.
- **Cancel and progress:**
  - `RenderRequest::control` carries the call's `CallControl`.
  - `captureFrame` checks the cancel flag before each frame after the first, and reports `progress(frames rendered, frames)` after each one. The RPC server throttles progress to one notification per 250 ms.
  - A cancelled outcome (`RenderOutcome::cancelled`) reads nothing back and makes the call `Cancelled`, so it gets no answer.
- **Launch options:**
  - `ohao serve --window` takes `--mode deferred|rt_realtime|rt_offline` and `--spp N` (1-65536, default 64); both need `--window`.
  - `ohao_game` takes `--mode`.
- **`serve_bench`** records `window_capture_rt_inline_ms`: Cornell, 1280x720, `rt_offline`, 16 spp, `denoise: none`, inline, 3 warm-ups and 25 timed captures. It is printed and written under `recorded`.
- The `render.capture` description now says a window presents each frame, uses its live mode in realtime, and can be cancelled. `schemaHash` moved, so the serve and CLI goldens holding it were updated (only those lines changed).

## 2. What was built

| File | Change |
|---|---|
| `ohao/gpu/vulkan/render_dispatch.cpp`, `renderer.hpp` | swapchain prologue, `blitRTOutputToSwapchain`, `copyRTOutputToStaging`, semaphored submit and present for RT frames |
| `ohao/render/frame/pass_registry.hpp` | `pt.present` |
| `ohao/api/session.hpp`, `session.cpp` | `RenderRequest::control`, `RenderOutcome::cancelled`; `render.capture` passes the call's control and returns `Cancelled` |
| `ohao/runtime/frame_capture.cpp` | the cancel check and progress in the frame loop |
| `ohao/runtime/live_host.{hpp,cpp}` | live mode and `spp`; RT captures; live state restored; progressive `rt_offline` |
| `tools/ohao/main.cpp`, `examples/ohao_game.cpp` | `--mode`, `--spp` |
| `tools/bench/serve_bench.cpp` | `window_capture_rt_inline_ms` |
| `tools/bench/render_bench.cpp` | `pt.present` counts as a copy out (outside the `frame` budget) |
| `tests/serve/check_live.py` | test 11 |

## 3. Bugs found and fixed

- None in shipped code beyond the gap the spec names (a window could neither show nor capture path-traced frames).
- While building test 11: a capture's `spp` is capped at 65,536, so the cancel test's first 100,000-sample request was refused with `E_RANGE`; it now uses 60,000.

## 4. Spec corrections

1. The window's live mode is a launch option (`--mode`), because the spec named none and `render.settings` has no mode. A lockstep `rt_offline` window converges to `--spp` samples, then stops presenting.
2. The window's RT blit is its own pass, `pt.present`.
3. Cancel and progress travel on `RenderRequest::control` and are checked in `captureFrame`, so headless captures are cancellable too.
4. A realtime window capture must name the window's live mode, and a path-traced one is captured as presented, undenoised.

## 5. Results

- **Test 11** (`check_live.py`, validation on), 14 checks:
  - lockstep `rt_offline` 16 spp: 16 frames, deterministic, passes `scene.upload`, `pt.render`, `pt.present`, 114 ms at 640x360;
  - lockstep `rt_realtime`: 8 frames; the window is deferred again afterwards;
  - window vs headless, same world: `rt_offline` max_abs 0 (0 channels differ), `rt_realtime` max_abs 0;
  - a 60,000-sample capture cancelled after 1.5 s: no answer, 5 progress notifications (the last at 545 of 60,000), then another capture renders;
  - `--mode rt_offline --spp 24`: 24 frames presented, then the newest frame stays at 24; a `world.step` brings 24 more;
  - realtime `--mode rt_realtime`: the capture is `deterministic: false`, 1 frame; a deferred capture is `E_COMMAND`;
  - every window exits 0, with validation 0 including teardown and 0 unrequested readbacks.
- `check_live.py` total: 42/42.
- **Presentation gate with the path tracer:** `OHAO_VALIDATION=1 ohao_game --frames 300 --resize-at 100 --mode rt_realtime` and `--mode rt_offline` each gave 0 validation messages including teardown and 0 readback copies (39 and 57 fps under the validation layers).
- **`tools/check.py`:** 37/37 after the docs (section 7).
- **Goldens:** 14/14, max_abs 0 on the deferred scenes; RT within its floor.
- **`serve_bench`**, two runs, both flagged noisy:

| Run | Load | `window_readback` | `window_capture_inline` | `window_capture_png` | `window_step_present` | `window_capture_rt_inline_ms` median / p95 |
|---|---|---|---|---|---|---|
| 1 | 0.66 cores | 1.31 ms | 8.63 ms | 8.07 ms | 0.34 ms | 128.0 / 130.5 ms |
| 2 | CPU calibration x1.94, 1.2 cores | 1.31 ms | 9.81 ms | 9.46 ms | 0.42 ms | 117.7 / 121.3 ms |

  Budgets: readback 2, inline 20, PNG 30, step-present 8 (ms). `tools_list_bytes` is 15,351 B (budget 24,576).

## 6. Known gaps

- `render.settings` (step 15) does not switch the window's live mode; it is fixed at launch.
- A realtime window cannot capture a denoised path-traced frame; capture in lockstep for one.
- `window_capture_rt_inline_ms` has no budget until Frank rules one. At 1280x720 a sample pass costs about 7-8 ms with its present.

## 7. How to run the new tests

```
python tests/serve/check_live.py                          # tests 16, 17, 15 and 11 (needs a GPU and Pillow)
build/Release/serve_bench.exe --check tests/bench/serve_budgets.json   # records window_capture_rt_inline_ms
OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 300 --resize-at 100 --mode rt_realtime
build/Release/ohao.exe serve --window --mode rt_offline --spp 256 assets/scenes/cornell_box.scene.json
```

## 8. Commits

- `4eb3f89` feat(render): path-traced frames in a window: the RT present path, RT captures in the window host, cancel and progress
- this record, the spec as built, AGENTS.md, ARCHITECTURE.md, the CLI and serve guides, and the monograph
