# Sub-project 4, step 5: protocol core and headless ohao serve

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` ("Method params are reflected structs", "Protocol", "Security" and their as-built notes; "Build order" step 5; "Test plan" tests 7-10 and 15)
**Status:** Done.

## 1. Summary

One engine process now serves a whole agent session. `ohao serve` answers JSON-RPC 2.0 on stdio (newline-delimited) for native clients (method names) and MCP clients (initialize, tools/list, tools/call, logging/setLevel). Every method's params are a registered struct, so one definition is the validation, the catalogue's JSON Schema and the MCP input schema.

## 2. What was built

- Params structs (`ohao/api/params.{hpp,cpp}`): one struct per method (14 methods) in their own registry, `api::paramsRegistry()`, with docs, limits, units and `required`; enums for save layout, capture mode, denoiser, post effect, log level and channel. `decodeParams` decodes with the scene files' typed decoder (E_UNKNOWN_FIELD with suggestions, E_TYPE, E_ENUM, E_RANGE with the limits), skips MCP's `_meta`, and checks required members (E_COMMAND). The hand-written `checkKeys` calls for method params are gone (commands keep theirs).
- Reflection: `Kind::Json` holding a `reflect::JsonRef` (a pointer into the request's document: free JSON such as world.apply's commands is not copied), `Kind::Int64`, `.required()`, limits on optionals and vectors of numbers, `readStruct`'s ignored key, and `writeParamsSchema` (a self-contained JSON Schema: no $ref, additionalProperties false, defaults, limits, required). The catalogue's method table now holds each method's params as that schema.
- `Session::call` over an already parsed request returns `{status: Ok, UnknownMethod, BadParams, Failed or Cancelled; result; errors}` and takes a `CallControl` (a cancel flag checked between whole steps of world.step, a progress callback). `api::exitClass` gives an error code's class (shared with the CLI). `Session::setHostKind`; `Session::setPathPolicy`.
- New methods: `engine.info` (protocolVersion 1, schemaHash, version 0.1.0, host, time: lockstep only until step 7) and `log.read` (since, level, channel, limit). `scene.load` gains `replace` (validate first, then clear the world; snapshots kept). `world.info` gains `host`. New error codes in the catalogue: E_ACCESS, E_STATE, E_REPLAY.
- The sandbox (`ohao/api/paths.{hpp,cpp}`): paths in params are project-relative with '/'; no `..`, no backslashes; reads inside the root, writes inside `--out` (default `<root>/agent_out`, git-ignored, created with a write's parent directories); absolute paths only inside an `--allow-path` root, which admits reads and writes; containment checked element by element on canonical paths. Anything else is E_ACCESS. The CLI sets no policy and keeps paths as given.
- `ohao/rpc` (library `ohao_rpc`, CPU-only): `LineReader` (newline framing, CR dropped, an oversize line skipped without being kept), `StdioTransport` and `separateStdout()` (descriptor 1 to stderr, a private binary copy of stdout for the protocol; the CLI now uses it too), and `Server`: a reader thread answers ping and notifications/cancelled; everything else, including the replies to invalid lines, is queued and run by the engine thread in arrival order. Errors: -32700 unparseable, -32600 not a request / batch / over --max-message (64 MiB default), -32601 unknown method or tool (data.suggestion), -32602 params (data: diagnostics), else the exit class 1/2/3 with data {code, diagnostics}; over MCP a domain error is a tool result with isError true. Ids echo byte for byte. notifications/progress at most every 250 ms when the params carry `_meta.progressToken`. End of input cancels the running request, drops the queue and the server exits 0. MCP: initialize negotiates 2025-06-18 or 2025-11-25 (unknown: 2025-11-25), capabilities tools and logging, an instructions string under 600 characters; tool names are method names with '.' -> '_'; results are one text block with the compact result JSON; logging/setLevel installs a log sink sending notifications/message {level, logger: channel, data}.
- `ohao serve [scene] [--root DIR] [--out DIR] [--allow-path DIR]... [--seed S] [--log-level L] [--max-message N]`: headless, lockstep; render.capture works through the existing render host.
- Test 7 and test 8: `rpc_tests` (suite, label quick, 10 tests): framing, ordering, the error table, ids, cancellation of a running and a queued request, ping while busy, progress, end of input; initialize, tools/list coverage and size, tools/call results and errors, logging/setLevel.
- `tests/serve/check_serve.py` (label quick, 105 checks): test 9 protocol goldens, 3 scripted sessions in tests/serve/sessions with expected stdout, byte-identical twice; test 8 every tool's inputSchema is a valid draft 2020-12 schema through jsonschema, accepts an example and refuses an unknown member, and the server agrees on both; test 10 `rpc_print_server` prints through printf, puts, fprintf, fwrite, fputs, putchar, cout, _write(1) and WriteFile on the console handle, every stdout line stays a JSON-RPC message and every print reached stderr; test 15 paths part: absolute, '..', writes outside --out, --allow-path admits exactly its root, the message-size and step caps.
- Tests elsewhere: `api_tests` +5 (params lint and closed schemas, engine.info and log.read, scene.load replace, test 15 sandbox, test 7 cancellation leaves the world bit-identical to the same number of whole steps). `reflect_tests` +1 (ohao_rpc includes no Vulkan, GLFW, renderer, GPU or window header).
- Docs: AGENTS.md, docs/ARCHITECTURE.md (the ohao_rpc row, the "add a method" recipe with the params struct), the CLI guide (a serve section with a run example, engine.info, log.read, scene.load replace, the new error codes), the verify skill draft, the spec's as-built notes, the monograph units reflection/api and reflection/cli; site rebuilt (92 leaf pages).

## 3. Bugs found and fixed

1. Limits were refused on `optional<number>` fields (the registry only allowed bare numeric kinds), although the decoder and schema handle them; the new params lint test caught it on world.step's dt, render.capture's exposure and log.read's since.
2. The print lint (step 4) flagged the stdout separation now in ohao/rpc/transport.cpp; it is the one place that must touch stdout and has an allowlist entry with that reason.

## 4. Spec corrections

- The params structs live in their own registry, not the engine's (they are not scene types, so the scene schema and the catalogue's type list are unchanged), and the free-JSON kind is a reference into the request (JsonRef), not an owning JsonValue, so a large world.apply batch is not copied.
- End of input also drops queued requests, not only the running one (stated in the spec now); a client waits for its last reply before closing stdin.
- log.read was built in this step although no step named it.
- The full Host interface is not built yet: setHostKind reports the host and render.capture keeps setRenderHandler; time control comes in step 7.

## 5. Results

- rpc_tests 10/10; check_serve 105/105; api_tests 22/22; reflect_tests 36/36; scene_tests 60/60; check_cli 29/29 (CPU 25/25).
- Goldens updated after reading the diffs: tests/reflect/golden and tests/scene/golden (the three new error codes and the schemaHash), five CLI cases (schemaHash; world.info's host; the unknown-command list gains serve; the unknown-method list gains engine.info and log.read; render_bad_size now reports width and height together). schemaHash is 8a3083d7fd9f023a.
- tools/list: 7,811 bytes for 14 tools (budget 24 KB).
- `python tools/check.py --all` (build 46.6 s, doctor ok with the tools.cmake warning): 35/36 passed, 36 tests registered (step 4: 34; new: rpc_tests, check_serve). Counts: check_cli_cpu 25, check_scene_schema 16, check_tests 7, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 105, check_cli 29, check_validation 2, render_golden 10, check_docs 59, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 21, ecs_tests 26, reflect_tests 36, scene_tests 60, api_tests 22, systems_tests 6, rpc_tests 10, log_tests 10, log_bench 2; ohao_game_presentation, monograph_structure and reflect_exhaustive_floats (355.3 s) passed. scene_bench failed (see Benchmark).
- Render goldens 10/10 (the deferred four exact).

### Benchmark

- api_tests in-process: world.apply 100 field.set median 0.059 to 0.062 ms (params now decoded through the struct; the commands are not copied).
- scene_bench failed as in steps 2-4, under external load: its cold bench10k block measured save_pretty 9.311 ms, load 13.519 ms, validate 8.405 ms, apply_100_field_set 0.227 ms (step 4: 8.993, 13.050, 8.052, 0.230); the warm rounds that follow measured 22.235, 31.119, 20.558, 0.400 ms. A log_bench run right after recorded blender.exe at 94.3% of a core, UnrealEditor.exe at 27.5% and claude.exe at 11.8% (168.9% in total, so that log_bench run was itself noisy). Per the reviewer's ruling this is external-load contention, recorded, not a regression; no budget was changed.
- log_bench passed inside the gate run.

## 6. Known gaps

- The serve budgets (round trips, transport overhead) and serve_bench are step 6.
- Time control (time.get/set, world.step watch, dt default from the session) is step 7; input is step 8; the window host and live capture are step 9; the named pipe and the remaining caps are step 10.
- Test 8's examples are in check_serve.py until the agent guide (step 11) has its own.
- scene_bench cannot be judged while other processes load the machine.
- The verify skill change waits in docs/agents/skills/ until Frank installs it.

## 7. Running the tests

```
build/Release/rpc_tests.exe
python tests/serve/check_serve.py
python tests/serve/check_serve.py --update
build/Release/api_tests.exe
build/Release/ohao serve --out build/serve_out
python tools/check.py --all
```
