# Sub-project 4, step 2: one check entry point

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Engine development by agents (G2)": "One entry point", "CTest registration", "Machine-readable output everywhere"; "Build order" step 2; "Test plan" test 3)
**Status:** Done.

## 1. Summary

`python tools/check.py` builds OHAO and runs every suite, harness and gate through CTest, and answers with one JSON verdict (`build/check/check.json`, also its last stdout line) plus JUnit XML. Before this step `ctest` found no tests at all (no `enable_testing()`), half the suites printed no machine-readable summary, a new source file needed a manual re-configure, and examples could run stale SPIR-V.

## 2. What was built

- `enable_testing()` and `cmake/ohao_tests.cmake`: `ohao_add_test(name LABEL ... [GPU] [SERIAL] [TIMEOUT] [LOCKS ...] [ENV ...] COMMAND ...)` and `ohao_add_python_test(...)`. Every test runs from the source root, has exactly one label, GPU tests share the resource lock `gpu`, benchmarks are RUN_SERIAL, and LOCKS names other shared resources. Harnesses are registered once at the end of the top-level CMakeLists (`ohao_register_harness_tests`).
- 29 tests: `quick` 14 (runtime, reflect, scene, api, systems, physics_backend, audio_system, force_generator, env_cdf, denoise_parse, sobol, check_cli_cpu, check_scene_schema, check_tests), `gpu` 10 (engine, ecs, renderer_test, instantiate_gpu_test, scene_view_gpu_test, restore_render_gpu_test, deferred_determinism_gpu_test, check_cli, check_validation, ohao_game_presentation with OHAO_VALIDATION=1), `docs` 2 (check_docs --cpu-only, monograph_structure), `golden` 1 (render_golden), `bench` 1 (scene_bench --check), `slow` 1 (reflect_tests --exhaustive-floats). `selftest` is used only by the fixtures of check_tests.
- `tools/check.py` (test-only Python, standard library): finds the real CMake (OHAO_CMAKE, else the scoop shim, else PATH; refuses conda's) and the ctest beside it; checks the Python modules the selected labels need with the interpreter CMake registered; configures when there is no CMakeCache.txt; builds the `shaders` target first, then everything, at most 6 jobs; runs ctest with `-L ^(labels)$`, `--output-junit`, `--output-on-failure`, `--no-tests=error`, 4 tests at a time, the full output of passing and failing tests kept for the report; prints only CTest's one-line-per-test progress, then the verdict. check.json: `{ok, labels, build, doctor, tests: [{name, labels, ok, seconds, passed, failed, failures, tail}], counts}`; passed/failed/failures come from each suite's own JSON line; a failing test carries the last 40 lines of its output. Exit 0 pass, 1 failure, 2 environment error. Options: `--labels`, `--all` (adds golden, bench, slow), `--jobs`, `--ctest-jobs`, `--config`, `--no-build`, `--build-dir`, `--out`. `doctor` is null until step 3.
- `tests/common/harness.hpp` (header-only): CHECK and RUN macros and `ohao::test::finish(...)`, which prints "Results: p/t passed" and the line `{"suite","passed","failed","failures"}` and returns the exit code. Adopted by ecs_tests, runtime_tests and systems_tests (their local copies of the macros removed) and, keeping their own TEST_BEGIN/EXPECT macros but now recording failures, by engine_tests, force_generator_tests, physics_backend_tests and audio_system_tests. `renderer_test` now checks what it renders (4 checks) and prints the line. `tests/common/gtest_main.cpp` (library `ohao_gtest_main`) replaces gtest_main for env_cdf_test, sobol_test and denoise_parse_test and prints the line; `gtest_discover_tests` is gone.
- `CONFIGURE_DEPENDS` on all 21 source and shader globs (ohao/core, scene, api, reflect, audio, runtime, ecs, render, physics, gpu/vulkan, shaders): a new file is picked up by the next build. `build/CMakeFiles/VerifyGlobs.cmake` holds 42 glob checks. No-op Release build: 7.5, 8.0, 8.5 s before; 11.0, 8.6, 7.4, 8.0 s after.
- `add_dependencies(... shaders)` for every target that loads SPIR-V: every example (in the shared `ohao_example` function), engine_tests and ecs_tests (renderer_test, the gpu tests and the ohao CLI already had it).
- Test 3, `tests/check/check_tests.py` (label quick, 7 checks): a Python-only fixture project `tests/check/fixture` (fixture_pass and fixture_fail labelled selftest, fixture_other_label labelled quick) configured into its own build tree `build/check_fixture`, so the nested check never shares `build/Testing` with the run that is testing it. Checks: a failing test fails the check (exit 1, counts 2/1/1); the failure carries its suite line and a 40-line tail ending with its marker; stdout's last line is check.json and the failing output stays out of the progress lines (it is in ctest.log); junit.xml agrees; labels select exactly their tests; an unknown label exits 2; every `*_test`, `*_tests` and `*_bench` executable in `build/Release` is a registered test and every test has exactly one known label.
- Deleted: `tests/CMakeLists.txt` (a dead gtest engine_tests target no CMakeLists included) and `tests/engine_tests.cpp`, which only it named.
- Docs: AGENTS.md leads its Gates with `python tools/check.py` (labels, registering a new suite), drops the re-configure and stale-shader gotchas for the new state, adds a gotcha about tests racing on fixed paths, lists check_tests; ARCHITECTURE.md's add-a-test-suite recipe uses the harness and ohao_add_test; drafts of the `build` and `verify` skills in `docs/agents/skills/` (this session cannot write `.claude/`); monograph units systems/tests (a new section "One command runs all of it"), systems/build, shaders/layout and architecture/invariants updated; site rebuilt; spec as-built notes.

## 3. Bugs found and fixed

The first full check run found these (22/26 passed).

1. Two tests raced on `build/cli_tmp`: the CLI check runs as check_cli_cpu and as check_cli (and inside check_docs); in parallel they deleted each other's files (check_cli_cpu: schema files "not written"; check_cli: "differs between two runs" and two more cases). The three now share the lock `cli_tmp`; check_tests and check_docs share `check_fixture` the same way.
2. api_tests wrote a fixed `%TEMP%/ohao_api_test.scene.json`, and two copies ran at once (CTest's and the one inside check_docs): check_docs failed on its api_tests line (`AGENTS.md:50`). The name now carries the process id.
3. renderer_test returned 0 even when no frame came back and wrote `smoke_test.png` into the working directory (the repo root); it now checks the frame and writes `build/renderer_test.png`.
4. Two monograph citations broke (the deleted `tests/CMakeLists.txt`; renderer_test's old "=== ALL PASS ===" line); monograph_structure caught them and the unit was rewritten.

## 4. Spec corrections

The shared harness macros are CHECK/RUN, not CHECK/SECTION (the names the suites already used). The spec's step 2 and test 3 carry as-built notes.

## 5. Results

- `python tools/check.py` (default labels quick, gpu, docs; build included, 51.2 s): 26/26 passed. Counts per test from their JSON lines: check_cli_cpu 25, check_scene_schema 16, check_tests 7, check_cli 29, check_docs 50, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 21, ecs_tests 26, reflect_tests 33, scene_tests 60, api_tests 17, systems_tests 6; check_validation, ohao_game_presentation and monograph_structure passed (they print no counts).
- `python tools/check.py --all --no-build`: 28/29 passed: render_golden passed (20.3 s), reflect_exhaustive_floats passed (364.3 s); scene_bench failed (see Benchmark).
- check_tests 7/7 run on its own.

### Benchmark

This step changes no code `scene_bench` runs. The codec, API and bench objects it executes were compiled at 12:39–14:43, before sub-project 4 began; the builds of this step only relinked the executable. The last passing runs of the same objects were step 1's runs 4 and 5 (bench10k save pretty 9.461 / 9.448 ms).

Five `scene_bench --check` runs in this step, in order, all FAIL with every operation about 2.3× slower at once (snapshot, a memcpy, included): bench10k save pretty 22.07 ms (inside `check.py --all`, run alone as RUN_SERIAL), 22.14 ms (`check.py --labels bench`), 22.25, 22.92 and 22.66 ms (run directly); validate 19.98–20.53 ms; apply 0.374–0.412 ms; bench100k ratios ×13.3–21.2. Running it directly gave the same numbers as under CTest, so the harness is not the cause. CPU load sampled 6–8% busy, on AC power; the per-core limit read back as the nominal 2.1/2.7 GHz (the API reports no boost clock), so the cause on the machine was not identified. Sub-project 4 step 6 adds the calibration loop and `noisy` verdict for this case.

## 6. Known gaps

- check_validation, the ohao_game gate and monograph_structure print no JSON summary line; check_validation gains `--json` in step 3.
- `doctor` is null in check.json until step 3.
- `tests/renderer/renderer_pipeline_tests.cpp` and `tests/shadow_system` are dead but cited by the monograph; left for a cleanup together with those units.
- The build and verify skill changes wait in `docs/agents/skills/` until Frank installs them.

## 7. Running the tests

```
python tools/check.py
python tools/check.py --all
python tests/check/check_tests.py
```
