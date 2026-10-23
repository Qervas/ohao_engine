# Sub-project 4, step 9: the live host and capture on request

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Live host" and "Frame capture", as built; the host interface note; "Budgets", the GPU part as built; "Test plan" tests 16 and 17; "Build order" step 9)
**Status:** Built. Every test passes; two GPU budgets (readback, inline capture) miss on the frame's own GPU cost and wait for Frank's ruling (section 7). No budget was changed.

## 1. Summary

The engine now runs as a live host. `ohao serve --window` opens a window, plays the scene in lockstep or realtime, and answers protocol requests between frames. An agent can ask for a frame at any point: the capture is rendered on the window's own renderer, and it can come back inline as an image or be written to a PNG file. `ohao_game` plays a scene file now, not code. The headless capture and the window capture share one procedure, so a lockstep window capture equals a headless capture of the same world bit for bit. Two GPU budgets miss on the frame's own GPU time; no budget was changed.

## 2. What was built

- `runtime::LiveHost` (`ohao/runtime/live_host.{hpp,cpp}`) replaces `Application`, which is deleted. It owns the window, the renderer, the asset resolver, the World (instantiate and render extract installed) and an `api::Session` that shares its Timeline. It is the session's `api::Host`. Each frame:
  1. Poll the window.
  2. Pump the protocol requests (`RequestPump`: the server's `poll(wait)`, waiting up to 16 ms while nothing steps, 0 in realtime).
  3. Run the timeline's steps: each is `Session::step(events, fixedDt)`, then the game's `fixedUpdate` hook.
  4. Run the `update` hook.
  5. Render and present, only when due: every frame in realtime; in lockstep or paused only when `Session::changeCount()` moved or the window resized.
  6. `flush`, so a reply is sent after the frame its request caused was presented.

  While nothing steps, the window's device input is dropped, and the next step starts with a focus loss.
- `rpc::Server` gained `start`, `poll(wait)`, `flush`, `stop` and `ServerOptions::holdReplies`. `run()` is built from them. `stop` interrupts a read the client left open (`Transport::interruptRead`, CancelSynchronousIo on Windows), so closing the window ends the session. The reader thread is named "ohao rpc reader".
- `ohao serve --window [WxH]` (default 1280x720), with `--lockstep` (default) or `--realtime`, `--vsync` and `--hidden`.
- `api::Host {kind(), capture()}` and `Session::setHost` replace `setRenderHandler`. `Session::changeCount()` is new. `api::CallOutcome::image` carries an image beside the result.
- `runtime::captureFrame` (`ohao/runtime/frame_capture.cpp`) is the one capture procedure that both the headless `host::RenderHost` and `LiveHost` run: scene view, mode, denoiser, post and exposure, a temporal reset unless the renderer is fresh, `frames` frames each after `World::sync()`, the last frame read back on request, then region, PNG file and preview. A lockstep window capture keeps the window's own post settings and leaves its last frame on screen.
- `render.capture` params: `path` (optional); `inline`; `format` (jpeg or png); `maxSize` (default 768); `region` [x, y, w, h]; `width` and `height` (optional). Headless default is 1920x1080. A window host uses its own size; another size is `E_COMMAND`. A path-traced mode in a window host is `E_COMMAND`. A minimized window is `E_STATE`. The result is `{path?, width, height, imageSize?, frames, ms, readbackMs, encodeMs, deterministic, validation?}`. The inline image is an MCP `image` content block; natively it is `"image"` (base64) with `"mimeType"`. Default frames: deferred 1, or 10 with TAA; path tracer `spp`.
- Readback on request: `VulkanRenderer::requestReadback()` and `readback()`. They use the frame slot's persistently mapped staging buffer and wait only for that frame's fence. A window frame copies only when asked. `readbackCopyCount()` still counts the headless ring (0 in a window). `requestedReadbackCount()` counts the copies that were asked for.
- Capture images (`ohao/runtime/image.cpp`): crop, an area downscale, stb's JPEG at quality 85, and OHAO's own PNG writer. The PNG writer uses filter Up and fixed-Huffman deflate, with runs written as distance-1 matches. It works in four bands of rows on four threads; each band is byte-aligned by an empty stored block, and the Adler-32 values are combined. A band that does not compress is written as stored blocks.
- `ohao_game` plays `assets/scenes/helmet_game.scene.json`. The scene is the helmet room of `damaged_helmet.scene.json`, plus a spinning cube (BoxPrim and Spin, 68.75 deg/s about [0.3, 1, 0.2]), the MainCamera with a FlyController, and an InputActions entity (W/S, D/A and E/Q axes; right mouse look, LeftShift fast, Escape quit). Only host settings stay in code: bloom, TAA, SSAO on, exposure 1, and quit and cursor capture in its hooks.
- `tools/ohao/scene_host.hpp` moved to `ohao/runtime/scene_view_apply.hpp` (`runtime::applySceneView`).
- serve_bench GPU part: a second child, `ohao serve --window 1280x720 --hidden`, on Cornell (lockstep, no post). Five warm-up rounds and 50 measured rounds, each of five `world.step` calls, one inline capture and one PNG capture. Readback is the captures' `readbackMs`. The reader thread's CPU is measured over at least 600 idle realtime steps with QueryThreadCycleTime. The part writes its own load record. Budgets added: `window_step_present_ms` 8, `window_readback_ms` 2, `window_capture_inline_ms` 20, `window_capture_png_ms` 30, `reader_cpu_idle_600_ms` 1.
- Tests:
  - `tests/serve/check_live.py` (label gpu, 22 checks): tests 16 and 17.
  - api_tests `sp4_t16` x2.
  - rpc_tests `sp4_t16` x3.
  - runtime_tests `sp4_t16` x2. One round-trips the PNG writer through stb_image for a frame, covering all noise and odd sizes; the other covers crop, downscale and JPEG.
- Docs: AGENTS.md (suite table, check_live, the GPU bench, gotchas on readback and capture frames); docs/ARCHITECTURE.md (libraries, the frame loop, LiveHost); docs/agents/ohao-cli.md (`serve --window`, images, the `render.capture` row); the spec as built; 20 monograph units updated (Application, the code-built ohao_game, scene_host.hpp, setRenderHandler). Site rebuilt: 92 leaf pages.

## 3. Bugs found and fixed

1. Headless captures returned a frame rendered three frames before the last. Every headless frame copies its image into its slot's staging buffer, and `getPixelSpan()` returns the copy made `MAX_FRAMES_IN_FLIGHT` (3) frames earlier. A 10-frame deferred capture was therefore frame 7, and a capture of 3 frames or fewer was black. Captures now keep the last frame they render. The old path-tracer default `spp + 3` existed to compensate; it is now `spp`. The deferred CLI goldens now say `--frames 7` and `--frames 27`, the frames the examples write. All 10 render goldens pass unchanged.
2. Readback staging buffers were uncached host-visible memory. Reading one 1280x720 frame took about 20 ms, and every headless frame paid it at frame start: a one-frame headless capture took about 45 ms of CPU, now about 1 ms. The buffers are now host-cached (coherent) when the device offers it.
3. stb_image_write's PNG took 7.3 s for one 1280x720 Cornell frame (encodeMs 7332). The new writer takes 4.1 ms (mixed synthetic frame) and 6.1 ms (all noise), and is lossless.
4. A forced redraw after each lockstep capture left a frame in flight, which the next capture's reset had to wait for (4.5 to 16 ms). The redraw is removed; the capture's own last frame is on screen.

## 4. Spec corrections and rulings

- "The host interface": there is no `Host::beforeSteps` (already dropped in step 8). Host is `{kind, capture}`.
- The `fixedUpdate` hook runs after each step, since `Session::step` is one call.
- The path tracer's default is `spp` frames, not `spp + 3` (the readback lag, Bug 1).
- "No temporal effect" without TAA is not true of the hybrid: RT GI keeps a history. Cornell frame 1 and frame 10 differ by max_abs 29, with 8% of pixels above 4. The defaults stay as specified (1 frame, or 10 with TAA). A converged image needs `frames`.
- A window host captures deferred only: the path tracer does not present.
- Ruling (ledgered): the capture PNG uses OHAO's own encoder instead of fpng. The spec's named fallback needs the network and a new dependency.
- Encoding is not moved off-thread. It takes a few ms, and the reply waits for it anyway.

## 5. Results

- `python tools/check.py --all`: build 13.1 s; 37 tests, 35 passed. scene_bench and serve_bench were noisy in all three attempts (outside load), so they are not counted as passes.
- Per-test counts from that run: check_cli_cpu 25, check_scene_schema 17, check_tests 8, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 143, check_cli 29, check_validation 2, check_live 22, render_golden 10, check_docs 59, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 29, ecs_tests 26, reflect_tests 37, scene_tests 60, api_tests 41, systems_tests 9, rpc_tests 13, log_tests 10. ohao_game_presentation, monograph_structure and reflect_exhaustive_floats passed.
- New in this step: api_tests 2, rpc_tests 3, runtime_tests 2 (`sp4_t16_*`), check_live 22.
- Render goldens 10/10 (the four deferred scenes exact; cli_cornell_box max_abs 0 at 16 frames).
- `ohao_game` presentation gate from the scene file: 600 frames, 149.5 fps, 0 readback copies, 0 validation messages including teardown.
- check_live: a lockstep window capture equals a headless capture of the same world bit for bit, with post effects off and on. The fly camera moved [0, -6, 25.5] to [0, -6, 22.500046] in 60 steps (3 m expected). Closing stdin during a 1,000,000-step `world.step` exits 0 in 0.3 s. Validation 0 and 0 unrequested readbacks through teardown.
- Goldens updated after reading the diffs: `tests/cli/expected` (schema.out, run_script.out) and `tests/serve/expected` (errors, mcp, native). Only schemaHash changed, from d3fb06fdb4aa5d0a to 2f1413bfc516b03d. `tools/list` is 14,509 bytes for 22 tools (budget 24,576 bytes).

### Benchmark

serve_bench GPU part, by run. Every run was noisy (other processes above half a core); none is counted as a pass. Values are in milliseconds. Budgets: window_step_present 8 ms, window_readback 2 ms, window_capture_inline 20 ms, window_capture_png 30 ms, reader_cpu_idle_600 1 ms.

| run | outside load | window_step_present (ms) | window_readback (ms) | window_capture_inline (ms) | window_capture_png (ms) | reader_cpu_idle_600 (ms) |
|---|---|---|---|---|---|---|
| A | python.exe 85.5% | 0.4322 | 5.4397 | 30.3340 | 21.5188 | 0.0000 (GetThreadTimes, 15.625 ms ticks; replaced) |
| B | python.exe 86.5% | 0.4977 | 5.3901 | 30.8732 | 21.8648 | 0.0000 (ticks) |
| C | UnrealEditor 227.9% | 0.4386 | 5.4408 | 26.7941 | 16.4807 | 15.6250 (one tick; why GetThreadTimes was replaced) |
| D | UnrealEditor 295.0% | 0.5611 | 8.2779 | 55.2329 | 25.1772 | 0.0780 (QueryThreadCycleTime) |
| gate attempt 3 | python.exe 88.6%, UnrealEditor 41.4% | 0.3851 | 5.4701 | 27.5478 | 16.3646 | 0.1375 |

- The CPU operations in the same runs stayed as in step 7 (gate attempt 3: world_hash_rpc 0.5624 ms, world_diff_1pct_rpc 0.4859 ms, world_info_rpc 0.0228 ms).
- scene_bench (gate attempt 3, noisy: python.exe 80.5%, UnrealEditor 40.9%, 27 of 50 rounds contended): every op's median was within budget (save 9.145 ms, load 13.238 ms, validate 8.284 ms).
- Capture breakdown, measured with the debug log in the window host with the GPU idle before each capture: inline capture 12 to 15 ms end to end (reset 0.05 ms, submit 0.4 ms, readback 5.2 to 5.8 ms, scale about 3 ms, JPEG about 3.5 ms). PNG capture 14 to 15 ms. The renderer's timestamps put one Cornell deferred 1280x720 frame at 5.0 to 5.6 ms of GPU time.

## 6. Known gaps

- The DamagedHelmet lies on its back in the helmet room (seen from above) in `damaged_helmet.scene.json`, in its golden and in `helmet_game` alike. The glTF's root rotation appears not to be applied on load. This is pre-existing and was not changed here.
- serve_bench has no non-noisy run in this step. The machine was shared with UnrealEditor and another session's python.
- RT-mode captures in a window host are refused, since the path tracer has no present path.
- The examples (cornell_box, model_viewer) still read the lagged ring (`getPixelSpan`), and their goldens rely on it.

## 7. For Frank: two budgets

- `window_readback` (budget 2 ms; measured 5.39 to 5.47 ms in the non-UnrealEditor runs). The readback waits for that frame's fence, and the frame's GPU work is 5.0 to 5.6 ms (RT shadows at 8 rays per pixel and RT GI at 32 samples per pixel, 1280x720). The readback itself (the copy inside the frame and the copy out of cached memory) adds about 0.3 ms.
- `window_capture_inline` (budget 20 ms; measured 26.8 to 30.9 ms). In the bench each capture follows five `world.step` calls whose frames are still on the GPU (10 to 15 ms of backlog), plus its own frame. With an idle GPU first, the capture measures 12 to 15 ms.

Options:

- (a) Measure the readback as its own cost beyond the frame (about 0.3 ms), and the capture from an idle GPU (12 to 15 ms). This restates the budgets' definitions.
- (b) Keep the definitions and the numbers, and make the deferred frame cheaper in step 2b (fewer RT GI samples, half-resolution GI). This changes the deferred goldens.
- (c) Set the GPU budgets relative to the frame's GPU time.

No budget was changed by this step.

## 8. Running the tests

```
python tests/serve/check_live.py
build/Release/runtime_tests.exe
build/Release/api_tests.exe
build/Release/rpc_tests.exe
OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200
build/Release/serve_bench.exe --check tests/bench/serve_budgets.json
python tools/check.py --all
```
