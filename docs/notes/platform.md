# Platforms, the build and the machine

Windows and Linux specifics, the build's traps, the GPU's failure modes on the reference laptop, and the frozen
monograph. Moved from `AGENTS.md` (system design step 2h, 2026-11-09), unchanged.

- On Windows conda's `cmake` shadows the real one; use the scoop shim (above). `tools/check.py`
  refuses conda's.
- **Git Bash (Windows) rewrites arguments that start with `/`** into Windows paths. Write
  `ohao query --fields Transform/position`, not `/Transform/position`.
- **Line endings:** `core.autocrlf` is on. Byte-compared files (goldens, scene files,
  CLI expectations) are pinned to LF in `.gitattributes`; add a pattern there for any new
  byte-compared directory, or a fresh clone checks out CRLF and the comparison fails.
- `/FORCE:MULTIPLE` (MSVC) and `--allow-multiple-definition` (GNU ld, set for every target in the root
  `CMakeLists.txt`) hide duplicate symbols: a second VMA inside `NRI_VK` (stb_image is implemented once, `external/stb_image.cpp`).
  Which copy wins depends on link order, so `gpu_allocator.cpp` uses NRI's VMA
  configuration; never change one without the other. A new target that crashes right
  after "resizing to WxH" has picked a mismatched VMA.
- `GLM_ENABLE_EXPERIMENTAL` before any `<glm/gtx/...>` include.
- Restoring a file from a backup copy keeps the backup's older mtime, so MSBuild keeps
  the objects built from the edited version: `touch` sources after a restore.
- `build/ohao` is the CMake binary directory of `ohao/`, not the CLI; the CLI is
  `build/Release/ohao.exe`.
- After a crash the GPU can stay wedged (device lost `-4` everywhere, 30 s renders of
  Cornell). It clears on reboot; it is not your change. `ohao doctor`'s `gpu.wedged`
  check says so. On Linux the NVIDIA open kernel module's GSP firmware can fail outright (`journalctl -k`:
  `Xid 120, GSP task exception`, then `NV_ERR_RESET_REQUIRED`): Vulkan then lists only the Intel GPU, every
  device creation asks the iGPU for ray tracing and the loader's error fails each GPU test. It needs a reboot (`nvidia-smi
  -r` refuses while the compositor holds the GPU). **Open (2026-11-08):** six GSP crashes, each during a GPU test, light
  ones included: five on driver 615.78.08 (2026-11-05 21:19 to 2026-11-06 18:35: Xid 120, 62, 119, and 109 context-switch
  timeouts before the fifth), one on 610.57.04 (2026-11-06 20:29, Xid 62), so no driver version is proven at fault. Two
  came during step 2b's runaway frames (dispatches of seconds; 2b is opt-in since, "Caustics"); the other four are not
  explained. Not heat (61 C at most), no PCIe errors; neither `nvidia-persistenced` nor
  `NVreg_DynamicPowerManagement=0x00` stopped one (both since undone). A failed GSP's state can survive a warm reboot
  (open-gpu-kernel-modules #1473): after a crash power the laptop off fully. **On Fedora 45 too** (RPM Fusion's
  615.78.08, 2026-11-08 19:04): Xid 119, "Timeout after 45s of waiting for RPC response from GPU0 GSP", during
  `check_env_orientation` (a sub-second render, bounded), after about 2.5 hours of GPU work; `[GPU requires reset]`.
  `nvidia-powerd` (Dynamic Boost, enabled by RPM Fusion) logged nothing before it and errors only after. So it follows
  the laptop across two distros and two driver versions; the production branch and the closed kernel module without
  GSP (`NVreg_EnableGpuFirmware=0`) are untried. **Never ship a dispatch whose work is unbounded**: a GPU dispatch
  is not preempted, and a few lanes walking for seconds hold their warps.
- **An MMU fault the engine caused (2026-11-11, driver 620.70):** `Xid 31 ... MMU Fault: ENGINE GRAPHICS`, one
  second after the engine's own hang report (`rt.gi` unfinished at the 2 s fence). Not the GSP: the engine's bug. A
  step 4 check replaced the sky map three times with the device wait before a replace removed; the texture table
  freed the old map's slot and handed it straight to the new map (slots are reused last in, first out), rewriting a
  descriptor that frames on the GPU were still reading. Validation cannot see this: the table's
  `UPDATE_UNUSED_WHILE_PENDING` makes it the application's promise. Fixed (an outside view's slot is retired after
  the frames in flight); the wait is back. A full power-off followed; the sequence ran clean three times after.
- **Linux, found by the port (2026-11-05):**
  - A pipeline must not outlive its layout on NVIDIA's Linux driver, `maintenance4` or not (Windows tolerated it):
    binding one whose layout was destroyed segfaulted in the driver. Keep every pipeline's layout until teardown
    (`GpuCulling`, `RTDirectTechnique` keep their extra ones in `m_extraLayouts`, `ManifoldCasters` its
    `m_groupLayout`). It is not always a crash: the manifold gather's groups dispatch also ran writing nothing, or hung
    the GPU, run to run.
  - Physics is deterministic run to run on one compiler, not across: MSVC's and GCC's builds land the blockout's
    crate one ulp apart, so `world.hash` and `tests/serve/expected` are recorded on the Linux build. The helmet's
    merged model bytes differ too.
  - `openpbr_tables --check` is byte for byte on the platform that wrote the tables; elsewhere every number within
    1e-5 (another math library's last digits: MSVC to GCC, at most 5.4e-6).
  - libstdc++ runs `std::execution::par` on oneTBB, linked where found (`TBB::tbb`).
  - Slang 2026 made `expand` a keyword; no entry point may be named so.
  - No custom command may output a path named like a target: the shader directory's `make_directory` edge was named
    `shaders` in Ninja, so `--target shaders` built the directory and no shader (made at configure time now).
  - slangc 2025.11 put an `ArrayStride` on arrays outside blocks (function-local constant arrays, the bindless table),
    which SPIRV-Tools 2026 and the validation layer 1.4.363 reject (VUID-StandaloneSpirv-None-10684) in nearly every
    shader: SPIR-V is made by 2026.20 (since 2026-11-06), whose 75 entry points all pass `spirv-val`.
  - **slangc 2026.20's C++ emitter re-reads a member after a store to it**: `Sampler.next()` (`const float u =
    point[dim & 3u]; ++dim; return u;`) became `dim = dim + 1; return point[dim & 3]` in C++, so every draw was the next
    coordinate; its SPIR-V of the same code loads first, as written. The twins (`tools/caustics`, `tools/material`)
    stay on 2025.11 (`SLANGC_CPP`) until a release's C++ passes `openpbr_tables --check` (the manifold twin and its `manifold_tests` were deleted
    2026-11-09 with the parked caustics work; git has them).
  - `ohao serve` on stdio interrupts its reader with SIGUSR1 (no `SA_RESTART`), the POSIX `CancelSynchronousIo`.
  - NRI (under NRD) is told of no surface extension (`nrd_denoise.cpp`): given `VK_KHR_surface` it resolves the surface
    functions of every platform it was built for (Xlib and Wayland), a window's instance on Hyprland has Wayland's alone,
    and NRI's default handler raised SIGTRAP: a path-traced capture in a window killed `ohao serve`. NRI's messages go
    through the logger, with no abort.
  - A Wayland client cannot learn it was minimized (GLFW_ICONIFIED is always false there) and Hyprland has no minimize:
    a window never reaches the 0x0 drawable the host treats as minimized.
  - **A window's size is pixels** (`runtime/window.cpp`, `fitPixels`):
    - GLFW sizes a Wayland window in the desktop's units and scales its framebuffer by the display's scale, so on
      KDE at 150% a 1920x1080 window was drawn with 2880x1620 pixels.
    - Measured 2026-11-10 on the 4060: the play scene's GPU frame is 8.7 to 9.3 ms at 1920x1080 and 19.8 to 20.0 ms
      at 2880x1620 (headless, `ohao run --size`); the window's frame went from 20.4 ms to 9.3 ms, and its look
      latency from 67 ms to 23 ms (`feel_bench`, immediate).
    - The window is made smaller in the desktop's units instead, and fitted again when Wayland tells it its
      display's scale. It logs `window: WxH pixels asked, scale S, framebuffer WxH`.
  - **A window's latency on this laptop** (the panel is on the Intel GPU, the frames come from the NVIDIA one;
    KWin 6.8 on Wayland; `feel_bench`, immediate, 2026-11-10, the display on):
    - Look latency is 20.8 ms at the median and 25.0 ms at p95, with 9.1 ms frames. The budget is 15 ms. (A dead
      player's movements, which nothing shows, were counted at about 55 ms until `PhysicsScene::ignoresLook`:
      the median read 23 to 24 ms and the p95 63 ms.)
    - From submit to present is 20 to 22 ms: the frame's 9 ms on the GPU, then 7 to 9 ms to the display through
      the compositor.
    - With the render scale (`feel_bench --render-scale`, 20 s each): 16.7 ms at 0.667 (4.7 ms frames) and
      12.7 ms at 0.5 (4.2 ms frames), against 20.8 ms at 1. From submit to present stays 12 to 16 ms: with the GPU
      at 3 to 5 ms a frame, most of it is the compositor's.
    - `VK_KHR_present_wait`'s time agrees with the compositor's own (Wayland's presentation-time, read in the same
      run) to 0.3 ms.
    - **Pacing** (2026-11-11). Until then a play window polled once the frame before the newest was presented.
      A present comes about three refreshes (240 Hz) after its submit here, so at scale 0.5 that gave two frames
      every three refreshes while the player lived (160 a second, 4.2 and 8.4 ms alternating) and 237 a second
      while dead: `frame_p99_over_median` 2.1 to 2.8. Now a frame starts on the display's refreshes and polls
      once the GPU has finished the frame before (`docs/changes/2026-11-11-frame-pacing.md`):

      | Scale | Latency, median / p95 (ms) | Frame, median / p99 (ms) | p99 / median |
      |---|---|---|---|
      | 1, before | 20.82 / 25.1 to 25.3 (3 runs) | 9.1 / 17.2 to 17.5 | 1.87 to 1.92 |
      | 1 | 19.6 to 20.3 / 22.1 to 23.1 (4 runs) | 9.1 / 11.5 to 12.4 | 1.27 to 1.36 |
      | 0.667, before | 16.6 to 16.7 / 17.5 to 17.8 (2 runs) | 4.9 / 13.4 | 2.7 to 2.8 |
      | 0.667 | 15.6 to 15.8 / 17.7 to 18.3 (3 runs) | 4.5 / 6.4 to 7.3 | 1.44 to 1.61 |
      | 0.5, before | 12.5 / 13.5 | 4.2 / 9.0 | 2.12 |
      | 0.5 | 13.0 / 13.6 to 14.1 (2 runs) | 4.17 / 4.3 | 1.02 to 1.04 |

    - **The compositor takes a frame at one point of each refresh.** At scale 0.5, one frame a refresh, the
      latency against the frame's start (after a present stamp, 10 s each): +1.0 ms 15.6, +2.0 ms 14.6, +3.0 ms
      13.6, -0.45 ms 13.0, 0 12.5, +0.25 ms 12.2, +0.5 ms 12.2 (p95 16.3: some frames a refresh late), +0.75 ms
      15.8. A timer at 240 a second (237 measured) drifts through all of these: 14.1 ms, p95 16.0. The start is
      0.45 ms before the stamp (`LiveHost::kRefreshLead`), the latest with a steady p95.
    - **Fullscreen is no faster** (2026-11-11, one run): at 2560x1600 and scale 0.5 a present comes 11.5 ms after
      the frame's 5.8 ms, against 9.3 ms after for the 1920x1080 window.
    - **Fifo** shows every frame for a refresh, so at one frame a refresh a frame waits for the one before: 17.1 ms
      at scale 0.5 (16.5 to 19.2 at four other starts), at 240 even frames a second. The old wait read 12.5 ms
      there, at 160 uneven frames a second (p99 / median 3.06). Mailbox, the default, reads as immediate (12.9 ms).
  - **A window measures nothing with the display asleep.** KWin discards every frame, and a frame's present is then
    confirmed only when the next frame replaces it, which reads as a present a frame late. `feel_bench` exits 2
    when no connected display is on (`/sys/class/drm/*/dpms`); `kscreen-doctor --dpms on` wakes it.
- Monograph: run its tests and `check_unit.py` from the repo root, never from `site/`
  (that directory shadows the stdlib `site` module and breaks `_ctypes`). The site build
  is the one exception: it imports `tools.monograph`, so it runs from `site/`.
  `build.py` has no `__main__`, so `python -m tools.monograph.build` does nothing.
  Removing a unit takes three edits: the `.md`, its `page()` entry in
  `site/tools/monograph/tree/<module>.py`, and its `author_rich.py` block, or the stale
  page keeps being served.

The monograph is frozen during the frontier renderer work (Frank, 2026-10-30): its citations
trail the code, so its checks are in no gate; run them by hand:

```bash
python site/tools/check_unit.py reflection/api
python site/tests/test_monograph_structure.py
cd site && python -c "from tools.monograph.build import build; build()"
```
