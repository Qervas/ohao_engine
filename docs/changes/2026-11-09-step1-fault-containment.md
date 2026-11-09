# Step 1: fault containment

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.7 and 5 (step 1); plan `docs/plans/2026-11-09-step1-fault-containment.md`.
**Status:** built, 2026-11-09.

## 1. Summary

A GPU fault was either a process waiting forever (every frame fence and acquire waited `UINT64_MAX`) or went
unnoticed (no `vkQueueSubmit` result was read). Now every submit, fence wait, acquire and present goes through
`gpu/vulkan/gpu_fault.*`: a lost device or a fence unsignalled after 2 s writes one `GPU fault: {...}` line, naming
what each frame in flight ran, and exits 5. Windows cap their frame rate.

## 2. What was built

- `gpu::submit`, `gpu::waitFence` (2 s, `kGpuTimeout`), `gpu::submitAndWait` (replaces each submit +
  `vkQueueWaitIdle`: 17 sites in texture, environment, mesh and acceleration-structure uploads and the readbacks),
  `gpu::checkResult` (acquire, present, the environment's replace idle), `gpu::reportFault` (`std::quick_exit(5)`,
  no Vulkan teardown). The two frame submits pass their frame number.
- `FrameResourceManager::waitForFrame` waits through `gpu::waitFence`. `Swapchain::acquire` waits 2 s; a timeout
  skips the frame (`Acquire::Timeout`), it is not a fault.
- Breadcrumbs: `GpuPassTimer` writes the frame's number with `vkCmdWriteBufferMarkerAMD` at the frame's top and at
  each timed pass's start (`TOP_OF_PIPE`) and end (`BOTTOM_OF_PIPE`) into a host-coherent buffer; `describe()`
  reports per frame in flight: submitted, started on the GPU, passes began and finished, the last finished, the ones
  begun and unfinished. Device creation enables `VK_AMD_buffer_marker` and `VK_EXT_device_fault` (with its feature)
  where present; the report adds `vkGetDeviceFaultInfoEXT`'s description, addresses and vendor records.
- `OHAO_INJECT_GPU_FAULT=lost:N|hang:N`: frame N's submit reports a lost device, or is skipped so its fence never
  signals. The report then carries `"injected": true`.
- The window loop catches what escapes a frame, ends in order, and `RunReport.error` makes `ohao_game` exit 1 and
  `ohao serve --window` exit 3.
- The frame-rate cap: `LiveConfig::maxFps` (default the display's refresh rate, 60 if unknown; 0 none; off under
  fifo), a sleep before the input poll (`runtime/sleep_until.*`: a high-resolution waitable timer on Windows),
  `ohao_game --fps N`, `ohao serve --window --fps N`, `render.stats` `pacing.maxFps`.

## 3. Spec corrections

- The exit code is 5, not 4: `ohao_game` already exits 4 for validation messages.
- "Done when" no longer asks for a real GPU fault: causing one on purpose risks the machine (requirement 1). Its
  report is checked when one happens.
- Not done here: `vkDeviceWaitIdle`'s 48 sites stay unchecked (a hang is caught at the next frame fence within 2 s);
  step 4a removes most of them.

## 4. Results (RTX 4060 Laptop, driver 620.70.06, Fedora 45)

- `OHAO_INJECT_GPU_FAULT=lost:5 ohao run cornell_box --frames 10`: exit 5 after 0.30 s. The report: frame 3 started,
  `cull.instances` the last finished, `gbuffer.early` unfinished; frame 4 submitted, not started; frame 5 not
  submitted.
- `hang:5`: exit 5 after 2.34 s, `"fault": "hang"`, `"where": "frame fence"`: frame 5 submitted, never started;
  frames 6 and 7 all 18 passes finished.
- `vkGetDeviceFaultInfoEXT` on a device that is not lost returns `VK_ERROR_UNKNOWN` (the injected reports say so); its
  content on a real fault is unseen yet.
- `python tools/check.py --labels quick,gpu,golden`: 10 of 10 passed, validation clean with the markers, the 5
  goldens unchanged, no Xid; GPU at most 47.7 C. No submit of any gate reached the 2 s limit (the margin under it
  was not measured).
- The cap, `ohao_game blockout_play.scene.json --mute`, 1280x720, a 240 Hz display:

  | Run | fps | GPU frame | Power | GPU load | Temperature |
  |---|---|---|---|---|---|
  | default (cap 240) | under 80 | | 54.6 W | 98% | 49.4 C avg |
  | `--fps 0` | under 80 | | 55.0 W | 97% | 50.3 C avg |
  | `--fps 60` | 49.9 | 19.5 ms | 55.8 W | 98% | 49.0 C avg |
  | `--fps 30` | 29.9 | 15.9 ms | 46.9 W | 51% | 47.7 C avg |

  The first two hit their 45 s limit before 3,600 frames, so their frame rate is only bounded.

## 5. Known gaps

- On this laptop the default cap does nothing for the play scene: the display is 240 Hz and the GPU makes about 50
  fps (a 19.5 ms GPU frame at 1280x720). The cap saves power only where a frame is cheaper than the display's period,
  as `feel_bench`'s window was on the RTX 5070. The play scene's frame cost is the larger problem for "performance
  like Call of Duty".
- `nvidia-smi --query-gpu=power.draw` read 590 W at idle on this driver; `power.draw.instant` and `nvidia-smi -q`
  agree with each other and were used.
- The Windows waitable-timer path is not compiled or run yet (no Windows build since 2026-11-05).
