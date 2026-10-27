# AGENTS.md — working on OHAO

The entry point for any agent (or person) building, testing or changing this repo.
Every command in a `console` block below is run by `python tests/docs/check_docs.py`
(`--slow` adds builds and long gates); if one stops working, this file is wrong.

OHAO is a C++20 game engine: a Vulkan 1.3 hybrid renderer (deferred raster with RT
shadows/GI, plus a path tracer), an EnTT world, reflection and canonical scene files, an
API for agents (`ohao/api`), the headless `ohao` CLI and `ohao serve`, the same API as a
JSON-RPC/MCP server on stdio (`ohao/rpc`). Windows/MSVC is the platform in daily use.

- **Code map, data flow, recipes:** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- **Driving the engine from an agent:** [docs/agents/ohao-cli.md](docs/agents/ohao-cli.md)
  (the CLI, every method and error code) and [docs/agents/ohao-serve.md](docs/agents/ohao-serve.md)
  (a live session over MCP: connect, orient, edit, play, look, record and replay, logs)
- **Designs and plans:** `docs/specs/`, `docs/plans/`; **what changed:** `docs/changes/`;
  **where it is going:** [docs/ROADMAP.md](docs/ROADMAP.md)
- **Monograph** (design units with code citations): `site/`

## Build

Use the real CMake. On this machine conda's `cmake` shadows it on PATH, so name the scoop
shim. Build with `-j6` at most: heavier parallel load has crashed this machine.

```console
$ C:/Users/djmax/scoop/shims/cmake -B build -S .   # slow
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6   # slow
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release --target shaders   # slow
```

- **Sources are globbed with `CONFIGURE_DEPENDS`**: a new `.cpp` or shader under a
  globbed directory is picked up by the next build, which re-runs the configure step
  itself. A new directory or target still needs a `CMakeLists.txt` edit.
- **The `shaders` target is not in `ALL`**, but every target that loads SPIR-V depends on
  it (the `ohao` CLI, the examples, the rendering tests), so building any of them builds
  the shaders. `python tools/check.py` builds it first regardless.
- Binaries land in `build/Release/`; SPIR-V in `build/Release/bin/shaders/`, named by
  flattening the path (`rt/pt_raygen.rgen` → `rt_pt_raygen.rgen.spv`).
- **New shaders are Slang** (`slangc` from the Vulkan SDK, pinned in
  `shaders/slangc.version`); a `.slang` with one `[shader(...)]` entry point gives
  `<flattened>.spv`, with several `<flattened>.<entry>.spv`, without one it is a module.
  Constants and structs shared with C++ live in `shaders/shared/layout.json`; regenerate
  their `.slang`/`.glsl`/`.hpp` with `python tools/gen_layout.py` (the check below fails
  until you do). See ARCHITECTURE.md, "add a render pass".

```console
$ python tools/gen_layout.py --check
```

## Gates

**One command builds and runs everything:**

```console
$ python tools/check.py   # slow
$ python tools/check.py --all   # slow
```

It finds the real CMake (refusing conda's), configures if needed, builds the `shaders`
target and then everything with at most 6 jobs, runs `ohao doctor` (below; a failing
environment stops the run there), runs every CTest test with the chosen labels, and
writes one verdict: `build/check/check.json`, also the last line it prints
(`{ok, labels, build, doctor, tests: [{name, labels, ok, seconds, passed, failed,
failures, tail}], counts}`), plus `build/check/junit.xml` and the logs beside them. A
failing test's entry carries the last 40 lines of its output. Exit 0 = all passed, 1 =
something failed, 2 = the environment is wrong (the message says how).

- **Labels** (one per test, `cmake/ohao_tests.cmake`): `quick` (CPU), `gpu`, `docs` — the
  default set — and `golden`, `bench`, `slow`, which `--all` adds. `--labels gpu,docs`
  picks your own set; `ctest --test-dir build -C Release -L quick` works without the
  script. GPU tests never run two at once; benchmarks run alone.
- **Registering a new suite:** `ohao_add_test(<name> LABEL <label> [GPU] COMMAND <exe>)`
  in its `CMakeLists.txt`, and end its `main` with `ohao::test::finish(...)` from
  `tests/common/harness.hpp` (or its `CHECK`/`RUN` macros), so the summary line is
  there. `check_tests` fails if a test executable is built but not registered.
- **The environment:** `ohao doctor` answers whether this machine can build, run and test
  OHAO, as `{"result": {ok, cpuOnly, checks: [{id, ok, level, detail, fix?}]}}`: the Vulkan
  instance, device, ray tracing and validation layer; every shader's SPIR-V present and
  newer than its source and every file it includes; the assets the checked-in scenes
  reference; glslc, slangc (at its pinned version), cmake (not conda's) and the harnesses'
  Python modules; and a 64x64
  deferred frame within 2 s (a wedged GPU after a crash fails here, not in every render
  test). Exit 1 when an error-level check fails; `--cpu-only` creates no device.

```console
$ build/Release/ohao doctor --cpu-only > /dev/null
```

Every suite exits 0 on success and nonzero on any failure; that is the pass criterion.
Every suite ends with one JSON line, `{"suite", "passed", "failed", "failures"}`. The
commands below run single suites by hand.

CPU suites (no GPU):

```console
$ build/Release/runtime_tests.exe > /dev/null
$ build/Release/log_tests.exe > /dev/null
$ build/Release/reflect_tests.exe > /dev/null
$ build/Release/scene_tests.exe > /dev/null
$ build/Release/api_tests.exe > /dev/null
$ build/Release/rpc_tests.exe > /dev/null
$ build/Release/systems_tests.exe > /dev/null
$ build/Release/physics_backend_tests.exe > /dev/null
$ build/Release/audio_system_tests.exe > /dev/null
$ build/Release/force_generator_tests.exe > /dev/null
$ build/Release/env_cdf_test.exe > /dev/null
$ build/Release/denoise_parse_test.exe > /dev/null
$ build/Release/sobol_test.exe > /dev/null
$ build/Release/level_physics_tests.exe > /dev/null
$ build/Release/combat_tests.exe > /dev/null
$ python tests/cli/check_cli.py --cpu-only > /dev/null
$ python tests/reflect/check_scene_schema.py > /dev/null
$ python tests/check/check_tests.py > /dev/null
$ python tests/golden/test_golden_diff.py > /dev/null
$ python tests/doctor/check_doctor.py > /dev/null
$ python tests/serve/check_serve.py > /dev/null
$ python tests/layout/check_layout.py > /dev/null
$ python tests/level/check_level_assets.py > /dev/null
```

| Suite | Covers |
|---|---|
| `runtime_tests` | the runtime shell without a window: fixed-step clock, the timeline (lockstep, realtime, pause), the input map (actions, axes, the virtual device, key names), swapchain choices, capture images (the PNG writer round trips through stb_image, crop, the preview downscale, JPEG), `render.stats`' per-pass medians, which footstep sound plays (round robin, the local folder first, `default`), which weapon sound each combat event plays (by shooter and surface), the click that captures the cursor never reaching the input map |
| `log_tests` | the logger: level filter, ring wrap and `dropped`, repeat collapse, channel filter, paging, concurrent writers, sinks, the 1 KB cut, the stderr format |
| `reflect_tests` | registry, schema export, JSON writer and reader, float formatting, the dependency rule, the source lints (nondeterminism, prints, literal light positions, float atomics in shaders) |
| `scene_tests` | engine registrations, ids, the scene codec, error corpus, migration, snapshots, instantiate (fake backend) |
| `api_tests` | methods, commands and inverses, queries, summaries, params structs, the path sandbox, cancellation, the time model (phases, `watch`, `E_STATE`, realtime vs lockstep), `world.hash` and `world.diff`, input (inject, state, record, replay), `render.stats`; levels and physics (`level.info` counts, spawns and size, `level.reload` keeping the hash, `physics.raycast` hits, misses and refused params, `physics.info`, the warnings); `physics.events` after a drop through `world.apply` and `world.step` (the landings, the cursor, refused params); `player.info` at the spawn and after a walk, `player.teleport` (onto the Step, at rest, refused params), `physics.contacts` (the resting crates, an entity's pairs, `max`), a restore mid-fall and a restored walk reaching the same `world.hash`; combat (`sp7_t18_*`): `combat.info` before and after the first steps (the round, three combatants in id order, under 1 KB), `combat.events` (a burst's shots, `round_start`, the cursor), `combat.damage` (on an enemy and on its body box, the dead taking none, both enemies dead: won, the player dead: the restart 3 s later back at the round start's `world.hash`), refused params, `E_STATE` without combat |
| `rpc_tests` | the protocol server in-process: framing, ordering, the error table, ids, cancellation, progress, ping while busy, MCP initialize/tools/logging, the queue cap; the named pipe (one local client, its DACL, a second client and a second server refused, remote clients refused against a control pipe) |
| `systems_tests` | engine-provided systems (spin, the fly controller) |
| `level_physics_tests` | level collision: colliders fitted from meshes (box, sphere, capsule, convex, mesh), `UCX_` hulls as one compound body, a dynamic mesh made convex, non-uniform and mirrored scale; rays against the blockout (spawns, doorway, wall, ramp), its crates stepped to rest, a render-only level, a still world doing no work; physics in the world's step (test 10): dropped crates land and rest at their authored places, contact begin events at the right step and speed, a settled world writing nothing, an edit teleporting a dynamic body, motion vectors over two steps in one frame, a destroyed body ending its contacts, trigger enter and exit, substeps, two runs identical bit for bit, the event log's cursor; the player (test 11): spawns at the player spawn facing its yaw, walks 4.0 m in 60 steps, stopped by the north wall, climbs the 0.3 m step, the platform and the ramp, pushes a crate, jumps 1.22 m, seen by a trigger; kinematic bodies moved by velocity; the camera (test 12): the head at eyeHeight, pitch clamped at ±89° (and set by an edited head), yaw turning the body, render interpolation drawing the head and a falling crate between steps while `world.hash` and the saved file stay the step's, `blockout_play.scene.json` viewed from the spawn before any step; footsteps (test 13): a footstep every 0.75 m walked and 0.95 m sprinted, the surface of the ground body (the blockout's `surf` hints, an edited `Collider.surface`, `default`), one land after a 0.5 m drop at the free-fall speed and none after 0.2 m, none standing or in the air; replay (test 14): `BodyMotion` following awake bodies (and neither saved nor hashed), restores replaying bit for bit mid-fall, mid-jump, from the loaded spawn and with crates resting on each other, sleeping bodies put back to sleep, a body put back however close it settled, a body rebuilt since the snapshot falling back to `BodyMotion` |
| `combat_tests` | combat (test 15): damage and death (the dead take none), 10 Hz exactly at 60 Hz steps, semi-automatic one round a press, the magazine and its reload (none during it, `reload` on a part-empty magazine only), a hit on a box with `Health` (point, normal, 25 damage), a round from inside the shooter's capsule through a trigger, a wall's surface and distance, the spread cone (10,000 draws, mean 2/3 of it), bloom growing and recovering, seeded patterns, recoil kicking and recovering, a turret hitting the player, the dead not firing (id order), two runs and a snapshot's replay bit for bit, the play scene's rifle; enemies (test 16): spawned at the enemy spawns, idle while the player is behind them, seeing and chasing, attacking after the reaction time (21 steps), bursts of 3 at 4 Hz with 0.8 s between, giving up 3 s after losing the line, woken by a hit, dead ones lying down with no character, their fire killing the player, two runs and a mid-fight snapshot replaying bit for bit, the play scene's two enemies; the round (test 17): `round_start` once, a restart 180 steps after the player's death equal to the round's start (`world.hash`, poses, health, ammo, a knocked crate), the `restart` key, a dead player neither walking, turning nor firing, a win once both enemies die (waiting for `restart`, which brings both back), no round without `Round`, the same play after a restart, and after a won round's restart, bit for bit, and the play scene's won round restarting to the round start's `world.hash` (its enemies' yaw of exactly 180, rotations bit for bit); presentation (`fx_*`): the muzzle light on for its 3 steps then off, impact marks (64 at most, the oldest reused, never saved or hashed, on an enemy's body box and following it, none on the player, cleared by a restart), the HUD list (health, ammunition, the crosshair opened by bloom, the hit marker for 0.15 s and the vignette for 0.3 s, DEAD with the restart countdown, CLEARED, nothing without a player) |
| `check_cli.py` | `ohao` CLI goldens: stdout, files and exit codes, each case run twice (a case's `mask` keys, timings such as `loadMs`, compare as 0) |
| `check_scene_schema.py` | the generated JSON Schemas in a stock validator (needs `jsonschema`) |
| `check_tests.py` | `tools/check.py` itself, against fixtures in `build/check_fixture`; every built test executable registered; `render_bench --evaluate` on fixture results (pending budgets reported, a gated miss named by pass, noisy runs never a pass) |
| `test_golden_diff.py` | the golden harness's diff report on synthetic images: statistics, regions, heatmap, crop, `--json` |
| `check_doctor.py` | `ohao doctor` on fake shader trees (missing, stale, includes, Slang entry points and imports) and on the real tree; `--gpu` adds the device checks |
| `check_layout.py` | `shaders/shared/layout.json`'s generated files current, std430 offsets asserted, bad layouts refused, the generated GLSL and Slang compiling |
| `check_level_assets.py` | the checked-in levels (`assets/levels/`): every node's `extras` against the hint schema (`tests/level/extras_schema.json`), the blockout's nodes and hints (the contract the level pipeline's later tests assert), its Blender rebuild byte for byte (`skip` without Blender 5.2.2) |
| `check_serve.py` | `ohao serve` as a process: protocol goldens (`tests/serve/sessions` → `expected`, `--update`; `loadMs` and `buildMs` compare as 0), every tool schema through `jsonschema`, `tools/list` at most 24 KB, stdout clean under stray prints, the sandbox and caps, `ohao serve --pipe` as a process |

GPU suites and harnesses (each creates a Vulkan device):

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/ecs_tests.exe > /dev/null   # gpu
$ build/Release/renderer_test.exe > /dev/null   # gpu
$ build/Release/instantiate_gpu_test.exe > /dev/null   # gpu
$ build/Release/scene_view_gpu_test.exe > /dev/null   # gpu
$ build/Release/restore_render_gpu_test.exe > /dev/null   # gpu
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ build/Release/cull_parity_gpu_test.exe > /dev/null   # gpu
$ build/Release/light_cluster_gpu_test.exe > /dev/null   # gpu
$ build/Release/light_emitter_gpu_test.exe > /dev/null   # gpu
$ python tests/cli/check_cli.py > /dev/null   # gpu
$ python tests/validation/check_validation.py > /dev/null   # gpu
$ python tests/doctor/check_doctor.py --gpu > /dev/null   # gpu
$ OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200 > /dev/null   # gpu
$ python tests/serve/check_live.py > /dev/null   # gpu
$ python tests/serve/e2e_mcp.py > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ build/Release/scene_bench.exe --check tests/bench/scene_budgets.json > /dev/null   # slow
$ build/Release/serve_bench.exe --check tests/bench/serve_budgets.json > /dev/null   # slow
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json > /dev/null   # slow
$ build/Release/level_bench.exe --check tests/bench/level_budgets.json > /dev/null   # slow
$ build/Release/physics_bench.exe --check tests/bench/physics_budgets.json > /dev/null   # slow
$ python tests/docs/check_docs.py --cpu-only > /dev/null   # slow
```

- `engine_tests`: event bus, command history, assets, headless deferred and path-traced
  frames, RenderScene, per-pass GPU timing (every pass registered, timed once, in frame
  order, within the frame; a pass turned off leaves the timings), the capture path (a reset
  idles nothing and a capture after it is the same from any frame slot; the GPU preview is
  the CPU box filter's bytes and is all a preview request copies), the render settings
  (`sp2b_t14_*`: a fresh renderer starts at the defaults and reads back what was set; the
  SSR, SSS, SSAO, RT GI and RT shadow switches drop their passes; GI resolution and rays
  reach the trace; CSM cascades and size rebuild the map), the HUD (`sp7_hud_*`: a capture
  with a HUD list differs from one without only inside the listed rectangles, and an empty
  list leaves the frame byte for byte). `ecs_tests`: world, hierarchy, transforms and the render extract
  against a real RenderScene. The `*_gpu_test`s: instantiate, scene view and
  snapshot/restore through the renderer; `deferred_determinism_gpu_test`: deferred frames,
  post effects included, bit-identical across fresh renderers, processes and repeated
  captures, and path-traced Cornell (offline and realtime) the same within the BVH floor
  (below). `cull_parity_gpu_test`: GPU culling on and off render the same image (a built scene
  with occluded and off-screen boxes, Cornell, the helmet room), two runs build byte-identical
  lists, occluded and off-screen draws are in no list, face culling changes no pixel of
  closed meshes (test 6); the meshlet path through task and mesh shaders and through their
  compute fallback render the same image as each other and as the instance path (the
  occlusion scene, a one-sided sphere whose back-facing meshlets the cones drop, Cornell, the
  helmet room), and the fallback's lists are byte-identical run to run (test 7).
  `light_cluster_gpu_test` (test 8): the froxel lists light every pixel exactly as summing
  every light does (PNGs byte for byte: Cornell's 12 lights, the helmet room with post, the
  bench scenes' 1,000 point lights and sun, a wall 1,500 m out under a 3,000 m far plane, an
  orthographic camera); 1,000 lights packed together overflow and `render.stats` reports
  `lightsOverflow`; twin runs build identical lists; casters past the 8 shadow channels
  render unshadowed (`--keep` leaves the compared PNGs in `build/`).
  `light_emitter_gpu_test`: a point, a spot and an area light get one emitter mesh each, a
  sun none; with the lights out of view the image with emitters is the image without them
  (deferred with RT shadows and GI, deferred with CSM, path traced: byte for byte), with
  them in view each emitter is bright and every other pixel the same (deferred: 1 LSB within
  24 px of an emitter, its denoisers' neighbourhoods); a moved light's emitter carries last
  frame's placement as its prevWorld, and under TAA a moving point light's mesh is as bright
  as the light held still. `check_live.py`: `ohao serve --window` (a hidden window, validation on) driven
  end to end: load, apply, step with watch, inject, captures (inline JPEG and PNG
  previews, a region, another size refused), log.read; a lockstep window capture equals a
  headless capture of the same world bit for bit (post off and on, and path-traced:
  `rt_offline` and `rt_realtime`, test 11); a long path-traced capture reports progress and
  is cancelled with the host serving on; a `--mode rt_offline --spp 24` window presents 24
  accumulating frames, stops, and restarts after a change; a realtime `--mode rt_realtime`
  window's capture is its next frame; minimized is `E_STATE`; closing stdin mid-step exits 0; validation and unrequested
  readbacks 0 through teardown; `helmet_game.scene.json` saves byte for byte and its fly
  camera moves 3 m in 60 injected steps; the window host over `--pipe` (a step, a capture,
  the client's disconnect ends it cleanly); the play scene's player walks, turns by the mouse
  and jumps on injected input, and the same walk headless reaches the same place,
  `world.hash` and picture from its eyes, bit for bit (sp6 test 14); a fight (sp7 test 18):
  EnemyA shot down (`combat.info` for where, `player.teleport` to face, bursts of `fire`), the
  player killed by `combat.damage`, the restart 3 s later, the room cleared and won, and the same
  fight headless reaching the same `world.hash`, `combat.info`, events and HUD picture. `e2e_mcp.py`: a
  stdlib-only MCP client runs a whole session against a hidden window (load, edit, snapshot,
  record, a flight, step with watch, captures, logs, restore, replay to the same hash, a
  capture bit-identical to the first; the blockout's level and physics; the play scene walked,
  `player_info`, `player_teleport`, `physics_contacts`, restored and walked again to the same
  hash and picture; the round fought: `combat_info`, EnemyA shot down, `combat_events`, a look
  with the HUD, `combat_damage` killing the player, the restart, the room cleared and won),
  validation clean through teardown.
- **Validation** (`OHAO_VALIDATION=1`): zero messages, teardown included. Each message
  is a record on the logger's `validation` channel, on stderr as
  `W validation: <VUID> | <text>` (`E` for errors; a run of identical ones ends with
  `... repeated N more times`). `check_validation.py` renders Cornell and the helmet,
  deferred and path traced (offline, realtime, realtime through NRD), under the layers;
  `--json FILE` gives each scene's message count, first VUID and per-VUID counts.
  `OHAO_VALIDATION=sync` adds the layer's synchronization validation (hazards between
  commands, as `SYNC-HAZARD-*` messages): slower, for chasing a missing barrier.
- **Render goldens** (`tests/golden/manifest.json`, 27 scenes: 8 examples, 19 through
  `ohao run`; seven at the deferred frame's `reference` quality; eight of them the Khronos
  validation pack, `assets/khronos/README.md`; two an area light, `area_light.scene.json`;
  one the level pipeline's blockout, `blockout_room.scene.json`; two the play scene through
  the player's head, `blockout_play.scene.json`, one of them with the HUD after 45 steps of the
  fight, `cli_blockout_play_hud`): `--only NAME`, `--update` after a deliberate change (look at what drifted
  first). `--json FILE` writes one object per scene (statistics, tolerance, up to 4
  regions where the image changed); a failing scene also gets
  `build/golden_diff/<name>.heat.png` (where and how much) and `<name>.crop.png`
  (expected | actual | heat of the worst region), `--heatmaps all` for every scene.
  `--selftest --json` records the run-to-run floor. They are a hash of an unchanged sampler, not an image-quality gate: anything
  that perturbs the sample sequence moves every pixel by the noise. Never widen a
  tolerance to make one pass. The deferred scenes are exact (max_abs 0): any difference
  there is a change. Path-traced scenes hold to a floor of 1 LSB on a few pixels in a
  minority of runs (tolerance 1 / 0.0005): the driver builds a mesh's BVH differently run
  to run, and a ray that hits exactly on a shared triangle edge takes whichever triangle
  the traversal reaches first.
- **Benchmarks** (`scene_bench --check`): budgets in `tests/bench/scene_budgets.json`;
  results in `build/scene_bench.json`. `serve_bench --check`: the protocol's budgets in
  `tests/bench/serve_budgets.json` (`ohao serve` as a child process on pipes, bench10k:
  round trips, transport overhead, apply, step, hash, diff, the logger's per-call costs, the
  `tools/list` size; then `ohao serve --window 1280x720 --hidden` on Cornell: a step until
  its frame is presented, a frame's readback, inline and PNG captures, an inline `rt_offline`
  capture at 16 spp (150 ms, Frank's ruling), the reader thread's CPU over 600 idle realtime steps; `--cpu-only` skips the window), results in
  `build/serve_bench.json`. `render_bench --check`: the renderer's GPU budgets in
  `tests/bench/render_budgets.json`, each tagged `gatedFrom` the 2b step that delivers it
  (gated once `currentStep` reaches it, else reported `pending` with its measurement);
  headless captures of `cornell_720p`, `helmet_game_720p`/`_1080p`,
  `bench10k_1080p`/`bench100k_1080p` and `play_hud_1080p` (the play scene with its HUD: the
  `hud` pass, 0.05 ms), 5 warm-up and 100 timed frames each, every pass's median
  and p95 from GPU timestamps, and `meshlet_build_1m` (the meshlet builder on a million
  triangles, CPU, median of 9), results in `build/render_bench.json` (the per-pass baseline
  every renderer change is judged by); `--only SCENE`, `--frames N`, `--geometry
  instances|auto|meshlets` and `--fallback` (the meshlet path without mesh shaders) to compare
  the geometry paths; `--evaluate RESULTS --check BUDGETS` judges recorded
  results without a GPU. Besides the CPU loop it times a GPU calibration kernel
  (`shaders/bench/gpu_calibrate.slang`) before and after, against
  `build/render_bench_reference.json` (`render_bench --calibrate`). `serve_bench` times a
  calibration loop before and after its blocks and records every other process that
  used CPU meanwhile; a run >25% slower than `build/serve_bench_reference.json` (written
  by `serve_bench --calibrate` on a quiet machine) or beside other processes using more
  than half a core in total is `noisy`, exit 2: it is neither a pass nor a regression,
  and `tools/check.py` reruns it, at most twice. `scene_bench` records the same, plus a
  CPU and a memory probe per round, and is also noisy when over a quarter of its rounds
  are contended (a probe over 1.5x its fastest round); it prints each op's median in
  clean and in contended rounds. This laptop slows a busy core by itself after about 3 s
  of sustained load (`scene_bench --machine-test` shows it), more with other bots' work
  running. `python tools/bench/wait_quiet.py` waits until other processes use under half a
  core for a minute (exit 1 at `--deadline`); run it before a bench whose verdict you need.
  `physics_bench --check`: the physics step's budgets in `tests/bench/physics_budgets.json`
  (the blockout's step while crates fall, its step at rest with zero writes, 1,000 contact
  begins in one step, a walking player's part of the step, `player_step_ms`, a step while
  1,000 boxes pile into a pit, `boxes1k_step_ms`; reported without a budget: what a snapshot
  of that world carries, `boxes1k_snapshot_ms`; and combat's, gated by `combatStep`: the
  combat system's part of a step in which the player fires a round in the blockout,
  `combat_shot_ms`, with two enemies attacking, `combat_step_ms`, and a whole step with 16
  enemies chasing, `enemies16_step_ms`; restoring the play scene's round start, `round_restart_ms`),
  CPU, results in `build/physics_bench.json`; the same noisy-run rules.
  A budget is met after five consecutive passes and changes only by Frank's ruling.
- **Docs**: `check_docs.py` runs every command in this file, ARCHITECTURE.md, the CLI
  and serve guides and the skills (`build`, `verify`, `add-component`: `.claude/skills/`, or
  `docs/agents/skills/` until they are installed there; see its README).

## Gotchas that have cost this project time

- conda's `cmake` shadows the real one; use the scoop shim (above). `tools/check.py`
  refuses conda's.
- Tests run in parallel under CTest: a test that writes a fixed path (a scratch
  directory under `build/`, a file in `%TEMP%`) races another copy of itself. Give it a
  per-process name or a `LOCKS` entry in its registration.
- **Git Bash rewrites arguments that start with `/`** into Windows paths. Write
  `ohao query --fields Transform/position`, not `/Transform/position`.
- **Line endings:** `core.autocrlf` is on. Byte-compared files (goldens, scene files,
  CLI expectations) are pinned to LF in `.gitattributes`; add a pattern there for any new
  byte-compared directory, or a fresh clone checks out CRLF and the comparison fails.
- `/FORCE:MULTIPLE` hides duplicate symbols: stb, and a second VMA inside `NRI_VK.lib`.
  Which copy wins depends on link order, so `gpu_allocator.cpp` uses NRI's VMA
  configuration; never change one without the other. A new target that crashes right
  after "resizing to WxH" has picked a mismatched VMA.
- Windows' `OPAQUE` macro (`wingdi.h`) collides with `MaterialData::AlphaMode::OPAQUE`:
  `#undef OPAQUE`.
- GLSL `flat` is a reserved qualifier; rename locals called `flat`.
- `GLM_ENABLE_EXPERIMENTAL` before any `<glm/gtx/...>` include.
- Jolt: `dynamic_cast` on Jolt types crashes (no RTTI); `SetMotionQuality` lives on
  `BodyInterface`. `PhysicsWorld(cfg)`, `stepOnce()` not `step(dt)`; bodies need a shape.
- `ohao::physics::math::AABB` is (center, halfExtents); render bounds (`MeshInfo`,
  `DrawRecord`) are (min, max). A box passed between them needs converting.
- ECS writes: `Transform`, `MeshRenderer`, `Light`, `EmissiveProxyLight` and
  `GiMaterialLegacy` reach the renderer only through `registry.patch<T>()` /
  `replace<T>()` (or emplace/remove). A write through `get<T>()` is never seen.
- World systems have a phase: `addSimulate` (behaviour, once per fixed step, gets dt) or
  `addSync` (derived state, no dt). Before a `render()` call `world.sync()`, never
  `update()`: rendering must not step the simulation.
- Input is stepped per simulation step, not per frame: read `pressed`/`released` in a
  simulate system or `fixedUpdate`, never once per frame (two steps in one frame would
  lose a press). A world with an `InputActions` component rebuilds its bindings from it;
  one without keeps the bindings made in code.
- Descriptor sets written at record time need one set per frame in flight
  (`MAX_FRAMES_IN_FLIGHT` = 3), or a frame rewrites the set the previous one reads (VUID 03047).
- A window presents path-traced frames too: `renderRTPipeline` acquires, blits the tracer's
  RGBA8 output (`pt.present`, which follows `pt.render` because the pass registry is in
  frame order) and presents like the deferred path. What a window presents is a launch
  option (`ohao serve --window --mode`, `ohao_game --mode`); a lockstep capture in another
  mode switches, renders, then restores the live mode, denoiser, quality and post. The path
  tracer does not notice a moved camera itself: a host calls `notifyCameraChanged()`
  (`LiveHost` does whenever the world changed).
- Readback is on request only: `requestReadback()` before the `render()` you want,
  `readback()` after (that frame's fence only; a path-traced frame under OIDN or NRD is
  denoised there), or for an inline preview of a deferred frame
  `requestPreviewReadback({rect, maxSide})` and `readbackPreview()`: the `capture.preview`
  pass box-filters the final image straight into the staging buffer and nothing else is
  copied. No other frame copies anything, so a read after an unrequested frame is empty.
  Buffers the CPU reads must be host-cached memory: uncached host-visible memory read one
  1280x720 frame in ~20 ms. A shader writing such a buffer over PCIe wants a warp's writes
  contiguous (`capture.preview` in 8x8 groups took 0.49 ms, in rows of 64 0.056 ms).
- G-buffer and CSM draws are GPU-culled indirect draws (`GpuCulling`): firstInstance is the
  draw's index in `RenderScene::drawTable()` (`DrawRecord`), not the instance slot; a vertex
  shader reads `draws[gl_InstanceIndex].draw.w` for the slot. A draw the culling never lists
  is never drawn, so a culling bug hides geometry: `cull_parity_gpu_test` compares culling on
  and off exactly. A mip chain halves rounding down: a pyramid whose levels assume rounding
  up reads past them (the Hi-Z is padded to powers of two for that reason).
- A G-buffer phase has four lists: instance one-sided, instance double-sided, meshlet
  one-sided, meshlet double-sided (`kCullListEarly`/`kCullListLate` + 0..3; a cascade has one,
  `kCullListCascade0 + c`, every draw instanced). A meshlet list's command is a task dispatch
  whose fourth word is the draw's index (the task shader reads it through `SV_DrawIndex`, which
  needs `shaderDrawParameters`); the fallback's per-meshlet draws carry the draw in firstVertex
  (`<< kMeshletVertexShift`) and the meshlet in firstInstance. `geometry: auto` sends draws of
  `kMeshletMinTriangles` (4,096) or more to meshlets: below that the meshlet path measured
  slower. Its images must equal the instance path's exactly: `cull/meshlets.slang` repeats
  `gbuffer.vert`'s operations (`MatrixInverse` through `spirv_asm`; Slang has no `inverse`).
  Wireframe mode applies to instance draws only.
- `resetAccumulation()` never waits for the GPU: the next frame's passes start without
  history and clear what they keep in their own command buffer, after the frames in flight
  by queue order. Nothing a frame renders may depend on its frame-in-flight slot (TAA's
  jitter is indexed by the frames since the reset); `engine_tests`' `sp2b_s10` captures
  from every slot to hold it.
- Render settings (`VulkanRenderer::RenderSettings`, `render.settings`) are read back from
  the subsystems that own them, never cached, and both structs' defaults (the api one in
  `ohao/api/params.hpp` too) must equal what a fresh renderer starts with: a headless host
  reports them before its renderer exists and captures pass them explicitly (exposure 0.6:
  a 1.0 there once made headless captures brighter than the window's). A GI resolution or
  CSM size change rebuilds images after `vkDeviceWaitIdle`; nothing else idles. The GI
  shift (`flags.w`, 1 at half resolution) keeps the half-resolution expressions literally
  behind a branch: folding it into a multiply changed FMA contraction and moved goldens.
- Ambient occlusion comes only from glTF's `occlusionTexture` (`MaterialDesc::occlusionTex`,
  material row 2, w), never from a metallicRoughness map's R channel, which glTF leaves
  unused (reading it blacked out MetalRoughSpheres and had kept DamagedHelmet out of all
  ambient light). A packed ORM image is uploaded once and serves both.
- The deferred frame's environment light is split-sum IBL from `EnvPrefilter`'s images
  (`render/deferred/env_prefilter.*`, built at environment upload: GGX-prefiltered into
  `kEnvPrefilterLevels` mips read at `roughness * (levels - 1)`, and irradiance), times the
  environment's intensity. RT GI's output is raw incoming radiance (no albedo) with the
  environment's visibility (escaped rays) in alpha: specular weights the prefiltered
  environment by it and takes GI's radiance for the rest, so a closed room does not
  mirror the sky; AO and SSAO occlude specular through `specularOcclusion`. The path
  tracer's environment keeps one mip and is untouched. RT GI's half-resolution noise
  shows as low-frequency blotches on flat walls; compare a metal against the path tracer
  before trusting its look.
- Reflected light is counted once. Lighting writes, beside the lit image, the
  environment/GI specular it added (`envSpecular`, E) and its weight (`specularWeight`,
  W: split sum times specular occlusion, computed for every pixel, environment or not);
  SSR's resolve (`postprocess/ssr_resolve.slang`) rewrites the lit image in place,
  `lit += W·(c·L) − c·E`, before bloom and TAA. A new reflection source must go through
  E/W, never be added on top in post.
- Indirect diffuse light has one source per frame: RT GI's rays, else the environment's
  irradiance, else (neither: no `VK_KHR_ray_query` or `rtGI` off, and no environment) the
  constant ambient (`ambientIntensity`, 0.35 with scene lights; `setAmbientIntensity`
  overrides it). With RT GI the constant does nothing, so walls are lit by direct light
  and one bounce, darker than the multi-bounce path tracer, and RT GI's denoiser noise
  shows on them. A test that wants ambient light must turn RT GI off. All three are
  diffuse only (`albedo·(1−metallic)`).
- Emission is the G-buffer's sixth attachment (`GBufferPass::kEmissive`, RGBA16F, the
  fragment shader's output 4; depth stays attachment 4): emissive texture × emissive
  strength, the same radiance `pt_closesthit` emits, added by lighting unlit. The normal
  target's alpha keeps only its luminance, to keep emitters out of SSS. A pipeline drawn
  in the G-buffer render pass needs `GBUFFER_COUNT - 1` blend states.
- Point and spot lights mean the same in both renderers (Frank's ruling, 2026-10-26): a
  light of intensity `I` lights a surface `d` away with `I/d²`, windowed to exactly 0 at
  its `range` (default 10 m) by `(1-(d/range)⁴)²`. The path tracer's sphere emitter has
  radiance `I/(πr²)` (`includes/rt/pt_lights.glsl`); albedo is base colour × albedo texture
  in both, never the vertex colour (primitives carry their colour as both). An area light
  is the same rectangle in both (Frank's ruling): uniform radiance `I/(4πr²)`, one-sided
  toward `edge1 × edge2`, no range and no `1/d²`; deferred lights it as light type 3
  (`evaluateBRDFRect`: the exact polygon irradiance, the representative point for specular),
  clustered lighting lists it in every froxel, RT GI's bounce sees a cosine-profile point at
  its centre. What stays between the renderers: one GI bounce against many, Burley against
  Lambert and the roughness-limited Fresnel at grazing angles
  (`docs/changes/2026-10-26-visible-lights.md`, `2026-10-26-area-light-align.md`).
- Every point, spot and area light is also a mesh the camera sees (`RenderScene::setLights`
  makes an emitter instance per light: a sphere of its radius, or its rectangle, with the
  light's radiance, `kInstanceEmitter`). It is pure emission and changes no other pixel: its
  TLAS mask (`kTlasMaskEmitter`) is in only the path tracer's camera ray's mask
  (`kRayMaskCamera`) and a near-mirror first hit's specular ray's (`ptMirrorRoughness`, 0.05:
  that hit's light samples drop their GGX term, so the reflection counts once), so no shadow,
  GI, bounce or other specular ray hits it (else the light would shadow itself and be
  counted twice); no CSM cascade draws it (`kDrawEmitter`); the
  G-buffer marks it with emissive alpha 1 and metallic −1, so lighting outputs its radiance
  alone, SSR treats it as a miss and RT shadows and GI skip it as sky. A new trace passes
  `kRayMaskScene`; a new screen-space pass that reads the lit image must decide what an
  emitter pixel is to it. `setLightEmitters(false)` turns the meshes off (the lights stay).
  A mesh moves with its light and writes motion vectors as any mesh does: the extract passes
  the light entity's `prevWorld * inverse(world)` as `LightDesc::lastFrameMotion`, and the
  emitter's prevWorld is that times its world (identity, so prevWorld = world, when still).
  Mirrors reflect it: in deferred a point or spot light's specular is its sphere's
  (`LightGpu.params.z` = the mesh's radius, `evaluateBRDFSphere`: the representative point,
  the lobe widened to `a + r/(2d)`), on screen or off, SSR or not; SSR treats an emitter pixel
  as a miss, the analytic reflection stands in. A spot's mesh shines toward the camera by
  its cone (`RenderScene::setViewerPosition`, each frame): dark from outside it, in both
  renderers; its reflection uses the same per-frame value in the path tracer.
  Bloom's input is clamped at luminance 4 before exposure (`BloomPass::kBloomMaxLuminance`,
  `bloom_threshold.frag`): a mesh at radiance ~19,000 otherwise glared over half the frame
  (the Karis average renormalises per tap group, so it cannot stop a bright area). Lighting
  never reads bloom; tone glare there, never the mesh's radiance.
- A deferred capture renders 1 frame by default (10 with TAA), but the hybrid's RT GI
  keeps a history: it traces half resolution at 2 rays a pixel and accumulates (up to 32
  frames), so frame 1 and frame 10 of Cornell differ (max_abs 10 at 1920x1080). Pass
  `frames` for a converged image; the deferred goldens state theirs (`--frames 10`,
  `--frames 30`). Without `VK_KHR_ray_query` there is no RT GI. With it, lighting drops its
  diffuse IBL term and the constant ambient: GI rays that leave the scene bring the
  environment back, occluded.
- RT shadows are compute with inline ray queries (`VK_KHR_ray_query`; without it the
  frame keeps CSM, which renders only when RT shadows do not). They keep a history too: the
  filter averages the frames since the last reset, so frame 1 is the noisiest. The first 8
  shadow-casting raster lights own a visibility channel each (`params.w`); casters past them
  render unshadowed (`RenderScene::shadowCastersWithoutChannel()`). A pixel traces only its 4
  most important; `render.stats`' `shadowLightsDropped` counts the pixels that had more.
- Lighting is clustered (`lights.cluster`, `LightClusters`): raster lights are uncapped (up
  to 65,536) in a `LightGpu` storage buffer per frame slot, and each 16x9x24 froxel lists up
  to 256 of them. The lists equal summing every light only because a light's attenuation is
  exactly 0 past its range and the loop skips exact zeros: keep both, and keep the lists in
  light order, or `light_cluster_gpu_test` fails. A froxel past 256 drops lights and
  `render.stats` reports `lightsOverflow`. The slices run to the camera's far plane; the
  sky is only the cleared depth (1.0), so geometry at any distance keeps its pixel.
- The validation layer stops at 10 reports per VUID; set
  `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` to see them all. The engine names no Vulkan
  objects: find a handle's creation site by temporarily printing handles at `vkCreate*`.
- Restoring a file from a backup copy keeps the backup's older mtime, so MSBuild keeps
  the objects built from the edited version: `touch` sources after a restore.
- **Levels and staged assets** (sub-project 5,
  `docs/specs/2026-10-26-level-pipeline-design.md`).
  - Checked-in levels are generated by Blender scripts in `tools/level/` and are ours (MIT).
  - Frank's Somnabulate assets are staged by `tools/level/stage_flat42.py` into git-ignored
    `assets/local/`, with the scenes that use them, and are never committed until he rules
    (the repo is MIT). Never write under `C:\Users\djmax\Desktop\games` or the Unreal
    project.
  - A `ModelAsset` without `mesh` merges every mesh in a file, collision hulls included;
    with `mesh` (a glTF mesh name or `#<index>`) it is that one mesh at its own origin,
    loaded by `ModelLoader::loadMesh` and cached per `(path, mesh)`. A glTF's node tree,
    `extras`, lights and cameras are read by `ohao/level`'s `readGltfStructure`, not Assimp.
  - A `Level` component is a level file in the world: the `level` sync system
    (`ohao_level_world`, `level/level_system.hpp`, before `instantiate`) makes one entity per
    node with `LevelNode`, ids `levelNodeId(level id, node index)`, and `MeshSource`
    `#<mesh index>`, `Light`, `Camera`, `SpawnPoint`, `Socket` and passed-through components
    from the node and its `extras` (`level/level_hints.hpp`; `vis: 0` and `UCX_` hulls get no
    mesh). A host must call `level::installLevels` (the CLI context and `LiveHost` do);
    `scene.load` expands levels at once (`World::runSync("level")`). Node entities are never
    saved, and a changed `Level` re-expands in place (same entities, the file's state back),
    so an unchanged file keeps `world.hash`.
  - Collision (`ecs/physics_scene.hpp`): `Collider` and `RigidBody` are saved components; the
    `physics` sync system (right after `transform`, installed by `ecs::installPhysics`, which
    the CLI context and `LiveHost` call next to `installLevels`) builds one Jolt body per
    entity with a `Collider`, fitted to its mesh at its world scale, plus `UCX_` hulls
    (`ColliderPart`) as one compound body on their target. It works from dirty state (signals
    and the transform system's `PhysicsMoved` tag), so a still world builds nothing; a moved
    body is moved, a rescaled one rebuilt (stepping: "Physics in the step", below). The
    backend runs without worker threads and room for 65,536 bodies; Jolt
    takes each step's contact buffer (sized by that cap) from the temp allocator, which falls
    back to the heap past 10 MB (a fixed 10 MB block aborted the process on the first step).
  - Agents read levels through `level.info`, `level.reload`, `physics.raycast`,
    `physics.info`, `physics.events`, `physics.contacts`, `player.info` and `player.teleport`
    (`ohao/api/level.cpp`), which reach the level system and the physics
    scene through what `installLevels` and `installPhysics` leave on the World
    (`level::levelSummary`/`levelDiagnostics`/`reloadLevel`, `ecs::physicsScene`); a World
    without them answers `E_STATE`. They run the `level`, `transform` and `physics` sync
    systems first (never instantiate or the render extract), so they answer right after
    `scene.load`. Recipe: look at a level file with `ohao level <file>`, in a session with
    `level_info` and `physics_raycast` (`docs/agents/ohao-serve.md`, "Load a level"); add
    one by `docs/ARCHITECTURE.md`, "add a level". Results stay small: `level.info` on the
    blockout ~480 bytes (budget 1 KB, `level_bench`), a ray ~100 (200).
  - Loaders keep a file's normals. `repairMeshNormals` fills only vertices left without
    one (zero or not finite) with their faces' smooth normal; a loader that finds no normal
    writes zero for it, never a placeholder direction.
- **Physics in the step** (sub-project 6, `docs/specs/2026-10-26-player-physics-design.md`).
  `installPhysics` also adds a `physics` simulate system: every `World::simulate(dt)` (the
  window's fixed step, `world.step`, `ohao run --steps`) propagates dirty transforms
  (`propagateTransforms`), brings bodies up to date, steps Jolt in substeps of at most 1/60 s
  (8 at most), writes every awake dynamic body back to its entity's `Transform`
  (`registry.patch`, through the parent), and publishes the step's events. A world without a
  collider does none of it. There is one clock: never step the backend from a host.
  - `propagateTransforms` is the transform system without the frame's bookkeeping: an
    entity's `prevWorld` is captured at its first recompute after a render and kept until the
    next `updateTransforms`, so a frame of several steps still has motion vectors spanning them
    (`t10_two_steps_one_frame_motion`). Do not call `updateTransforms` inside a step.
  - A dynamic body's entity moved by anything but the write-back (an edit, a moved parent)
    puts the body there at rest, however close: the scene marks its own `Transform` writes
    (`writePose`) and treats any other as a placement; through a parent, a pose more than 1e-4
    from what it wrote. Teleports set position and rotation at once (`setPose`). Sleeping
    bodies are not written, so a settled level writes nothing.
  - Snapshots replay exactly (step 5). A snapshot carries the simulation: `installPhysics`
    puts `PhysicsScene::saveSimulation`/`restoreSimulation` in the World's `SnapshotHook`
    (`ecs/snapshot_hook.hpp`): Jolt's state (bodies with velocities and sleep timers, the
    contact cache the solver warm-starts from), each character, the players' records, the
    contact bookkeeping. The next sync after a restore puts it back if the bodies are the
    snapshot's (same handles for the same entities) and skips their teleports; a body rebuilt
    since falls back to teleports and `BodyMotion` (runtime, snapshotted, never saved or
    hashed: what each step left a dynamic body or player moving at). Velocities alone are not
    enough: resting contacts replayed 2-4e-5 m off without the contact cache. `scene.load`
    runs `player_spawn`, so a snapshot after a load holds the spawned player. A new or
    teleported character finds its ground at once (`RefreshContacts`).
  - Events (`ecs::physicsEvents(world)`, the latest step's; `PhysicsScene::eventsSince` and
    `physics.events`, a log of 1,024): `contact_begin`/`contact_end` per body pair (Jolt's
    per-sub-shape callbacks counted per pair), `trigger_enter`/`trigger_exit` for a
    `Collider.trigger` sensor (hint `"trig": 1`). Jolt's callbacks only queue (thread-safe);
    the scene drains them on the simulating thread after the step and sorts them. Jolt removes
    a sleeping body's contacts: the scene keeps such a pair (dormant) and ends it only if the
    bodies wake apart. Removing a body ends its pairs in the next step's events.
  - Jolt's penetration slop is 5 mm (its 2 cm default left dropped crates visibly in the
    floor): a resting body sits up to 5 mm into each support.
  - The player (`PlayerController`, saved; `PlayerState`, runtime): a Jolt `CharacterVirtual`
    whose position is the feet, moved in the step before Jolt's substeps by the actions it
    names (`move_forward`, `move_right`, `jump`, `sprint`, `look_x`, `look_y`; mouse look
    always on). Its kinematic inner body (layer `CHARACTER`, never asleep) is what dynamic
    bodies and sensors touch: `CHARACTER` does not collide with `DYNAMIC` or itself (the
    virtual character pushes crates by `pushForce`), and a trigger sees the inner body as it
    sees any kinematic body. Vertical motion is integrated exactly (`vy + g·dt/2` per step),
    so a jump of `jumpSpeed` 4.9 peaks at 1.22 m. It spawns once, when created, at the first
    `SpawnPoint` tagged `player` (by creation order; `W_PLAYER_NO_SPAWN` else); a write to
    its `Transform` by anything but the step teleports it. It must be a root entity. A
    kinematic body moved during a step moves by `moveKinematic` (velocity, so it pushes what
    it meets) and stops after the step; moved outside a step it teleports.
  - The camera: the player's head is its first child holding a `Camera` (the scene authors
    it, `assets/scenes/blockout_play.scene.json`); the step writes it at `eyeHeight`, pitched
    within ±89°. Players spawn in the `player_spawn` sync system (before `transform`), so a
    play scene's first frame, before any step, is the spawn's. The view is the first *active*
    camera (a level's own camera stays inactive beside the head).
  - Render interpolation: `World::setRenderAlpha` (default 1; `LiveHost` sets the timeline's
    alpha in realtime, 1 in lockstep and for lockstep captures). The `interpolate` sync system
    (after `physics`, before `render_extract`) gives what the last step moved (players, heads,
    dynamic bodies) and their subtrees a runtime `RenderPose` between the last two step poses,
    which the render extract and the scene view's camera use; `Transform`, `WorldTransform`,
    `world.hash` and saved files stay the step's. A placement (spawn, teleport, edit) is never
    interpolated across. A capture syncs before it resolves the view.
  - The window reports look input only from a captured cursor, as `MouseDelta`;
    `ohao_game` on a scene with a `PlayerController` captures the mouse while focused,
    `release_mouse` (Esc) frees it, and a click on the window takes it back
    (`LiveHost::setClickToCapture`, `runtime/click_capture.hpp`): that press and its release
    are swallowed before the input map sees them, so the capturing click never fires.
  - Footsteps: the player's step logs a `footstep` event every 0.75 m walked (0.95 m
    sprinting) on the ground and a `land` after a fall of more than 0.3 m, in the physics
    event log (`physics.events`), with the ground's `Collider.surface` (hint `surf`; empty is
    `default`). Only a window host with `LiveConfig::footsteps` (ohao_game; `--mute` off) plays
    them (`runtime/footsteps.hpp`): `<surface>_<n>.wav` from git-ignored
    `assets/local/audio/footsteps/` (Somnabulate's, staged for local listening only, never
    committed) else `assets/audio/footsteps/` (ours, `tools/audio/make_footsteps.py`).
- **Combat** (sub-project 7, `docs/specs/2026-10-27-combat-design.md`). `ecs::installCombat`
  (the CLI context and `LiveHost` call it after `installPhysics`) adds a `combat` simulate system
  after `physics`: every `Weapon`, in id order, reads its trigger (a player's from the World's
  input, `fire`/`reload`; anything else's from `WeaponTrigger`), advances cooldown, reload, bloom
  and recoil, and fires at most one round a step. A round is one ray from the eye (a character:
  feet + `eyeHeight` along this step's yaw and pitch; anything else: its origin along −Z) through
  `PhysicsScene::raycastFrom`, which skips the shooter's own bodies and triggers (a plain
  `raycast` from the eye stops inside the shooter's capsule). What it hits, or its nearest
  ancestor, with `Health` takes the damage; at 0 it is dead and neither takes damage nor fires.
  - `Health`, `Weapon`, `WeaponState` and `WeaponTrigger` are saved, so files, `world.hash` and
    snapshots hold a fight exactly; the step emplaces `WeaponState` (a full magazine). Spread
    and recoil draw counter-based random numbers, `hash(seed, shots, i)`: no generator state.
  - The cooldown carries its remainder and compares within 1e-5 s, so 10 Hz fires on every
    sixth 60 Hz step. Bloom and the vertical kick recover only once the fire interval passes
    without a round (else sustained fire would never bloom).
  - Recoil turns the view through `PhysicsScene::turnPlayer`: the step's own write of the
    player's yaw and the head's pitch (not a placement), kept inside this step's interpolated
    move. Events (`combatEvents(world)`, the step's; `CombatSystem::eventsSince`, a log of
    1,024): `shot`, `hit`, `damage`, `death`, `reload`, `reloaded`.
  - Saved types hash their names even with no instances, so a new saved component moves every
    `world.hash` in the serve and CLI expectations, not only scenes that use it.
  - Enemies (step 2) are characters on the player's code: `PlayerController` with `spawn`
    (the k-th character with a tag takes the k-th `SpawnPoint` with it, creation order; empty:
    where it stands) and `input: false` (moved by its saved `MoveIntent`, not the world's
    input; weapons fire by input only for `input: true`). The enemy AI (`ecs/enemy_ai.cpp`)
    runs at the start of the combat step, before the weapons: sight is a `raycastFrom` from its
    eye to the player's chest (in its cone only while idle); it writes `EnemyState`,
    `MoveIntent` (the next step walks it) and `WeaponTrigger` (this step fires it), all saved.
    A dead enemy loses its `PlayerController` and is tipped over; its character is parked
    (`IPhysicsBackend::setCharacterInWorld(false)`: out of the broadphase, so it blocks nothing
    and rays pass) and comes back as the same character when a restore puts the controller back,
    so the snapshot's simulation restores exactly (a new character would leave resting crates
    3e-5 m off). Characters block each other: a player teleported into an enemy is pushed
    out. `player.info` and friends without `entity` mean the input-driven character.
  - `scene.load` runs `level`, `player_spawn`, `transform` and `physics` sync, so a snapshot
    right after a load holds the built bodies; a restore of one taken before the bodies existed
    rebuilt them, and a character standing on a slope replayed one float off.
  - The round (step 3): a world with the saved singleton `Round` starts it at the end of its
    first combat step (`round_start`, a snapshot through the `RoundHook` in the registry's
    context, which `api::Session` installs with `ecs::installRoundHook`; a bare World installs
    it itself, else the round warns once and never runs) and restarts at the end of the step
    the player has been dead `restartDelay` (3 s) or `restart` is pressed: `world.hash` after
    that step is the start's. Every enemy dead is `win` once, then the round waits for
    `restart`. A dead player neither walks, turns, jumps nor fires. A character a restore
    creates starts where its restored entity stands, never at a spawn.
  - Presentation (step 4). The muzzle flash is a `Light` with a saved `Muzzle {intensity,
    time}` below the weapon's entity: the combat step sets it to `intensity` for `time` after a
    round (`WeaponState.flash` counts the steps) and back to 0; a light at intensity 0 has no
    emitter mesh (`render::lightEmitter`). Impact marks (`ecs/impact_marks.hpp`) are 64 runtime
    quads made from `hit` events by the `impact_marks` simulate system after `combat` (installed
    by `installCombat`), attached to what was hit by its pose at that step (on a character, to
    the body box the round crossed; none on the player), so a renderer created late (a headless
    host's first capture) draws the marks a window drew; the `impact_marks` sync system after
    `render_extract` (`installImpactMarks`, where a RenderScene exists) only draws them, as
    RenderScene instances, never in the registry, so never saved or hashed; a restart clears
    them. The HUD (`ecs/combat_hud.hpp` → `render::HudList` → the `hud` pass after the gizmo,
    `render/hud/`) is laid out from combat state and the log, read from its oldest event. A window always draws the HUD; `render.capture` and `ohao run --render` only with
    `hud: true`/`--hud`, and only on deferred frames. Sounds (`runtime/combat_sounds.hpp`,
    `assets/audio/weapons/`, ours: `tools/audio/make_weapons.py`) play beside the footsteps,
    under the same `LiveConfig::footsteps`.
  - Agents (step 5) fight through `combat.info` (the round and every combatant: health, `pos`,
    weapon, enemy state), `combat.events` (the log, as `physics.events`) and `combat.damage`
    (hit points off an entity's `Health` or an ancestor's, as a round does), all in
    `ohao/api/combat.cpp`; firing is `input.inject` (`fire`), as in a window. Aim by
    `player.teleport` to where the player stands with the yaw toward a combatant's `pos`.
  - A restore keeps restored rotations bit for bit: a character a restart brings back (a dead
    enemy's, parked) is placed where its restored entity stands without rewriting its rotation
    from the yaw read back (`yawOf` gives +180 for a quaternion stored at -180: the same
    rotation, another hash). Any code that re-derives a pose from a restored one must keep it
    exact, or `world.hash` after a restart stops being the round start's.
- `build/ohao` is the CMake binary directory of `ohao/`, not the CLI; the CLI is
  `build/Release/ohao.exe`.
- After a crash the GPU can stay wedged (device lost `-4` everywhere, 30 s renders of
  Cornell). It clears on reboot; it is not your change. `ohao doctor`'s `gpu.wedged`
  check says so.
- Monograph: run its tests and `check_unit.py` from the repo root, never from `site/`
  (that directory shadows the stdlib `site` module and breaks `_ctypes`). The site build
  is the one exception: it imports `tools.monograph`, so it runs from `site/`.
  `build.py` has no `__main__`, so `python -m tools.monograph.build` does nothing.
  Removing a unit takes three edits: the `.md`, its `page()` entry in
  `site/tools/monograph/tree/<module>.py`, and its `author_rich.py` block, or the stale
  page keeps being served.

```console
$ python site/tools/check_unit.py reflection/api > /dev/null
$ python site/tests/test_monograph_structure.py > /dev/null 2>&1
$ cd site && python -c "from tools.monograph.build import build; build()" > /dev/null   # slow
```

## Conventions

- **Spec → plan → build.** A sub-project gets a design in `docs/specs/`, an
  implementation plan in `docs/plans/`, then a build in the plan's steps, each ending
  with the engine running and every gate green. If the spec turns out wrong, fix the spec
  in the same commit and say so.
- **A change record per sub-project (or build step)** in
  `docs/changes/YYYY-MM-DD-<subproject>[-stepN].md`, written for the project wiki. Shape:
  a title; `**Spec:**` (path and sections) and `**Status:**` lines; then numbered
  sections — summary, what was built, bugs found and fixed, spec corrections, results
  (test counts, benchmark medians/p95, golden max_abs), known gaps, how to run the new
  tests. Facts and numbers only, from runs, never estimates.
- **Commits:** local, one or a few per step, subject `type(scope): what` (`feat`, `fix`,
  `perf`, `docs`, `test`), body saying why. Never push, merge, rebase or reset without
  the owner's say-so.
- **Forward only.** Old code and docs are reference, not something to preserve: delete
  what is dead, and describe the tree as it is, not its history.
- **Engine code logs through `OHAO_LOG(level, channel, fmt, args...)`** (`core/log.hpp`),
  never `std::cout`/`std::cerr`/`printf`; `reflect_tests` fails on a print under `ohao/`.
  Per-item and per-frame detail is `Debug` (shown with `OHAO_LOG_LEVEL=debug`).
- **Performance is a gate.** New hot paths come with a benchmark or a budget; reflection
  never runs per frame (the ECS system sources may not include `reflect/` or `api/`;
  `reflect_tests` enforces it).
- **Tests are named for what they prove** and, where a spec numbers its tests, carry the
  number (`t21_…`). A golden is updated only after looking at what changed.
