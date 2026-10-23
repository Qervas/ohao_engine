# Sub-project 4, step 3: golden diffs and ohao doctor

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Golden diffs an agent can read", "`ohao doctor`" and their as-built notes; "Build order" step 3; "Test plan" tests 4 and 5)
**Status:** Done.

## 1. Summary

A failing render golden now says where and how much the image changed: per-scene JSON with up to four regions, a heatmap and an expected | actual | heat crop of the worst region, which an agent can open. `ohao doctor` answers whether the machine can build, run and test OHAO, as JSON with a fix for each failing check, and `tools/check.py` runs it before any test.

## 2. What was built

- `tests/golden/render_golden.py`: `--json FILE` (one object per scene: name, mode, pass, max_abs, diff_frac, diff_px, rmse, tolerance, regions, more_regions, heatmap, crop, error), `--heatmaps failing|all` (default failing), `--diff-dir DIR` (default build/golden_diff). Regions: connected components (4-neighbour) of 16x16-pixel cells with any difference, the 4 largest by differing pixels. Heatmap: per-pixel max channel difference at the compare resolution (640 wide), black where equal, otherwise a ramp from red (at least 191) through yellow to white at the largest difference. Crop: expected | actual | heat of the worst region, 4x nearest; a region larger than 96x64 is cropped to a 96x64 window centred on its largest difference (a strip is at most 1168x256). `--selftest --json` records the run-to-run floor. The diff logic is importable functions (diff_map, regions, heatmap, crop_window, crop_strip, scene_record). The last stdout line is the shared summary {"suite":"render_golden","passed","failed","failures"}. CTest runs it with `--json build/golden_diff/golden.json`.
- `tests/validation/check_validation.py`: `--json FILE` (per scene: status pass|fail|skip, messages, distinct, first {vuid, text}, per-VUID counts, exit) and the shared summary line. CTest runs it with `--json build/check_validation.json`.
- `ohao doctor [--cpu-only] [--root DIR] [--shader-dir DIR] [--spirv-dir DIR]` (tools/ohao/doctor.cpp, doctor.hpp): prints `{"result": {ok, cpuOnly, checks: [{id, ok, level, detail, fix?}]}}`; exit 1 when an error-level check fails. Checks: vulkan.instance (error), vulkan.device (error; Vulkan 1.3 required; name, driver, API version), vulkan.rt (warn; acceleration structure, ray tracing pipeline, deferred host operations), vulkan.validation (warn), shaders.complete (error; every stage source has its SPIR-V in <exe dir>/bin/shaders), shaders.fresh (error; no SPIR-V older than its source or any file it #includes, transitively, resolved as glslc does: quoted includes in the including file's directory first, then shaders/ and shaders/includes/; unresolved includes named), assets.present (error; every asset the scenes in assets/scenes reference, through scene.validate with checkAssets), tools.glslc (error), tools.cmake (warn when PATH's cmake is conda's), python.deps (warn; numpy, Pillow, jsonschema), gpu.wedged (error; a 64x64 deferred frame within 2 s). `--cpu-only` skips the vulkan and gpu checks and creates no device.
- `tools/check.py` runs `ohao doctor` after the build (`--cpu-only` unless a gpu or golden label is chosen), puts its verdict in check.json's `doctor`, and stops before the tests when it fails; `--no-doctor` skips it; numpy and Pillow are now required for the quick label (test 4 needs them).
- Test 4, `tests/golden/test_golden_diff.py` (label quick, 11 checks): identical images, statistics of a known difference, regions largest first with exact boxes, the 4-region cap with ties broken by max_abs, regions clipped to the image, heatmap colours, the crop panels, the bounded crop of a large region, and an end-to-end run on a fake manifest whose command copies a prepared PNG (failing scene: regions, heatmap, crop, summary line, the .actual.png drift file; passing scene: no images unless --heatmaps all; the selftest floor).
- Test 5, `tests/doctor/check_doctor.py` (label quick, 7 checks) and `--gpu` (label gpu, 1 check): a fake shader tree with one missing SPIR-V named exactly; staleness through a quoted include, through two includes via the include root, and from a newer source, each naming exactly the stale SPIR-V; an unresolved include named with its file; --cpu-only lists exactly the six CPU checks with the documented shape (fix exactly when failing); the real tree passes; with --gpu the device checks pass on this machine.
- 32 registered tests (step 2: 29): test_golden_diff and check_doctor (quick), check_doctor_gpu (gpu).
- Docs: AGENTS.md (the doctor, the golden harness's --json and diff images, check_validation --json, the new tests), the CLI guide (`ohao doctor` in the command list and a section "The environment"), the verify skill draft in docs/agents/skills/ (failing goldens point at the heatmap and crop, wedged GPUs at ohao doctor, the new tests), the monograph unit systems/tests (the doctor and the diff report), the spec's as-built notes; site rebuilt.

## 3. Bugs found and fixed

1. `tools/check.py` read the CMake cache key `Python3_EXECUTABLE` for the interpreter CTest runs the harnesses with; FindPython keeps it as `_Python3_EXECUTABLE`, so the module check used whichever Python ran check.py. It now reads both.
2. The CLI's unknown-command message lists `doctor`; the one CLI golden that pins it (tests/cli/expected/usage_unknown_command.out) was updated after checking that the diff was only ", doctor".

## 4. Spec corrections

The crop is at the compare resolution, not full resolution (the goldens are stored downscaled to 640 wide), and bounded to a 96x64 window for large regions. The doctor's verdict is printed inside the CLI's {"result": ...} envelope like every other ohao command.

## 5. Results

- `ohao doctor` on this machine: ok; every check passes except tools.cmake (warn: conda's cmake is first on PATH); vulkan.device "NVIDIA GeForce RTX 5070 Laptop GPU, driver 621.1, Vulkan 1.4.357"; 40 shader sources, every SPIR-V present and fresh; 2 scenes with every asset present; gpu.wedged "renderer start 449 ms, 64x64 deferred frame 1.2 ms".
- test_golden_diff 11/11; check_doctor 7/7; check_doctor --gpu 1/1; check_cli --cpu-only 25/25.
- `python tools/check.py --all` (build 41.0 s, doctor ok): 31/32 passed. Counts from the suites' JSON lines: check_cli_cpu 25, check_scene_schema 16, check_tests 7, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_cli 29, check_validation 2, render_golden 10, check_docs 55, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 21, ecs_tests 26, reflect_tests 33, scene_tests 60, api_tests 17, systems_tests 6; ohao_game_presentation, monograph_structure and reflect_exhaustive_floats (382.0 s) passed. scene_bench failed (see Benchmark).
- Render goldens 10/10 (the deferred four at max_abs 0).
- A real run of the harness with --heatmaps all on cornell_box_rt_realtime (passing, max_abs 5 then 1 on about 1.4% of pixels across two runs): its noise formed one region 448x360 plus 20 small ones; the crop was bounded to 96x64 (1168x256 written).

### Benchmark

This step changes no code scene_bench runs. In the --all run it failed with bench10k warm medians save pretty 22.273 ms, validate 20.302 ms, load 32.192 ms, apply 0.370 ms. In the same process its cold bench10k block, which runs first, measured save pretty 8.900 ms, validate 8.204 ms, load 12.837 ms, apply 0.230 ms; the warm rounds that follow seconds later were about 2.5x slower. The machine runs at full speed for the first seconds of sustained single-core load and then slows, which is consistent with clock throttling; the same failure has persisted in every scene_bench run since about 17:40 (step 2's record).

## 6. Known gaps

- scene_bench cannot pass on the reference machine in its present state (see Benchmark); step 6 sets new budgets on the same machine.
- The verify skill change waits in docs/agents/skills/ until Frank installs it.
- tools.glslc reports the first glslc found (conda's here), not necessarily the one the build's shaders target uses.

## 7. Running the tests

```
python tests/golden/test_golden_diff.py
python tests/doctor/check_doctor.py
python tests/doctor/check_doctor.py --gpu
build/Release/ohao doctor
python tools/check.py --all
```
