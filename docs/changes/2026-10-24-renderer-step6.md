# Sub-project 2b, step 6: Slang toolchain and the shared layout

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Slang", "Build order" step 6, as built)
**Status:** Built. `slangc` compiles in the `shaders` target. `ohao doctor` checks `.slang` freshness, follows imports and reports `tools.slangc`. `shaders/shared/layout.json` generates the Slang module, the GLSL header and the C++ header. The GPU calibration kernel is now Slang, and its GLSL source is deleted. `render_bench --calibrate` was rerun. `tools/check.py` quick + gpu + docs 35/35 (one new suite, `check_layout`). Render goldens 11/11. No rendering code changed, no budget changed and no tolerance was widened.

## 1. Summary

- **Slang beside GLSL.** `shaders/CMakeLists.txt` finds `slangc` (`$VULKAN_SDK/Bin`, the newest `C:/VulkanSDK/*/Bin`, then `PATH`; fatal if missing). It compiles every `.slang` under `shaders/` once per entry point, with `-target spirv -profile spirv_1_6`. The SPIR-V naming follows the flattening rule. GLSL is unchanged.
- **One layout, three languages.** Sizes and push constants shared by C++ and shaders live in `shaders/shared/layout.json`. `tools/gen_layout.py` writes `shaders/shared/layout.slang`, `shaders/shared/layout.glsl` and `ohao/gpu/shared_layout.hpp`. The C++ header `static_assert`s every std430 offset and size.
- **The doctor knows Slang.** `shaders.complete` expects one SPIR-V per entry point. `shaders.fresh` follows `import a.b;` to `a/b.slang` the way it follows `#include`. The new `tools.slangc` check fails at error level when `slangc` is missing, and at warn level when its version differs from `shaders/slangc.version`.
- **Proof:** the calibration kernel `shaders/bench/gpu_calibrate.slang` (entry `calibrate`) replaces `gpu_calibrate.comp`. It takes its group size, element count and push-constant struct from the shared layout. `render_bench` loads `bench_gpu_calibrate.slang.spv`.

## 2. What was built

- **CMake (`shaders/CMakeLists.txt`):**
  - Finds `slangc` and reads its version. Configure warns if the version differs from the pinned `shaders/slangc.version` (`2025.11`).
  - The configure step depends on the pin file and on every `.slang`, so a new entry point is picked up by the next build.
  - Entry points come from the source: a function under a `[shader("…")]` line, optionally followed by more `[…]` attribute lines.
  - Naming: one entry gives `<flattened>.spv` (`bench/gpu_calibrate.slang` → `bench_gpu_calibrate.slang.spv`); several give `<flattened>.<entry>.spv`. A file with no entry is a module and is only imported.
  - Compile line: `-entry <E> -fvk-use-entrypoint-name -matrix-layout-column-major -I shaders`. With `-fvk-use-entrypoint-name` the SPIR-V entry keeps its name, so `pName` is the function's name, not `main`.
  - Each Slang SPIR-V depends on every `.slang` file, so an edited module rebuilds its importers.
- **`shaders/shared/layout.json` + `tools/gen_layout.py`:**
  - Inputs: scalar constants and structs of `uint`, `int`, `float`, their 2- and 4-vectors, and `float4x4` (column-major in Slang, `mat4` in GLSL).
  - The generator refuses 3-vectors (std430 pads them differently in C++), non-scalar constants and duplicate names.
  - `--check` reports stale outputs without writing anything.
  - The generated files are checked in. The `check_layout` suite keeps them in sync with the JSON.
- **The calibration kernel:** `tools/bench/gpu_calibration.hpp` takes `kElements`/`kGroupSize` from `gpu::shared`, with a `static_assert` that the element count divides into groups. Its push range is `sizeof(gpu::shared::CalibratePush)` and `pName` is `"calibrate"`. The loop is the same xorshift/LCG work as the GLSL kernel it replaces.
- **`ohao doctor` (`tools/ohao/doctor.cpp`):**
  - `slangEntries` / `spirvNames` give the expected SPIR-V names per source.
  - `newestInput` resolves imports first in the importing file's directory, then in the shader root. An unresolved import is named in the detail.
  - `slangcCheck` locates `slangc` the same way CMake does and compares its version with the pin.
- **Tests:**
  - `tests/doctor/check_doctor.py` adds `s6_slang_sources_are_files_with_entry_points_named_per_entry`, `s6_an_imported_module_newer_than_the_spirv_makes_it_stale`, `s6_an_unresolved_import_is_named` and `s6_tools_slangc_is_found_at_the_pinned_version`, run on a fake Slang tree. `tools.slangc` joins the CPU check list.
  - New suite `tests/layout/check_layout.py` (label `quick`):
    - `s6_the_generated_files_are_current`
    - `s6_an_edit_to_layout_json_is_caught`
    - `s6_structs_follow_std430_and_the_header_asserts_it`: offsets 0/16/32/48/112, size 128, over a mixed struct
    - `s6_a_bad_layout_is_refused`
    - `s6_the_generated_glsl_and_slang_compile`: through `glslc` and `slangc`
- **Docs:** AGENTS.md (the Slang build bullet, `gen_layout.py --check`, the doctor's `slangc`, the new suite), ARCHITECTURE.md (the add-a-shader recipe gains Slang), the CLI guide's doctor check list. The monograph gains a "Slang beside GLSL" section in `shaders/layout`, the `slangc` requirement in `systems/build`, and the doctor's import and `slangc` checks in `systems/tests`.

## 3. Bugs found and fixed

- The monograph's `shaders/layout` unit said "40 sources, 40 SPIR-V modules" and "Ninety-two lines of CMake". Both were stale before this step: the glob now matches 41 GLSL sources and the file is 154 lines. The unit now gives the GLSL count and no line count.

## 4. Spec corrections

- The spec said the three layout files are generated from `layout.json`. As built, they are generated by `tools/gen_layout.py` but checked in, and `check_layout` fails while they are stale. They are not generated at build time. Reasons: C++ and both shader languages include them like any other source, a fresh clone builds without Python on the build path, and a diff to the layout is reviewable. The spec's step 6 "As built" paragraph says so.
- Kernel path: the spec's benchmark section now names `shaders/bench/gpu_calibrate.slang` (since step 6).

## 5. Results

- `tools/check.py` (quick, gpu, docs): 35/35. Among them `check_doctor` 11/11 (4 new), `check_layout` 5/5, `check_docs` 67/67, `engine_tests` 61/61, `deferred_determinism_gpu_test` 55/55. Doctor on the real tree: `shaders.complete` "42 sources" (41 GLSL + 1 Slang entry), `tools.slangc` "C:/VulkanSDK/1.4.321.1/Bin/slangc.exe (2025.11-12-gc5295eae2)".
- Render goldens: 11/11. Deferred scenes max_abs 0. Path-traced scenes max_abs 0, except `cli_cornell_box_rt_realtime` at max_abs 1 on 1 pixel (the documented BVH floor).
- Monograph: 98/98 units pass the citation check, the site rebuilds (98 pages), and the structure test passes 16/16.
- **`render_bench --calibrate`** (the Slang kernel): GPU calibration 1.549 and 1.551 ms. The GLSL kernel's reference was 1.7209 ms, so the Slang build is about 10% faster on the same work.
  - CPU calibration during the same runs was 45.033 and 44.047 ms. UnrealEditor used about 35% of a core at the time, and `wait_quiet.py` timed out at its 240 s deadline.
  - `build/render_bench_reference.json` therefore keeps the quiet-machine CPU reference (28.2536 ms) and takes the new GPU reference (1.551 ms). Writing the loaded CPU number would have made later noisy runs look clean.
- **`render_bench --check`:**
  - First run: noisy (outside load), so no verdict.
  - Rerun: not noisy, pass. CPU calibration 24.975 ms before and 23.180 ms after (reference 28.254, x0.88). GPU calibration 1.551 ms before and 1.650 ms after (reference 1.551, x1.06).
  - Frame medians: `cornell_720p` 4.801 ms, `helmet_game_720p` 5.303 ms, `helmet_game_1080p` 12.699 ms.
  - Every budget is still `pending` (gated from step 7 on).

## 6. Known gaps

- Only the calibration kernel is in Slang. Porting renderer shaders belongs to the steps that rewrite them (meshlets, clustered lighting).
- No GLSL shader includes `shaders/shared/layout.glsl` yet. It compiles in `check_layout`, and its first user comes with the GPU-driven steps.
- `tools.glslc` on this machine resolves to conda's `glslc` (it is first on `PATH`). It works, but it is not the SDK's. The doctor reports it and does not judge it.

## 7. How to run the new tests

```console
$ python tests/layout/check_layout.py
$ python tests/doctor/check_doctor.py
$ python tools/gen_layout.py --check
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json
```
