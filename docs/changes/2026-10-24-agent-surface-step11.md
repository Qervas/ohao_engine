# Sub-project 4, step 11: acceptance, the serve guide, recipes and the monograph module

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` (tests 19 and 20 as built; "Recipes added to docs/ARCHITECTURE.md" as built; "Programs" (.mcp.json); "Frank actions"; "Build order" step 11; "Done criteria")
**Status:** Built. Every functional test passes (36 of 38 gate tests; the two benches were noisy in the gate). serve_bench has a clean run with every budget met except the two window budgets Frank assigned to 2b; scene_bench has no clean run (section 5). Waiting on Frank: `.mcp.json` and the manual Claude Code run, and deleting four stray files. No budget or tolerance was changed.

## 1. Summary

This is the last step of sub-project 4. The scripted acceptance run of the agent surface now goes end to end over MCP against a hidden window: load, edit, snapshot, record, a 61-step flight, replay to the same hash, captures and logs. The agent guide for a live session is written (`docs/agents/ohao-serve.md`), the ARCHITECTURE.md recipes are in place and were each followed once, and the monograph has a new module 13, "Agent control surface". No existing bug was found in engine code. Four statements in the documentation were wrong and were fixed. The repo's `.mcp.json` and the manual Claude Code run could not be done in this session and are Frank's (section 7).

## 2. What was built

- Test 19: `tests/serve/e2e_mcp.py`, a stdlib-only MCP client (newline-delimited JSON-RPC on the server's stdio, every call a `tools/call`), registered as CTest `e2e_mcp` (label gpu). It starts `ohao serve --window 960x540 --hidden` with `OHAO_VALIDATION=1` and runs:
  - initialize (MCP 2025-11-25) -> tools/list (22 tools) -> engine_info (window, lockstep) -> scene_load helmet_game (19 entities, seed 00000000000000e2) -> world_summary;
  - world_apply (the spinning cube's baseColor to green, with its inverse) -> world_snapshot -> input_record -> input_inject a 61-step flight (move_forward from step 0, move_up from step 30, both released at step 60);
  - world_step 61 with watch: at moveSpeed 3 m/s the camera went [0, -6, 25.5] -> [0, -4.4999943, 22.500046], i.e. 3 m forward and 1.5 m up, checked against the scene's moveSpeed;
  - render_capture inline JPEG (57,564 bytes) plus a PNG file -> log_read -> input_record stop (61 steps, 4 events) -> world_hash -> world_restore -> input_replay (61 steps, 2 checkpoints, every one matching) -> world_hash equal (8c873875786a7fa7) -> render_capture again: the PNG file byte for byte and the inline JPEG equal the first;
  - close stdin: exit 0, validation 0 through teardown, no unrequested readback.
  - 19 checks.
- Test 20: `tests/docs/check_docs.py` also runs `docs/agents/ohao-serve.md` (3 commands: an MCP initialize over pipes; a headless record -> restore -> replay through `ohao run --script docs/agents/examples/fly.jsonl`, compared line for line with its expected output and ending with the same hash cbe53378e4f01f3a before and after the replay; and e2e_mcp.py) and the new recipes' commands in docs/ARCHITECTURE.md. check_docs: 65 commands pass in the gate.
- `docs/agents/ohao-serve.md`: the agent guide for a live session. It covers connecting (the hosts, the named pipe, the Claude Code .mcp.json), orienting, editing, playing, recording and replaying, looking, reading logs, a whole session, and limits. Measured sizes in it: initialize 725 bytes (instructions 551), tools/list 14,543 bytes for 22 tools, a 768x432 helmet-room JPEG about 57 KB.
- docs/ARCHITECTURE.md recipes: "add an API method" (its own recipe now; the path world.hash/world.diff in step 7 and the input methods in step 8 took it), "add a render pass (and its shader)", "add a golden scene".
- Recipes followed once each in this step:
  - The render pass recipe, with a throwaway compute pass (mean luminance of each 8x8 tile of the lit HDR image, shader `shaders/postprocess/luminance_tiles.comp` -> `postprocess_luminance_tiles.comp.spv`). It built; the shader glob picked it up; `ohao doctor` counted 41 sources and found every SPIR-V fresh; check_validation passed (deferred Cornell and helmet clean); `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` reported 0 validation messages including teardown and 0 readbacks (the pass initialized at 240x135 tiles); all 10 render goldens passed with the deferred ones exact. Then the pass was taken out of DeferredRenderer, since nothing read its output; `deferred_renderer.{hpp,cpp}` are identical to the committed version.
  - The golden recipe, for real: `cli_helmet_game_deferred` (helmet_game.scene.json through `ohao run`, deferred, 30 frames, TAA + SSAO + bloom, exposure 1; golden `tests/golden/helmet_game_deferred.png`). It was looked at, then checked exact (max_abs 0) on two further runs.
- Monograph: module 13 "Agent control surface" (`site/tools/monograph/tree/agent.py`, registered after module 12) with six units, every citation resolving:
  - protocol (1,782 words, 15 citations);
  - transports (about 1,470 words, 14);
  - live-host (about 1,480 words, 15);
  - time-input (1,773 words, 15);
  - capture (1,690 words, 15);
  - hash-diff (about 1,170 words, 8).
  Five were drafted by a subagent before the previous session was cut off. All six were reviewed against the code in this session; three wrong statements were corrected (an unsourced "step-9 record" claim, a test described as splitting at every byte position, and a test's list of edits).
- Also fixed: the reflection/instantiate page still listed `tools/ohao/scene_host.hpp`, which moved to `ohao/runtime/scene_view_apply.hpp` in step 9. The site was rebuilt: 98 leaf pages across 18 modules; the structure test passes (16 tests).
- docs/ROADMAP.md: row 4 done (2026-10-24), 2b next, and 4's two GPU budgets added to 2b's scope.
- AGENTS.md: the serve guide linked, e2e_mcp in the GPU gates, 11 render goldens with five exact deferred ones.
- The verify skill draft (`docs/agents/skills/verify/SKILL.md`, waiting for Frank's install) gains check_live and e2e_mcp.

## 3. Bugs found and fixed

None in engine code. Four documentation statements were wrong and were fixed: the stale scene_host.hpp page path, and the three statements in the drafted monograph units listed in section 2.

## 4. Spec corrections and rulings

- "Add a render pass" and "add a shader" are one recipe, since a pass and its shader are written together.
- The render pass recipe was followed with a throwaway pass, then the pass was removed from the renderer, per "forward only" (no code without a user).
- `.mcp.json` and the manual Claude Code run could not be done in this session: the harness refuses writing `.mcp.json` (an MCP server configuration) and launching the `claude` CLI. Both are recorded as Frank actions with the file's exact content (section 7). Test 19 covers the scripted MCP acceptance.
- Done criteria not met, with reasons (recorded in the spec's step 11 note):
  - The two window GPU budgets are open for 2b by Frank's ruling of 2026-10-24, so `serve_bench --check` and with it `check.py --all` cannot exit 0 until 2b.
  - No budget had five consecutive passes in steps 9-11, since every gate bench run was noisy from outside load.

## 5. Results

- `python tools/check.py --all`: build 62.0 s, doctor ok; 38 tests, 36 passed. scene_bench and serve_bench were noisy in all their attempts (outside load: another project's Blender texture pipeline in python.exe at about 95% of a core, Unreal Editor, Chrome). They are not counted as passes.
- Per-test counts from that run: check_cli_cpu 25, check_scene_schema 17, check_tests 8, test_golden_diff 11, check_doctor 7, check_doctor_gpu 1, check_serve 162, check_cli 29, check_validation 2, check_live 25, e2e_mcp 19, render_golden 11, check_docs 65, env_cdf_test 2, sobol_test 8, denoise_parse_test 5, renderer_test 4, instantiate_gpu_test 5, scene_view_gpu_test 7, restore_render_gpu_test 14, deferred_determinism_gpu_test 43, physics_backend_tests 59, audio_system_tests 13, force_generator_tests 54, engine_tests 58, runtime_tests 29, ecs_tests 26, reflect_tests 38, scene_tests 60, api_tests 42, systems_tests 9, rpc_tests 18, log_tests 10. ohao_game_presentation, monograph_structure and reflect_exhaustive_floats passed.
- New in this step: e2e_mcp 19 checks; render_golden 11 scenes (one new, exact); check_docs 65 commands (from 59).
- Render goldens 11 of 11, the five deferred scenes exact (max_abs 0).

### Benchmark

To get clean runs, the bench was held until outside load dropped. A sampler watched every other process for about two hours (12:04-13:50): 347-405% of a core while the Blender pipeline ran, then 70-100% with Unreal Editor at about 35% and System at about 20%. Then, with nothing else of this session running:

- serve_bench, CLEAN (not noisy): calibration 26.141 / 23.375 ms against the 23.184 ms reference (x1.13); other processes 28.3% of a core (UnrealEditor.exe 20.9%); window part 47.7% (UnrealEditor.exe 31.3%). Results 13 of 15: every budget met except window_readback and window_capture_inline, which are open for 2b by Frank's ruling.
- scene_bench, three runs, each NOISY by its contention rule (32, 33 and 36 of 50 rounds contended), although calibration was x1.02, x0.99 and x1.00 and other processes were 46.3%, 46.9% and 41.4% of a core (Unreal Editor 22.5-32.0%). The flags come from the memory probe, which was bimodal. In the second run, 15 rounds were at 2.13-2.34 ms and 35 at 2.73-4.11 ms (fastest 2.13 ms, so 1.5x is 3.2 ms, which 32 rounds exceeded), while the CPU probe stayed at 2.317-2.498 ms. Every bench10k median was within budget in all three runs, and the clean-round and contended-round medians agree (second run: save_pretty 8.786 vs 8.843 ms, load 12.523 vs 12.454, validate 8.206 vs 8.190). The memory contention is consistent with Unreal Editor running; a clean run needs it closed. The contention rule was not changed.

### The final sub-project 4 budget table

The serve_bench column is from the clean run of this step. The scene_bench rows are from its last run, which was noisy, and are given for reference.

serve_bench (clean):

| op | median | p95 | budget | verdict |
|---|---|---|---|---|
| world_info_rpc | 0.0183 ms | 0.0239 | 0.15 | met |
| transport_overhead | 0.0177 ms | 0.0232 | 0.10 | met |
| world_query_one_rpc | 0.0161 ms | 0.0223 | 0.20 | met |
| world_apply_100_rpc | 0.0847 ms | 0.1033 | 0.40 | met |
| world_step_1_rpc | 0.0336 ms | 0.0423 | 0.50 | met |
| world_hash_rpc | 0.5091 ms | 0.5419 | 1.0 | met |
| world_diff_1pct_rpc | 0.4309 ms | 0.4605 | 2.0 | met |
| log_below_threshold | 0.1957 ns | 0.2581 | 5 | met |
| log_ring_80 | 163.91 ns | 176.29 | 300 | met |
| tools_list_bytes | 14,543 B | | 24,576 | met |
| window_step_present | 0.4817 ms | 5.9720 | 8 | met |
| window_readback | 5.3388 ms | 6.1820 | 2 | over: open for 2b (Frank's ruling) |
| window_capture_inline | 34.3120 ms | 35.6318 | 20 | over: open for 2b (Frank's ruling) |
| window_capture_png | 22.7411 ms | 25.9698 | 30 | met |
| reader_cpu_idle_600 | 0.2972 ms (613 idle steps) | | 1 | met |

Recorded, not gated: process start to initialize 93.5 ms; scene.load bench10k over stdio 52.1 ms; window host start to engine.info 2,164 ms; first world.step to its present 0.8 ms.

scene_bench bench10k (last run, noisy by the memory probe; within budget): save_pretty 8.851 (budget 10), load 12.648 (20), validate 8.172 (10), snapshot 0.549 (1), restore_unchanged 0.607 (1), restore_1pct 0.472 (2), restore_empty 1.800 (10), apply_100_field_set 0.147 (0.2), query_1000_ids 1.167 (2). bench100k ratios to bench10k cold: x6.4-x8.1 (save 6.4, load 7.1, validate 7.3, snapshot 8.1, restore_unchanged 7.3, restore_1pct 7.2, restore_empty 7.5).

Five consecutive passes (the spec's acceptance rule): not reached for any budget in steps 9-11.

## 6. Known gaps

- scene_bench has no clean run in this step (section 5).
- The repo's `.mcp.json` and the manual Claude Code run are Frank's (section 7).
- Four stray untracked files, which the harness would not let this session delete (rm, mv and git clean are denied): `ohao/render/deferred/luminance_pass.hpp`, `ohao/render/deferred/luminance_pass.cpp`, `shaders/postprocess/luminance_tiles.comp` (the recipe exercise; still compiled by the source globs, referenced by nothing) and `examples/model_viewer_old.cpp` (an older leftover). None is committed.
- From earlier steps, still open: the DamagedHelmet lies on its back (glTF root rotation, pre-existing; visible in the new golden too); the examples read the lagged readback ring.

## 7. For Frank

1. Create `.mcp.json` at the repo root with:
   `{"mcpServers": {"ohao": {"command": "build/Release/ohao.exe", "args": ["serve", "--window", "assets/scenes/helmet_game.scene.json"]}}}`
   Approve it once in Claude Code. Then run the manual check: ask Claude Code to load helmet_game, move the cube, fly the camera 60 steps with input_inject + world_step, capture inline, and read logs. Record what happened in this file.
2. Delete the four stray files listed under known gaps.
3. For a clean scene_bench run, close Unreal Editor (its memory traffic makes the memory probe bimodal), then run `build/Release/scene_bench.exe --check tests/bench/scene_budgets.json`.
4. Install the verify skill draft (`docs/agents/skills/README.md`).
5. The two window budgets stay open for 2b (Frank's ruling of 2026-10-24). 2b makes the deferred frame cheaper; the measured cause is the deferred frame's own GPU cost (one Cornell deferred 1280x720 frame is 5.0 to 5.6 ms of GPU time).

## 8. Running the tests

```
python tests/serve/e2e_mcp.py
python tests/docs/check_docs.py docs/agents/ohao-serve.md docs/ARCHITECTURE.md
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_helmet_game_deferred
python site/tests/test_monograph_structure.py
build/Release/serve_bench.exe --check tests/bench/serve_budgets.json
build/Release/scene_bench.exe --check tests/bench/scene_budgets.json
python tools/check.py --all
```
