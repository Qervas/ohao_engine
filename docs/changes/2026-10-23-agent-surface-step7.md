# Sub-project 4, step 7: time model, world.hash and world.diff

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("The host interface" correction; "System phases", "Time modes", "Determinism" and "Budgets" as-built notes; "Build order" step 7; "Test plan" tests 11 and 12)
**Status:** Done.

## 1. Summary

World systems now have two phases, so a frame's simulation no longer depends on how often it is rendered. The session has a time model (lockstep or realtime, paused, fixed step, frame) with time.get and time.set. world.step gains watch. world.hash and world.diff exist and meet their budgets.

## 2. What was built

- System phases (`ohao/ecs/world.{hpp,cpp}`): `addSimulate(name, fn(World&, float dt))` (behaviour, once per fixed step) and `addSync`, `addSyncBefore`, `addSyncAfter` with `fn(World&)` (derived state, once per batch of steps and before each render, no dt). `simulate(dt)`, `sync()`, `update(dt)` = simulate then sync; `simulateNames()`, `syncNames()`. spin is simulate; instantiate, transform and render_extract are sync. The old `addSystem`, `addSystemBefore`, `addSystemAfter` and `systemNames` are gone. The headless examples and the ohao CLI's render host call `world.sync()` before each frame instead of `update(0)`, so a capture never steps the world.
- The time model, `ohao::Timeline` (`ohao/core/timeline.{hpp,cpp}`), with `FixedStepClock` moved there from `ohao/runtime`: mode (lockstep or realtime), paused, fixed step in integer nanoseconds, frame number, optional fps. `advance(wall)` gives fixed steps only when realtime and not paused. Pausing, resuming and mode switches drop the clock's leftover time, so no step is run or lost. CPU-only, reads no clock. `ohao/runtime/clock.hpp` keeps FrameStats.
- `Application` steps its World with a Timeline (realtime by default, `AppConfig::timeMode`). Per frame, for each due step: the fixedUpdate hook and `World::simulate(fixedDt)`; then the update hook and `World::sync()`. `Application::timeline()`. Spin now runs per fixed step in ohao_game (it ran with the frame's dt before).
- Session time: the session owns a lockstep Timeline until a host shares its own (`Session::setTimeline`, `Session::timeline()`), so a window host's realtime steps and world.step count on one frame axis. New methods `time.get` returning {mode, paused, frame, fixedDt, fps?} and `time.set` {mode?, paused?, fixedDt?}. Rules: realtime only for a window host (E_STATE otherwise); world.step is E_STATE while realtime runs; fixedDt in (0, 1]. `world.info` gains `time`.
- `world.step`: dt defaults to the timeline's fixed step (bit-identical to the old 1/60 default). `watch: {ids?, names?, fields, limit = 50}` returns world.query's selection and projection after the last step, checked before any step runs. Sync runs once after the steps, also when cancelled. world.query was split into a shared plan/write pair (`planQuery`, `writeQuery`) used by query, watch and diff.
- `world.hash` returns {hash, frame} (`ohao/ecs/world_hash.{hpp,cpp}`): 64 bits over what a snapshot holds (entity table: ids, creation sequence, parent rows, child rows; each saved component's rows and values in row order; preserved unknown components; id generator; next creation sequence). It is read live from the storages without copying values (`captureEntityTable`, a typed `hashColumn` per component). Values are hashed field by field through a new `FieldOps::hash` (`reflect/codec.hpp` `hashValue`): floats by bits, strings by length and bytes, entity references by id. A type whose registered fields fill its whole size is hashed as its bytes (`TypeInfo::plainBytes`, decided once). The hash function (`reflect::Hasher`, `reflect/hash.hpp`) is FNV-1a 64 over 8-byte words with a xor-shift fold per word and a splitmix64 finish.
- `world.diff` {snapshot, fields?, limit = 50} returns {changed: [{id, name?, fields: {path: [old, new]}}], created, destroyed, count, more?} (`ohao/ecs/world_diff.{hpp,cpp}`, a typed read-only `diffColumn` per component, memcmp first for trivially copyable ones). Paths are "/Type/field" with old and new values in file form; "/Type" for a component added or removed; "/Parent/entity" or "/Parent" for the hierarchy. Created and destroyed ids are listed.
- serve_bench: ops world_hash_rpc and world_diff_1pct_rpc. The world is put back to a fixed 1%-changed state before each diff block (untimed, by an Op `prepare`). Budgets world_hash_rpc_ms 1.0 and world_diff_1pct_rpc_ms 2.0 in `tests/bench/serve_budgets.json`. Correctness checks run after timing (two hashes equal; the diff counts 100).
- Tests: runtime_tests sp4_t11 x3 (timeline); api_tests sp4_t11 x5 and sp4_t12 x4; reflect_tests sp4_t12_value_hash_ignores_padding_and_counts_bits; check_serve.py examples for the four new tools. The native protocol session covers world.hash (equal before an edit and after the restore), world.diff, time.get and time.set with the E_STATE refusal, and world.step with watch.
- Docs: AGENTS.md (suite table; a gotcha on system phases and sync before render); docs/ARCHITECTURE.md (the frame diagram by phase, module table, the add-a-system recipe); docs/agents/ohao-cli.md (method table, habits, tool count 18); the spec's as-built notes and the step 7 done note; monograph units scene/world, architecture/request-path, systems/examples, reflection/api. Site rebuilt (92 leaf pages).

## 3. Bugs found and fixed

1. The first world.hash (a snapshot, then a hash pass over it) measured 0.86 ms of the 1.0 ms budget: snapshot copy 0.27-0.37 ms warm, hash pass 0.52 ms. Reading the storages live in row order without copying values, and hashing padding-free types as bytes, brought it to 0.52 ms over stdio. In process, the entity table takes 0.09 ms; per column, field-wise before the change: MeshSource 0.17, Transform 0.12, Name 0.11, MaterialOverride 0.09 ms.
2. Two new tests first compared hashes of worlds built with random id seeds, so the ids differed. They now seed the id generator. This is a test fix, recorded because the defect would have hidden a real difference.
3. diffColumn repeated a line the monograph's snapshots unit cites, making that citation ambiguous. The diff's comparison was rewritten with swapped arguments.

## 4. Spec corrections

- "The host interface": time is not a pair of Host virtuals but a Timeline shared by session and host (`Session::setTimeline`). The host kind decides whether realtime is allowed. Capture and beforeSteps stay for steps 8 and 9.
- world.hash: same content as specified, read live in snapshot order without copying values. FNV-1a over 8-byte words with a fold, instead of byte-wise FNV-1a (speed).
- The phase is in the method name and in the system's type (addSimulate, addSync), not an addSystem parameter. Sync systems take no dt.

## 5. Results

- New and changed suites: api_tests 31/31 (was 22), runtime_tests 24/24 (was 21), reflect_tests 37/37 (was 36), check_serve 125/125 (was 105), rpc_tests 10/10 (tools/list 10,357 bytes for 18 tools, budget 24 KB), systems_tests 6/6, ecs_tests 26/26, scene_tests 60/60, check_cli 29/29 (CPU 25/25).
- Goldens updated after reading the diffs: tests/cli/expected (schema.out, run_script.out, run_script_mistakes.out) and tests/serve/expected (errors, mcp, native). Changes: schemaHash 8a3083d7fd9f023a -> ef86e2dd5f1d6054; world.info gains time; the method list gains time.get and time.set (and 6 more); WorldStepParams lists watch; the native session has the new replies. No world.step result or frame number changed.
- `python tools/check.py --all` (build 84.0 s, doctor ok with the tools.cmake warning): 34/36 passed, 36 tests registered. check_docs failed on one line: the CLI guide's tools/list example still said 14 tools; fixed to 18, then `python tests/docs/check_docs.py` 82/82 (19 skipped). scene_bench failed (see Benchmark). Counts: check_cli_cpu 25, check_scene_schema 16, check_tests 8, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 125, check_cli 29, check_validation 2, render_golden 10, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 24, ecs_tests 26, reflect_tests 37, scene_tests 60, api_tests 31, systems_tests 6, rpc_tests 10, log_tests 10, serve_bench 10. ohao_game_presentation, monograph_structure and reflect_exhaustive_floats (341.0 s) passed.
- Render goldens 10/10 inside the gate (the deferred four exact). The switch from update(0) to sync() in the renders changed no pixel.

### Benchmark

serve_bench, 15 runs by hand on 2026-10-23. Noisy: run 1 (other processes 58.1% of a core), run 6 (calibration 1.60x the reference before the rounds), run 10 (57.8%). Passed: runs 2-5, 7-9, 11-15. Runs 11-15 are five consecutive passes: the budgets are accepted. The gate's run passed too.

| run | other processes (% of a core) | world_hash_rpc ms | world_diff_1pct_rpc ms | world_info_rpc ms | world_apply_100_rpc ms | world_step_1_rpc ms | log_ring_80 ns |
|---|---|---|---|---|---|---|---|
| 11 | 27.7 | 0.5240 | 0.4420 | 0.0204 | 0.0967 | 0.0393 | 197.070 |
| 12 | 43.4 | 0.5243 | 0.4461 | 0.0209 | 0.0982 | 0.0409 | 198.165 |
| 13 | 31.9 | 0.5251 | 0.4436 | 0.0208 | 0.0952 | 0.0408 | 197.670 |
| 14 | 41.8 | 0.5193 | 0.4541 | 0.0208 | 0.0961 | 0.0396 | 199.425 |
| 15 | 39.3 | 0.5241 | 0.4373 | 0.0203 | 0.0956 | 0.0387 | 199.180 |

Largest p95 over those five: world_hash_rpc 0.8014 ms, world_diff_1pct_rpc 0.6831 ms. log_below_threshold 0.1957-0.1959 ns. Calibration x1.00-1.01. Budgets: hash 1.0 ms, diff 2.0 ms.

scene_bench failed as in steps 2-6: cold bench10k block save_pretty 9.132 ms, load 13.410 ms, validate 8.277 ms, snapshot 0.515 ms, restore_unchanged 0.615 ms (step 6: 9.121, 13.593, 8.348, -, -; the cold snapshot in step 6 was 0.535). Warm rounds 22.473, 31.912, 20.462, 1.011, 1.342 ms. This step refactored snapshot capture (the entity table split into captureEntityTable) without changing what it does; the cold snapshot did not move. No budget was changed. The step 6 question about scene_bench's warm rounds is still open.

## 6. Known gaps

- The window host that runs realtime and shares its Timeline with a session is step 9. Until then only tests drive a Session in realtime (by hand). ohao_game has no session.
- Input stamping per step (beforeSteps) and the input.* methods: step 8.
- world.diff writes old values' entity references through the live world (no saved component holds one besides Parent, which is compared by id).
- world.hash is 0.52 ms of 1.0 ms. The remaining cost is the hash's serial multiply chain and per-value calls; a multi-lane hasher would cut it further.
- scene_bench's failure is still not explained (step 6).

## 7. Running the tests

```
build/Release/api_tests.exe
build/Release/runtime_tests.exe
build/Release/reflect_tests.exe
python tests/serve/check_serve.py
build/Release/serve_bench.exe --check tests/bench/serve_budgets.json
python tools/check.py --all
```
