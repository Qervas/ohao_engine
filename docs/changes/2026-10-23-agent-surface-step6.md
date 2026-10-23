# Sub-project 4, step 6: serve_bench and the CPU budgets

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Performance": "Measurement rules", "Budgets" and its as-built note; "Build order" step 6; "Test plan" test 18, CPU part)
**Status:** Done.

## 1. Summary

The protocol's CPU budgets are now a gate. serve_bench drives `ohao serve` as a child process on pipes, as an MCP client runs it, and checks the spec's CPU budget table. tools/check.py reruns a benchmark that reports itself noisy.

## 2. What was built

- `tools/bench/serve_bench.cpp` (new): builds bench10k (seed 2026; 10,002 entities) and saves it compact to `build/serve_bench/bench10k.scene.json`. It loads the scene into an in-process `api::Session` and into `ohao serve --out build/serve_bench/out --log-level warn`, started as a child process (CreateProcess, pipes, HIGH_PRIORITY_CLASS, its stderr to `build/serve_bench/server.stderr.txt`). Over stdio it sends initialize, notifications/initialized, tools/list, scene.load, then Spin on the first 100 entities.
- Ops: world_info_rpc and world_info_inproc (20 calls per block); world_query_one_rpc (one entity, field `/Transform/position`; 20); world_apply_100_rpc (100 field.set; 10); world_step_1_rpc (10); log_below_threshold (1,000,000 OHAO_LOG Debug calls below the threshold per block); log_ring_80 (20,000 80-character records into the ring per block). Each op runs its whole block in one closure call; ops are interleaved; 5 warm-up rounds, 50 measured; median and p95.
- transport_overhead = each round's RPC world.info median minus the in-process one. Server page faults per call (GetProcessMemoryInfo) are reported beside each RPC op.
- After timing, the replies are checked (entities 10002, count 1, applied 100, a frame), and the server must exit 0 when stdin closes: a bench of broken code fails.
- Recorded, not gated: process start to initialize reply; scene.load of bench10k over stdio; initialize reply and instructions sizes.
- Writes `build/serve_bench.json`. Exit 0 pass, 1 over budget or wrong reply, 2 noisy. `--calibrate` writes `build/serve_bench_reference.json`.
- `tests/bench/serve_budgets.json`: the CPU table: world_info_rpc_ms 0.15, transport_overhead_ms 0.10, world_query_one_rpc_ms 0.20, world_apply_100_rpc_ms 0.40, world_step_1_rpc_ms 0.50, log_below_threshold_ns 5, log_ring_80_ns 300, tools_list_bytes 24576. Noise: maxCalibrationSlowdown 1.25, maxExternalCorePct 50. The world.hash and world.diff budgets come with step 7; the GPU budgets with step 9.
- `tools/bench/bench_env.hpp`: `LoadMonitor::exclude(pid)`, so the server the bench started is not counted as outside load.
- `tools/bench/bench_world.hpp` (new): `buildBenchWorld` moved out of scene_bench.cpp unchanged (byte-identical body), shared by scene_bench and serve_bench.
- log_bench (step 4) is folded into serve_bench and deleted (`tools/bench/log_bench.cpp`, its target and registration). serve_bench is registered as a CTest test, label bench, RUN_SERIAL, timeout 600 s.
- `tools/check.py`: a test whose JSON summary line has `"noisy": true` is rerun alone (`ctest -R ^name$`), at most twice. Its check.json entry becomes the last attempt's, with `attempts: [{ok, noisy, seconds}]`. A test still noisy after the reruns is not passed and keeps `"noisy": true`. The ctest output flags are shared by the main run and the reruns, so a rerun keeps the end of the output where the summary line is.
- `tests/check`: fixture.py gained modes noisy_once and noisy_always (summary `"noisy": true`, exit 2; each run appends a line to a counter file). Two fixture tests with label bench. check_tests.py gained `t3_a_noisy_bench_is_rerun_at_most_twice`: noisy once then pass gives ok with attempts [(noisy), (ok)]; always noisy gives three runs in all, not ok, noisy, three attempts; check exits 1 with 1 of 2 passed.
- Docs: AGENTS.md (serve_bench command and the Benchmarks paragraph); docs/ARCHITECTURE.md (programs list); the verify skill draft and its README in docs/agents/skills (the skill under .claude/ waits for Frank's install); the spec (as-built note under Budgets, the serve_bench flags, step 4's note updated, step 6 done); monograph units core/log (serve_bench instead of log_bench, ring cost 195 ns) and systems/tests (the noisy rerun). Site rebuilt (92 leaf pages, 17 modules).

## 3. Bugs found and fixed

1. The first serve_bench draft ran each call through a std::function, which cost about 1.2 ns per call and measured log_below_threshold at 1.41 ns instead of the logger's 0.20 ns. Ops now run a whole block per closure call (0.1956 ns).
2. The stale `build/Release/log_bench.exe` stayed in the build tree after the target was removed, and check_tests' "every test executable registered" would flag it. Removed.

## 4. Spec corrections

None to the design. As-built notes added: serve_bench's flags are `[--check FILE] [--out FILE] [--reference FILE] [--rounds N] [--cpu-only] | --calibrate`; a noisy test still noisy after two reruns is reported as not passed with `noisy` kept.

## 5. Results

- check_tests 8/8 (was 7).
- serve_bench: nine runs by hand on 2026-10-23 after the block refactor, then one inside the gate. Run 1 noisy (other processes 72.6% of a core in total; UnrealEditor.exe 41.0%). Run 2 passed (34.4%; UnrealEditor 31.3%). Run 3 passed (44.2%; UnrealEditor 34.8%). Run 4 noisy (66.8%; UnrealEditor 15.9%, Zed 9.5%, CrossDeviceService 6.4%, PhoneExperienceHost 6.4%, chrome 6.4%). Runs 5-9 passed, five consecutive: the budgets are accepted. The gate's run (`check.py --all`) passed too, the sixth in a row.
- Server page faults per call 0.00 in every run. Calibration x1.00 of the reference (23.184 ms) in every run. tools/list 7,811 B in every run.
- Recorded: start to initialize 18.7-20.2 ms; scene.load bench10k over stdio 27.4-30.0 ms; initialize reply 725 B (instructions 551 B). In the earlier runs, initialize took 19.2 and 25.7 ms.
- `python tools/check.py --all` (build 18.1 s, doctor ok with the tools.cmake warning): 35/36 passed, 36 tests registered (step 5: 36; log_bench replaced by serve_bench). Counts: check_cli_cpu 25, check_scene_schema 16, check_tests 8, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 105, check_cli 29, check_validation 2, render_golden 10, check_docs 59, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 21, ecs_tests 26, reflect_tests 36, scene_tests 60, api_tests 22, systems_tests 6, rpc_tests 10, log_tests 10, serve_bench 8 (passed, not noisy). ohao_game_presentation, monograph_structure and reflect_exhaustive_floats (328.7 s) passed. scene_bench failed (see Benchmark).
- Render goldens 10/10 inside the gate.

### Benchmark

Five consecutive passing serve_bench runs (runs 5-9, after the block refactor):

| run | other processes (% of a core) | world_info_rpc ms | transport_overhead ms | world_query_one_rpc ms | world_apply_100_rpc ms | world_step_1_rpc ms | log_below_threshold ns | log_ring_80 ns |
|---|---|---|---|---|---|---|---|---|
| 5 | 41.1 | 0.0193 | 0.0189 | 0.0196 | 0.0908 | 0.0374 | 0.1953 | 194.565 |
| 6 | 24.8 | 0.0208 | 0.0203 | 0.0216 | 0.0955 | 0.0397 | 0.1954 | 195.790 |
| 7 | 31.4 | 0.0192 | 0.0187 | 0.0198 | 0.0940 | 0.0395 | 0.1956 | 193.850 |
| 8 | 44.3 | 0.0196 | 0.0192 | 0.0186 | 0.0916 | 0.0381 | 0.1953 | 195.280 |
| 9 | 41.5 | 0.0179 | 0.0176 | 0.0189 | 0.0896 | 0.0376 | 0.1954 | 194.765 |

Largest p95 over those five: world_info_rpc 0.0338 ms, world_apply_100_rpc 0.1287 ms, world_step_1_rpc 0.0590 ms, log_ring_80 209.165 ns.

scene_bench failed as in steps 2-5: its cold bench10k block measured save_pretty 9.121 ms, load 13.593 ms, validate 8.348 ms, apply_100_field_set 0.235 ms (step 5: 9.311, 13.519, 8.405, 0.227); the warm rounds that follow measured 22.069, 31.765, 20.193, 0.416 ms. This step moved its world builder into bench_world.hpp unchanged and changes none of its measured paths. No budget was changed.

Observation for Frank: serve_bench's calibration loop ran at x1.00 of the reference throughout this step, so the CPU was not slowed. But the calibration loop is pure integer arithmetic with no memory traffic, so it cannot see contention for caches or memory bandwidth, which scene_bench's large-scene operations depend on. Whether scene_bench's warm-round slowdown is outside load or something in scene_bench itself is not settled; scene_bench still has no load record of its own.

## 6. Known gaps

- world.hash and world.diff budgets: step 7 (the methods do not exist yet). GPU budgets: step 9.
- scene_bench's failure is not explained (see Benchmark); it has no calibration or load record.
- The calibration loop does not measure memory-system contention.
- The verify skill change waits in docs/agents/skills/ until Frank installs it.
- serve_bench's child-process client is Windows-only.

## 7. Running the tests

```
build/Release/serve_bench.exe --check tests/bench/serve_budgets.json
build/Release/serve_bench.exe --calibrate
python tests/check/check_tests.py
python tools/check.py --all
```
