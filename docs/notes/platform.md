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
