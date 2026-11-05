# OHAO on Linux (Omarchy, Arch): the port

**Spec:** none (a platform move); AGENTS.md "Build" and "Gotchas" (Linux) describe the result.
**Status:** CPU gates green; GPU gates open: the GPU has been down since a driver fault (below), and the validation
gates wait on a ruling about the shader compiler (5).

## 1. Summary

OHAO builds and runs on Linux (Omarchy, Arch; GCC 16, Ninja Multi-Config, NVIDIA 615.78, RTX 4060 Laptop) as of
2026-11-05, which is the platform in daily use from now on; Windows/MSVC still builds. Commits 3deac2aa (build),
992287d6 (harnesses and expectations), 38629875 (the shaders target), 3fb0367d (the build skill).

## 2. What was built

- **Build:** the Ninja Multi-Config generator (it keeps the `build/Release/` layout every harness reads); slangc pinned at
  2025.11 from Slang's own release in `~/.local/opt/slang-2025.11` (Arch's `shader-slang` 2026.18 writes C++ the CPU
  twins cannot compile, and makes `expand` a keyword: `vfx/expand.slang`'s entry is `expandParticles`); oneTBB linked
  where found (libstdc++ runs `std::execution::par` on it); `--allow-multiple-definition` (GNU ld) as MSVC's
  `/FORCE:MULTIPLE`; assimp with its own zlib.
- **Device and pipelines:** `maintenance4` enabled, and every pipeline's layout kept until teardown (`GpuCulling`,
  `RTDirectTechnique`: `m_extraLayouts`): NVIDIA's Linux driver segfaulted binding a pipeline whose layout was destroyed,
  `maintenance4` or not.
- **Platform code:** `ohao serve` on stdio interrupts its reader with SIGUSR1 (POSIX's `CancelSynchronousIo`); the asset
  resolver refuses drive-letter paths; `ohao doctor` reads `OHAO_SLANGC`; the harness's `processId()` and `setEnvVar()`.
- **Harnesses:** binaries named per platform (`.exe` on Windows only); `check_docs.py` drops the suffix elsewhere.
- **Expectations recorded on Linux:** physics is deterministic on one compiler, not across: MSVC's and GCC's builds land
  the blockout's crate one ulp apart, so `world.hash` in `tests/cli/expected` and `tests/serve/expected` moved;
  `scene_tests` pins each platform's merged-helmet hash; `openpbr_tables --check` is byte for byte on the platform that
  wrote the tables and every number within 1e-5 elsewhere (MSVC to GCC: at most 5.4e-6).
- **The `shaders` target** (38629875): the shader directory was made by a custom command whose output was the
  directory, which Ninja names `shaders`, the target's own name, so `--target shaders` built nothing. Made at configure
  time now.

## 3. Bugs found and fixed

The pipeline-layout segfault, the shaders-target collision, and a negative constant index (`kDispersionLambda[-1]`,
inlined from `manifoldSurface(hit, -1, ...)` into dead code) that spirv-val refuses (54cff06c), each above.

## 4. Results

- `ctest -L quick`: 35 of 35 (material_tests 295 s, openpbr_tables_current 75 s, manifold_tests 35 s).
- `check_docs.py --cpu-only`: 96 of 96 CPU commands; the installed build skill's three Windows commands are skipped (a
  Linux one waits in `docs/agents/skills/build`).
- GPU: the render goldens ran once before the fault: the deferred scenes drift by rounding, `materials_step2`, `glass`
  and `subsurface` show local structure in their RT passes; not yet re-recorded (they need looking at on a live GPU).

## 5. Known gaps

- **The GPU is down:** during `light_cluster_gpu_test` the NVIDIA open module's GSP firmware faulted (`Xid 120`, then
  `NV_ERR_RESET_REQUIRED`); Vulkan lists only the Intel iGPU until `sudo nvidia-smi -r` or a reboot. Every GPU gate, the
  goldens' re-recording and the realtime path tracer's slowdown bisection wait on it.
- **Validation on Linux:** Arch's validation layer (1.4.363) reports VUID-StandaloneSpirv-None-10684 at
  `vkCreateShaderModule` for slangc 2025.11's SPIR-V in nearly every shader (`ArrayStride` on arrays outside blocks:
  function-local constant arrays, the bindless texture table), so every validation-on gate fails on Linux for that alone
  (checked on the iGPU with the layer). slangc 2026.18 emits valid SPIR-V for the same sources. Moving the pin moves
  every shader's code: a ruling, then the goldens on the GPU.

## 6. How to run

```console
$ ctest --test-dir build -C Release -L quick   # slow
$ python tests/docs/check_docs.py --cpu-only   # slow
```
