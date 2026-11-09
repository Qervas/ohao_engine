# Step 1: fault containment

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.7 and 5 (step 1). **Status:** built, 2026-11-09.

A GPU fault today either hangs the process forever (every frame fence and acquire waits `UINT64_MAX`) or is never
noticed (no `vkQueueSubmit` result is read: 19 sites). Nothing tells us which pass was running. A window renders
uncapped. This step makes a fault a report and an exit, and caps the frame rate.

## What the code has (2026-11-09)

- 19 `vkQueueSubmit` sites, none checked: the two frame submits (`render_dispatch.cpp:220,413`), `readImageRGBA`
  (`:669`), ten readbacks in `vulkan_renderer.cpp` (each submit then `vkQueueWaitIdle`), uploads in
  `bindless_texture_manager.cpp` (2), `env_upload.cpp`, `rt_acceleration_structure.cpp`, `mesh_registry.cpp`.
- 17 `vkQueueWaitIdle` (no timeout exists for it), 48 `vkDeviceWaitIdle`.
- `FrameResourceManager::waitForFrame` defaults to `UINT64_MAX` and its result is ignored at the frame's start
  (`render_dispatch.cpp:108,318`). `Swapchain::acquire` waits `UINT64_MAX`.
- `GpuPassTimer` brackets every registered pass with `BOTTOM_OF_PIPE` timestamps: the place for breadcrumbs.
- The RTX 4060 (driver 620.70) has `VK_AMD_buffer_marker`, `VK_NV_device_diagnostic_checkpoints` and
  `VK_EXT_device_fault`; the Intel iGPU has `VK_AMD_buffer_marker`.
- No frame-rate cap anywhere. Mailbox (the default) and immediate render as fast as the GPU goes.

## Design

**One file owns GPU calls that can fail:** `ohao/gpu/vulkan/gpu_fault.{hpp,cpp}`.

- `submit(queue, submitInfo, fence, where)`: checks the result. `VK_ERROR_DEVICE_LOST` goes to `deviceLost`.
- `submitAndWait(queue, cmd, where)`: a submit with a fence, then `waitFence`. It replaces every submit followed by
  `vkQueueWaitIdle` (the readbacks and uploads), so those gain the timeout too.
- `waitFence(device, fence, where)`: waits `kGpuTimeout`, 2 s, Windows' TDR delay. `VK_TIMEOUT` is a hang and goes to
  `deviceLost` with kind `hang`. A submit that needs more than 2 s is an unbounded dispatch (spec 4.7); on Windows it
  would be reset anyway.
- `present` and `acquire` results: `VK_ERROR_DEVICE_LOST` goes to `deviceLost`. Acquire waits at most 2 s; a timeout
  skips the frame (a hidden window can hold its images), it is not a fault.
- `[[noreturn]] deviceLost(kind, where)`: writes the report, flushes the log, exits with code 5 through `std::quick_exit`
  (`ohao` uses 1-3, `ohao_game` 1-4). No Vulkan teardown after a lost device: drivers can hang in it, and the OS reclaims everything.
  The report is one `Error` line on the `gpu` channel, a JSON object: `kind` (`device_lost` or `hang`), `where`, the
  frame number, the breadcrumbs, the `VK_EXT_device_fault` description and address records when the device has it.
  Agents read it from stderr or `log.read`; `tools/check.py` already keeps a failing test's last 40 lines.

**Breadcrumbs** (`VK_AMD_buffer_marker`: one mechanism on NVIDIA, Intel and AMD). A host-coherent buffer, one record
per frame slot: the frame number, and two words per pass. `GpuPassTimer::begin` writes the pass's index at
`TOP_OF_PIPE`, `end` at `BOTTOM_OF_PIPE`. After a fault the report lists, for each frame in flight, the passes that
began and did not end (the culprit) and the last pass that ended. Without the extension the report says so and
lists the passes recorded.

**The window loop** catches what escapes a frame (`std::exception`), logs it and shuts down in order. `deviceLost`
never returns, so it is not caught.

**Frame-rate cap.** `LiveConfig::maxFps`: 0 uncapped; default -1, the display's refresh rate
(`glfwGetVideoMode`), or 60 when unknown. Under fifo the cap is off (vsync already limits). The loop sleeps before it
polls input, so input stays as fresh as before: the frame starts later, and its poll with it. On Windows the sleep
is a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), elsewhere `sleep_until`.
`ohao_game --fps N` and `ohao serve --window --fps N` set it; `--fps 0` is uncapped. `render.stats` reports the cap.

**Fault injection, for the proof only:** `OHAO_INJECT_GPU_FAULT=lost:N` makes the N-th frame submit return
`VK_ERROR_DEVICE_LOST` without submitting; `hang:N` waits on a fence that was never submitted at frame N. Neither touches
the GPU. One parse at startup, one branch in `submit` and `waitFence`.

## Not in this step

- `vkDeviceWaitIdle`'s 48 sites: a hang is caught at the next frame's fence within 2 s. Resize and teardown idles
  stay unchecked until step 4a removes most of them.
- `VK_NV_device_diagnostic_checkpoints` and Aftermath: the buffer markers name the pass, which is what we lack.
- A real GPU fault on purpose (an out-of-bounds address, a spin): it risks the machine (requirement 1). The spec's
  "a real GPU fault does the same" is checked when one happens; the spec is corrected to say so.

## Steps

1. `gpu_fault` with `submit`, `submitAndWait`, `waitFence`, `deviceLost`, the report and the injection. Route all 19
   submits through it; `submitAndWait` replaces each submit + `vkQueueWaitIdle` pair. Frame fences and acquire get
   their timeouts and checked results.
2. Breadcrumbs in `GpuPassTimer` (enable `VK_AMD_buffer_marker` and `VK_EXT_device_fault` where present, at device
   creation).
3. The window loop's catch.
4. The frame-rate cap, its flags, `render.stats`.
5. Docs: AGENTS.md's GSP gotcha points at the report; `ohao-cli.md` gets exit code 5; the spec's
   "done when" says how a real fault is checked.

## Done when (measured, recorded in `docs/changes/2026-11-09-step1-fault-containment.md`)

- `OHAO_INJECT_GPU_FAULT=lost:5 ohao run` on Cornell: exit 5, a report naming frame 5 and the last pass frame 4
  finished. Same with `hang:5`: exit 5 within 2.5 s, kind `hang`. Headless, no window.
- The longest single submit across the gates (pass timer `gpuMs`, `render.capture {timings}`) is under 1 s, so the
  2 s timeout cannot fire on legitimate work. If one is not, it is split before the timeout lands.
- One visible run of `ohao_game` on the play scene: the frame rate at the display's refresh rate, and GPU power and
  temperature (nvidia-smi, 60 s) against the same run with `--fps 0`.
- `python tools/check.py` green: validation clean (the markers add commands), goldens unchanged (markers and the cap
  change no pixel).
