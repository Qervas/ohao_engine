# Sub-project 4, step 8: input as data, and scene_bench's slow rounds

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Input" and "Input, as built"; the step 8 correction under "The host interface"; "Build order" step 8; "Test plan" tests 13 and 14)
**Status:** Done. scene_bench and serve_bench were noisy in every run of this step (outside load); no budget changed.

## 1. Summary

Input is data now. Bindings and key names live in the world, so a scene file can hold them. The session can inject input events, record a session's input, and replay it with a checked world hash at every checkpoint. A virtual device gives agents the same action and axis input a person's keyboard gives. A fly controller is the first system that reads input. scene_bench gained probes that show when the machine, not OHAO, is slow. Its budgets are unchanged, and no run of this step was quiet.

## 2. What was built

- InputMap moved from `ohao/runtime` to `ohao/core/input.{hpp,cpp}` (namespace `ohao`), so ECS systems can read it. The World owns it (`World::input()`) and steps it once per simulation step with `World::stepInput(events)`. `Application::input()` returns the World's. The application collects events polled during a frame and hands them to the first step that runs, so a device event is stamped with the step that consumes it. `InputMap::beginFrame` was renamed `beginStep`.
- Key names: a reflected enum `Key` (catalogue name "Key"). Values are GLFW codes; mouse buttons are 1000 + button. Digits are `Digit0`-`Digit9` (enum names must be identifiers). `None` (0) means unbound. Functions `keyFromName`, `keyName` and `keyBinding`.
- Virtual device: `InputEvent` gains `ActionDown`, `ActionUp`, `AxisSet`, `AxisRelease` and `MouseDelta`. An action is held while a bound key or the virtual device holds it. An injected axis value overrides its keys until it is released. `FocusLost` releases keys, mouse buttons and the virtual device.
- Components: `InputActions` (singleton) holds `actions: [ActionBinding{action, key}]` and `axes: [AxisBinding{axis, positive, negative}]`. Its keys are the enum `Key`, so a typo is `E_ENUM` with a suggestion. The World rebuilds its bindings from `InputActions` when the component is constructed, updated or removed (EnTT signals set a dirty flag; the next `stepInput` rebuilds).
- `FlyController` {moveSpeed 3 m/s, fastMultiplier 4, lookDegPerUnit 0.1 deg, and the axis and action names move_forward, move_right, move_up, look, fast}, with the `fly_controller` simulate system (`ohao/ecs/fly_controller_system.{hpp,cpp}`). While `look` is held, rotation = normalize(yaw(-dx * rate) * rotation * pitch(-dy * rate)). The entity moves by (front * forward + right * right + worldUp * up) * moveSpeed * (fast ? fastMultiplier : 1) * dt. The Transform is patched only when it moved or turned.
- `ohao_game`'s quit and fly camera moved into its fixedUpdate hook, so they run per step.
- Methods (`ohao/api/input.cpp`); the method count is 22 (was 18):
  - `input.inject` {events: [{step, action | axis | key | mouse | focusLost, down?, value?}]} returns {queued, firstStep, lastStep}.
  - `input.state` returns {actions: {name: {held, pressed, released}}, axes: {name: value}}.
  - `input.record` {path} starts a recording; {stop: true} ends it and returns {path, steps, events}.
  - `input.replay` {path, verify = true} returns {steps, events, checkpoints, frame}.
- One step function, `stepOnce` / `Session::step(deviceEvents, dt)`: device events, then the injected events due at this step, then the recording, then `World::simulate`, then frame + 1, then a checkpoint every 60 steps. `world.step`, `input.replay` and (step 9) the window host all go through it. The injection queue is dropped by `world.restore` and by `scene.load` with `replace`.
- Recording format (JSON Lines):
  - A header {format "ohao.input", version 1, schemaHash, the scene and seed of the last `scene.load` if there was one, startFrame, startHash, fixedDt}.
  - Event lines with an absolute step. Keys are written by name, or by keyCode or button when the code has no name. Cursor is an absolute pointer move, mouse is a delta. Also focusLost, action and axis lines.
  - A {"step", "dt"} line before any step whose length differs from fixedDt.
  - A {"step", "hash"} line every 60 steps, and {"end", "hash"} on stop.
  - Starting a recording releases all input (as a focus loss).
- Replay: `E_STATE` while realtime runs or while injected events are queued. With `verify`, the world hash must equal startHash. Each checkpoint is compared before its step runs. `E_REPLAY`'s expected field names the first diverging step. Replay also starts by releasing all input.
- scene_bench (at the reviewer's request): a per-round CPU probe (2M xorshift iterations, about 2.3 ms) and a memory probe (one load per 64-byte line over a 64 MB buffer), in `tools/bench/bench_env.hpp`. Calibration runs before and after the gated rounds. serve_bench's outside-load record (other processes' CPU) is included. Per-op medians are printed for clean rounds and for contended rounds. `--machine-test [S]` runs the CPU probe alone. Noise thresholds are in the `noise` block of `tests/bench/scene_budgets.json` (maxCalibrationSlowdown 1.25, maxExternalCorePct 50). A round is contended when a probe is over 1.5x its fastest round. The run is noisy (exit 2) when calibration is over 1.25x the reference, when other processes use more than 50% of a core, or when over a quarter of the rounds are contended. The JSON gains `machine` and `noisy`. `tools/check.py` reruns a noisy test at most twice. Budgets are unchanged.
- Tests:
  - runtime_tests `sp4_t13` x3: key names round trip; the virtual device acts like a device; a rebind counts at the next step without an event.
  - api_tests `sp4_t13` x8: injection is stamped by step; an axis override holds and releases to the keys; InputActions rebind on change; focus lost releases everything; unknown key names are `E_ENUM` with a suggestion; record then replay matches every checkpoint (150 steps, 8 events, 3 checkpoints); a tampered recording fails at the right step (step 120); a realtime recording with dropped time replays exactly.
  - systems_tests `sp4_t14` x3: sixty steps forward move moveSpeed * dt * 60 along the front; looking turns by the configured rate; fast multiplies, and idle writes nothing.
  - Scene corpus case `tests/scenes/invalid/20_input`: `E_ENUM` with suggestion Space, `E_TYPE`, `E_UNKNOWN_FIELD` with suggestion negative, `E_SINGLETON`, `E_RANGE` on moveSpeed, and `E_UNKNOWN_FIELD` with suggestion moveSpeed.
  - scene_tests random worlds include InputActions and FlyControllers.
  - check_serve.py has examples for the four new tools. The native protocol session gains inject, a 2-step world.step, input.state and a bad key name.
- Docs: AGENTS.md (suite table; the scene_bench noise record and the machine finding; a gotcha on per-step input). docs/ARCHITECTURE.md (module rows, the frame diagram, the systems_tests summary line, now 9). docs/agents/ohao-cli.md (method rows; the InputActions and FlyController component rows; the tools/list example with 22 tools). The spec's as-built notes. Monograph units reflection/api (new "Input as data" section) and scene/world. Site rebuilt (92 leaf pages).

## 3. Bugs found and fixed

1. InputMap re-evaluated actions only when an event arrived. A binding change with no new event (for example, InputActions removed while a key was held) left the action held until some key changed. `beginStep` now re-evaluates every action from the current device and virtual state before the step's events. Covered by runtime_tests `sp4_t13_a_rebind_counts_at_the_next_step_without_an_event`.
2. The first gate run failed check_docs: docs/ARCHITECTURE.md:234 still expected systems_tests `"passed": 6`; it is 9 now. Fixed. check_docs then passed 82/82 (19 skipped).

## 4. Spec corrections

- "The host interface": there is no `Host::beforeSteps`. Input stamping needs no host virtual; `Session::step(deviceEvents, dt)` is the single per-step entry point.
- `ActionBinding.key` and `AxisBinding.positive` / `negative` are the enum `Key`, not `std::string`. The codec then gives `E_ENUM` with suggestions for free. Digits are `Digit0`-`Digit9`; `None` means unbound.
- The recording format gains {"step", "dt"} lines for uneven steps. Checkpoints hash the world at that step before it runs. Recording and replay both start by releasing all input. Replay is refused while realtime runs or while injections are pending.
- world.hash covers every saved type's column. Adding InputActions and FlyController changed the hash of unchanged scenes (the CLI rooms golden went from 0dc30d7b47b1f234 to 87ba2c301e031a40). Hashes compare worlds of one engine build.
- `ohao_game` played from a scene file (`helmet_game.scene.json`) stays with step 9 (LiveHost); in step 8 its fly camera is still host code, run per step.

## 5. Results

- Suites: api_tests 39/39 (was 31), runtime_tests 27/27 (was 24), systems_tests 9/9 (was 6), reflect_tests 37/37, scene_tests 60/60, rpc_tests 10/10, check_serve 143/143 (was 125), check_cli 29/29 (CPU 25/25), check_scene_schema 16/16, ecs_tests 26/26, engine_tests 58/58, log_tests 10/10.
- Goldens updated after reading the diffs: `tests/scene/golden` (engine_catalogue.json and engine_scene.schema.json, for the Key enum and the four new types); `tests/cli/expected` (schema.out, run_script.out, run_script_mistakes.out); `tests/serve/expected` (errors, mcp, native). schemaHash is now d3fb06fdb4aa5d0a. tools/list is 13,763 bytes for 22 tools (budget 24,576 bytes).
- `python tools/check.py --all`: build 16.0 s; doctor ok (tools.cmake warning: conda's cmake first on PATH); 36 tests, 33 passed, 3 failed. The three failures: check_docs (the stale line in Bug 2, fixed afterwards; then 82/82 by hand), scene_bench and serve_bench (noisy in all three attempts each).
- Per-test counts from that run: check_cli_cpu 25, check_scene_schema 16, check_tests 8, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 143, check_cli 29, check_validation 2, render_golden 10, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 27, ecs_tests 26, reflect_tests 37, scene_tests 60, api_tests 39, systems_tests 9, rpc_tests 10, log_tests 10. ohao_game_presentation, monograph_structure and reflect_exhaustive_floats (570.3 s) passed.
- Render goldens 10/10 inside the gate (the four deferred scenes exact). Validation clean.

### Benchmark

The question came from the reviewer after step 6. scene_bench's cold pass met its budgets (save 9.1 ms, load 13.6 ms, validate 8.3 ms), but the 50 measured rounds ran about 2.4x slower (22.1 ms, 31.8 ms and 20.2 ms). Is the slowdown inside scene_bench or the engine, or is it outside load?

Evidence that it is the machine, not the code:

- `scene_bench --machine-test 25` runs the CPU probe in a loop, with no OHAO code. The probe took 2.314 ms for the first ~3 s, then 4.5-8 ms for the rest of the run.
- A 250-round run: rounds 0 to ~65 had the CPU probe at ~2.4 ms and save at ~9.2 ms. After that the probe was 5.5-8 ms and save ~24 ms. The operation time follows the probe, which runs no OHAO code.
- Clean rounds meet every budget: save 9.263 ms, load 13.418 ms, validate 8.535 ms, snapshot 0.650 ms, restore_unchanged 0.697 ms, apply 0.202 ms, query 1.317 ms.
- The first gated run with the new record showed UnrealEditor at 138.2% of a core, and a calibration after the rounds of 60.976 ms (x2.63 of the reference).
- A 150 ms gap between rounds did not help (UnrealEditor at 259% of a core). That option was removed.
- A 20-round `--no-scale` run met the budgets (save 9.49 ms, load 13.71 ms, validate 8.53 ms). The full `--iters 20` run was bimodal (median ~9.7 ms, p95 25 ms).

Conclusion: on this laptop a busy core slows after about 3 s of sustained load, and more with UnrealEditor and other bots' work running. Nothing in OHAO grows across rounds. No budget was changed.

This step's final runs (2026-10-23). Each is noisy by rule.

| scene_bench run | calibration (x ref) | other processes (% of a core) | contended rounds | save_pretty ms | load ms | validate ms | snapshot ms | restore_unchanged ms | apply_100 ms | query_1000 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| gate attempt 3 | x1.05 (before 24.410, after 23.013, ref 23.184) | 196.4 (UnrealEditor 94.3, python.exe 87.8) | 16/50 | 8.835 | 12.523 | 7.974 | 0.612 | 0.658 | 0.193 | 1.206 |
| by hand, after the gate | x1.32 (before 30.609, after 23.164) | 211.0 (UnrealEditor 101.9, python.exe 96.4) | 27/50 | 9.008 | 12.774 | 8.192 | 0.614 | 0.676 | 0.192 | 1.222 |

In both runs every op's median over the measured rounds met its budget (save 10 ms, load 20 ms, validate 10 ms, snapshot 1 ms, restore_unchanged 1 ms, apply 0.2 ms, query 2 ms). The CPU probe's median stayed at 2.319 and 2.356 ms (p95 2.341 and 2.566 ms), so the core did not slow as it had in the earlier runs. Contended rounds were flagged by the memory probe (median 2.627 and 3.412 ms, minimum ~2.21 ms). Their op medians equal the clean rounds': save 8.781 ms clean vs 8.910 ms contended, and 8.981 ms vs 9.019 ms in the second run. The python.exe process at 88-96% of a core was not OHAO's. tools/check.py blocks reading ctest's output, and the process was still running after check.py exited (another session's work). It was left alone.

| serve_bench run | calibration | other processes (% of a core) | world_hash_rpc ms | world_diff_1pct_rpc ms | world_info_rpc ms | world_apply_100_rpc ms | world_step_1_rpc ms | log_ring_80 ns |
|---|---|---|---|---|---|---|---|---|
| gate attempt 3 | x1.04 | 225.9 (UnrealEditor 95.3, python.exe 88.5, explorer.exe 15.0) | 0.5369 | 0.4664 | 0.0213 | 0.0941 | 0.0396 | 182.245 |
| by hand | x1.00 | 204.5 (UnrealEditor 97.9, python.exe 96.5) | 0.5218 | 0.4565 | 0.0187 | 0.0867 | 0.0358 | 185.445 |

Every serve_bench op was under budget (hash 1.0 ms, diff 2.0 ms, info 0.15 ms, apply 0.4 ms, step 0.5 ms, log_ring_80 300 ns). The runs are noisy by rule (other processes above 50% of a core), so they are not counted as passes. The five-consecutive-pass streak from step 7 is not extended by this step.

## 6. Known gaps

- `ohao_game` is not yet a scene file and has no session. LiveHost, `ohao serve --window` and `helmet_game.scene.json` are step 9. Until then only tests and `ohao serve` (headless) drive `Session::step`.
- No bench run in this step was quiet. scene_bench and serve_bench have no passing run in step 8. The machine was shared with UnrealEditor and another session's python the whole time.
- scene_bench's contended-round rule flags rounds by the memory probe even when op times are unchanged (both final runs). It is kept as built (conservative).
- A recording replays with `verify` only on an engine build with the same saved types: its checkpoints are world hashes, which change when a component type is added (the header's `schemaHash` names the build). `verify: false` replays the input without the check.

## 7. Running the tests

```
build/Release/runtime_tests.exe
build/Release/api_tests.exe
build/Release/systems_tests.exe
build/Release/scene_tests.exe
python tests/serve/check_serve.py
build/Release/scene_bench.exe --check tests/bench/scene_budgets.json
build/Release/scene_bench.exe --machine-test 25
python tools/check.py --all
```
