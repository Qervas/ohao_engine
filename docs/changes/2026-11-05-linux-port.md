# OHAO on Linux (Omarchy, Arch): the port

**Spec:** none (a platform move); AGENTS.md "Build" and "Gotchas" (Linux) describe the result.
**Status:** CPU gates green; the validation gate green on Linux since 2026-11-06 (SPIR-V from slangc 2026.20, the CPU
twins from 2025.11: 2); the goldens wait on re-recording on Linux (4).

## 1. Summary

OHAO builds and runs on Linux (Omarchy, Arch; GCC 16, Ninja Multi-Config, NVIDIA 615.78, RTX 4060 Laptop) as of
2026-11-05, which is the platform in daily use from now on; Windows/MSVC still builds. Commits 3deac2aa (build),
992287d6 (harnesses and expectations), 38629875 (the shaders target), 3fb0367d (the build skill).

## 2. What was built

- **Build:** the Ninja Multi-Config generator (it keeps the `build/Release/` layout every harness reads); two slangc from
  Slang's own release in `~/.local/opt/slang-<version>`, found there by configure: 2026.20 for SPIR-V
  (`shaders/slangc.version`, `SLANGC`) and 2025.11 for the CPU twins' C++ (`shaders/slangc_cpp.version`, `SLANGC_CPP`).
  Arch's `shader-slang` 2026.18 writes C++ the twins cannot compile; Slang 2026 makes `expand` a keyword
  (`vfx/expand.slang`'s entry is `expandParticles`); oneTBB linked
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
inlined from `manifoldSurface(hit, -1, ...)` into dead code) that spirv-val refuses (54cff06c), each above. Found once
the GPU was back (2026-11-06):

- **The manifold gather's groups pipeline outlived its layout** (`ManifoldCasters::create` destroyed it at once): the
  same driver behaviour as above, here intermittent. Of 12 renders of `m4_slab_seen`, 10 were right, one did no manifold
  work (the groups kernel left every draw's alpha bounds at 0), one segfaulted in the driver inside the groups dispatch
  (exit 139, `coredumpctl`), and a run before hung the GPU (a kernel reading garbage counts). The layout is kept until
  teardown; 40 of 40 renders right after. Caustics step 2a uses the same gather, so it was as broken on Linux.
- **`manifoldConnect` read bindings 4, 5, 8 and 36 that the layout gave the hit stages only**
  (VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988, the caustics scene): step 2b's walk reads a solid's triangles
  from its raygen; they carry the raygen stage now.
- **slangc 2026.20's C++ emitter re-reads a member after a store to it:** `Sampler.next()` (`const float u =
  point[dim & 3u]; ++dim; return u;`) became `dim = dim + 1; return point[dim & 3]`, so every draw was the next
  coordinate (`sobol_test`, the only one of 35 CPU suites to fail on 2026.20's twins). Its SPIR-V of the same code loads
  before the store, as written (checked in the disassembly). The twins stay on 2025.11.
- 2026.20 warns (E41035) about 41 possibly uninitialised reads in the shaders (`path_guiding` `guideRead`, `restir_di`'s
  neighbour lists, `manifold_gather`'s triangle, `gi_trace`'s `reflected`): each is read only on the paths that set it.
  One in `tools/caustics/manifold_kernel.slang` (op 15's frame outputs when the map is undefined) is now initialised.

## 4. Results

- `ctest -L quick`: 35 of 35 (material_tests 295 s, openpbr_tables_current 75 s, manifold_tests 35 s); the same after the
  compiler split (material_tests 275 s).
- Validation (`check_validation.py`) on the RTX 4060: 20 of 20 scenes clean with 2026.20's SPIR-V (with 2025.11's,
  VUID-10684 in nearly every shader). `spirv-val` 2026.4: 75 of 75 entry points. Kernel sizes: raygen 29,431
  instructions, raygenCaustics 30,562, manifoldConnect 36,306 (2025.11: 27,901 and 34,168).
- `check_caustics.py` (all but the showcase): 14 of 14.
- `check_docs.py --cpu-only`: 96 of 96 CPU commands; the installed build skill's three Windows commands are skipped (a
  Linux one waits in `docs/agents/skills/build`).
- GPU: the render goldens ran once before the fault: the deferred scenes drift by rounding, `materials_step2`, `glass`
  and `subsurface` show local structure in their RT passes; not yet re-recorded (they need looking at on a live GPU).

## 5. Known gaps

- **The GPU went down once:** during `light_cluster_gpu_test` (2026-11-05) the NVIDIA open module's GSP firmware faulted
  (`Xid 120`, then `NV_ERR_RESET_REQUIRED`); Vulkan listed only the Intel iGPU until a reboot (`nvidia-smi -r` refuses
  while Hyprland holds the device).
- **The goldens** are not yet re-recorded on Linux: the compiler move changes every shader's SPIR-V, so they are looked
  at and recorded after it.
- **Two compilers** (Frank's ruling of 2026-11-06: one 2026 release for both, else the split): the twins test the
  modules as 2025.11 compiles them while the GPU runs 2026.20's code. A later release whose C++ passes every twin's
  suite ends the split.

## 6. How to run

```console
$ ctest --test-dir build -C Release -L quick   # slow
$ python tests/docs/check_docs.py --cpu-only   # slow
```
