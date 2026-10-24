# Sub-project 4, step 10: the named pipe and the remaining caps

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Transport and framing", "Security" with "Bounded work", as built; "Test plan" test 15; "Build order" step 10; step 9's note with Frank's ruling)
**Status:** Built. Every functional test passes (35 of 37 gate tests; the two benches were noisy in every attempt, from outside load). No budget or tolerance was changed.

## 1. Summary

`ohao serve` can now serve a Windows named pipe as well as stdin and stdout. The pipe accepts only the current user, refuses remote clients, and takes one client at a time. The remaining bounds on work and memory are in place: the batch sizes of `world.apply` and `input.inject` (stated in their schemas as `maxItems`), the injected events waiting for their steps, and the queue of messages waiting for the engine; step 5's message, step, log-page and query caps stay. The batch caps are checked before any element is decoded. No existing bug was found. Two benches were noisy in every attempt, so their budgets are neither met nor missed in this step.

## 2. What was built

- `rpc::PipeTransport` (`ohao/rpc/transport.{hpp,cpp}`, Windows only; elsewhere `--pipe` is a usage error) and `ohao serve --pipe NAME`, in the headless host and with `--window`. It serves `\\.\pipe\ohao-NAME` with these properties:
  - `PIPE_REJECT_REMOTE_CLIENTS`.
  - A protected DACL `D:P(A;;GA;;;<current user SID>)`: one entry, no inherited entries, no default read access for Everyone.
  - One instance. A second client gets ERROR_PIPE_BUSY.
  - `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a name another process already serves is an error, not a second instance beside a squatter.
  - Overlapped I/O with a stop event. The server's reader thread waits for the client in its first read, and `Server::stop()` ends that wait, so closing a window nobody connected to works.
  - One client per process. Its disconnect is the end of input, as EOF on stdin is; the session ends with exit 0. stdin is not read.
  - NAME is 1 to 64 characters of [A-Za-z0-9._-].
  - The pipe is created before the window or the scene, so a bad or taken name fails at once (exit 1, E_COMMAND on stdout).
  - On exit with the client still connected, the pipe is flushed before it is disconnected.
  - stderr logs "serving on <path>" and "a client connected to <path>".
- Remote refusal, measured. With the single instance free, opening `\\localhost\pipe\ohao-NAME`, `\\127.0.0.1\pipe\...` and `\\<hostname>\pipe\...` (the SMB redirector, which is a remote client to the pipe) each failed with error 5 (ERROR_ACCESS_DENIED). A pipe name that does not exist gives error 2 through the same path. A control pipe with the same current-user DACL but without PIPE_REJECT_REMOTE_CLIENTS admits the `\\localhost` client, so the refusal is the flag's. With the flag removed from PipeTransport, the rpc_tests remote test failed (the client got in), which shows the test catches its absence; the flag was restored.
- Reflection: a `maxItems(n)` field attribute (`ohao/reflect/type_info.hpp`, `registry.hpp`) for array fields (vector, optional vector, free JSON). The tree decoder counts the array before reading any element (E_RANGE at the member, expected "at most N elements", got "N elements"). The streaming fast path declines such structs (they are params structs only). Both schema writers emit `maxItems`. On a non-array field the registry records an error.
- Caps:
  - `world.apply` takes at most 10,000 commands and `input.inject` at most 10,000 events per call (`kMaxApplyCommands`, `kMaxInjectEvents` in `ohao/api/params.hpp`, as `maxItems`).
  - At most 100,000 injected events may wait for their steps at once (`kMaxQueuedInput`, E_STATE "step the world first").
  - The caps from step 5 stay: message 64 MiB (`--max-message`), `world.step` 1,000,000 frames, `log.read` 1,000 lines, `world.query` 100,000 entities.
- `rpc::Server`: the queue of messages read but not yet run is capped at 256 MiB (`ServerOptions::maxQueued`). At the cap the reader stops reading until the engine thread catches up, so a client that floods a busy engine blocks on its own writes instead of growing the server's memory; ping waits too then. `stop()` releases a reader waiting for space.
- Tests:
  - `rpc_tests` `sp4_t15_*` (5): the queue cap (with 4 KB queued the reader read at most 6 of 100 lines of ~1 KB; all 100 were answered in order as the engine drained the queue; stop() released a waiting reader within 2 s); pipe names; one local client of the current user (a request answered over the pipe, a second client ERROR_PIPE_BUSY, a second server on the name refused, the DACL read back with GetSecurityInfo: protected, one ACCESS_ALLOWED entry for the current user's SID, the client's disconnect ending run()); remote refusal (above); stop() ending a wait for a client.
  - `api_tests` `sp4_t15_batch_and_inject_caps_hold`.
  - `reflect_tests` `sp4_t15_max_items_is_checked_before_reading`.
  - `check_serve.py`: the caps (7 checks) and the pipe as a process (12 checks, stdin left open throughout).
  - `check_live.py`: the window host over the pipe (3 checks: connect; a step and an inline capture over the pipe; the client's disconnect ends it with exit 0, validation clean, no unasked readback).
- Docs: the spec as built (transport, security, bounded work, test 15, step 10 done, step 9 done with Frank's ruling); AGENTS.md (rpc_tests, check_serve and check_live rows); docs/ARCHITECTURE.md (ohao_rpc row); docs/agents/ohao-cli.md (`--pipe`, the caps); monograph reflection/api (the maxItems check, with a citation) and reflection/cli (no longer calls ohao serve "planned"). Site rebuilt: 92 leaf pages, every citation resolves.

## 3. Bugs found and fixed

None found in existing code.

## 4. Spec corrections and rulings

- Added beyond the step's list (ruled here, recorded in the spec): `input.inject`'s 10,000-event and 100,000-waiting caps, the server's 256 MiB queue cap, and reflection's maxItems. Reason: each is a way for one client to make the server hold or do unbounded work, which "Bounded work" rules out.
- The caps are in the schemas an agent reads (maxItems), so `schemaHash` changed once: 2f1413bfc516b03d to 94eb6d1669f2d5d1. The CLI goldens (`tests/cli/expected/schema.out`, `run_script.out`) and the serve goldens (`tests/serve/expected/errors.jsonl`, `mcp.jsonl`, `native.jsonl`) were updated after reading the diffs: only the hash changed. `tools/list` is 14,543 bytes for 22 tools (budget 24,576).
- One client per process. The spec said "one client at a time"; the client's disconnect now ends the session, as EOF on stdin does.
- Frank's ruling on step 9 (2026-10-24): the `window_readback` (2 ms) and `window_capture_inline` (20 ms) budgets stay exactly as they are. Their misses are open items for sub-project 2b (section 7). The ruling is recorded in the spec's step 9 note, the GPU budgets paragraph and "Frank actions".

## 5. Results

- `python tools/check.py --all`: build 17.6 s, doctor ok; 37 tests, 35 passed. scene_bench and serve_bench were noisy in all three attempts each, so they are not counted as passes.
- Per-test counts from that run: check_cli_cpu 25, check_scene_schema 17, check_tests 8, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 162, check_cli 29, check_validation 2, check_live 25, render_golden 10, check_docs 59, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 29, ecs_tests 26, reflect_tests 38, scene_tests 60, api_tests 42, systems_tests 9, rpc_tests 18, log_tests 10. ohao_game_presentation, monograph_structure and reflect_exhaustive_floats passed.
- New in this step: rpc_tests 5 (`sp4_t15_*`), api_tests 1, reflect_tests 1, check_serve 19 (162 from 143), check_live 3 (25 from 22).
- Render goldens 10/10 (no rendering code changed).

### Benchmark

Every run was noisy (other processes above half a core); none counts as a pass or a regression.

serve_bench, last gate attempt: calibration 23.21 / 23.27 ms against the reference 23.18 ms; outside load 348.9% of a core (python.exe 90.1%, chrome.exe 78.7% + 30.4% + 20.3% + 20.3%, explorer.exe 59.6%, UnrealEditor.exe 24.1%, claude.exe 12.7% + 6.3%). Medians, all within budget except the two ruled window budgets:

| op | median | p95 | budget |
|---|---|---|---|
| world_info_rpc | 0.0232 ms | 0.0399 | 0.15 |
| transport_overhead | 0.0223 ms | 0.0384 | 0.10 |
| world_query_one_rpc | 0.0229 ms | 0.0477 | 0.20 |
| world_apply_100_rpc | 0.1138 ms | 0.1635 | 0.40 |
| world_step_1_rpc | 0.0453 ms | 0.0797 | 0.50 |
| world_hash_rpc | 0.6222 ms | 0.7799 | 1.0 |
| world_diff_1pct_rpc | 0.5284 ms | 0.6810 | 2.0 |
| log_below_threshold | 0.2035 ns | 0.4087 | 5 |
| log_ring_80 | 173.7 ns | 280.1 | 300 |
| tools_list_bytes | 14,543 B | | 24,576 |
| window_step_present | 0.6293 ms | 6.4417 | 8 |
| window_readback | 5.4554 ms | 6.8948 | 2 (open for 2b) |
| window_capture_inline | 32.1297 ms | 40.6036 | 20 (open for 2b) |
| window_capture_png | 21.2259 ms | 34.1202 | 30 |
| reader_cpu_idle_600 | 0.098 ms over 608 idle steps | | 1 |

scene_bench, a standalone run after the gate (the same picture as the gate's three attempts): its calibration loop took 77.4 / 69.9 ms against its 23.2 ms reference (x3.34), beside 356.1% of a core of other processes (chrome.exe 91.4% + 52.2% + 21.2%, python.exe 88.6%, UnrealEditor.exe 38.6%, node.exe 22.2%). Only 2 of 50 rounds were contended, so the whole machine was slow. bench10k medians: save_pretty 27.72 ms (budget 10), load 40.39 (20), validate 26.79 (10), snapshot 1.134 (1), restore_unchanged 1.925 (1), restore_1pct 1.962 (2), restore_empty 6.560 (10), apply_100_field_set 0.488 (0.2), query_1000_ids 4.146 (2).

The slowdown against step 9's run (save 9.145, load 13.238, validate 8.284 ms) is uniform: save_pretty x3.0, which runs no decode code at all; load x3.05; validate x3.2; against the calibration loop's x3.34 on unchanged code. Step 10's only change on scene_bench's paths is one branch per field in the tree decoder (the maxItems check). The budgets are unchanged.

## 6. Known gaps

- The pipe is Windows only.
- Neither bench has a non-noisy run in this step.
- The rpc_tests remote test depends on loopback SMB. Where it is unavailable the control proves nothing and the test says so (it ran with the control here).
- From step 9, still open: the DamagedHelmet lies on its back (pre-existing; glTF root rotation); the examples read the lagged ring (`getPixelSpan`).

## 7. Open items owned by sub-project 2b

Frank's ruling, 2026-10-24: `window_readback` (budget 2 ms; 5.4554 ms median here) and `window_capture_inline` (budget 20 ms; 32.1297 ms here) miss on the deferred frame's own GPU cost (one Cornell deferred 1280x720 frame is 5.0 to 5.6 ms of GPU time). Sub-project 2b makes the frame cheaper (RT GI samples, RT shadows), which may change the deferred goldens. Steps 10 and 11 do not work on it.

## 8. Running the tests

```
build/Release/rpc_tests.exe
build/Release/api_tests.exe
build/Release/reflect_tests.exe
python tests/serve/check_serve.py
python tests/serve/check_live.py
build/Release/ohao.exe serve --pipe NAME
python tools/check.py --all
```
