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
  flattening the path (`core/gbuffer.vert` → `core_gbuffer.vert.spv`).
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
  default set — and `golden`, `bench`, `slow`, which `--all` adds (`monograph`, the frozen
  site's checks, only by name). `--labels gpu,docs`
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
$ build/Release/audio_schedule_tests.exe > /dev/null
$ build/Release/force_generator_tests.exe > /dev/null
$ build/Release/env_cdf_test.exe > /dev/null
$ build/Release/env_sun_test.exe > /dev/null
$ build/Release/denoise_parse_test.exe > /dev/null
$ build/Release/sobol_test.exe > /dev/null
$ build/Release/level_physics_tests.exe > /dev/null
$ build/Release/combat_tests.exe > /dev/null
$ build/Release/anim_tests.exe > /dev/null
$ build/Release/vfx_tests.exe > /dev/null
$ build/Release/material_tests.exe > /dev/null
$ build/Release/openpbr_tables.exe --check ohao/render/material/openpbr_tables.hpp > /dev/null
$ python tests/cli/check_cli.py --cpu-only > /dev/null
$ python tests/reflect/check_scene_schema.py > /dev/null
$ python tests/check/check_tests.py > /dev/null
$ python tests/golden/test_golden_diff.py > /dev/null
$ python tests/doctor/check_doctor.py > /dev/null
$ python tests/serve/check_serve.py > /dev/null
$ python tests/layout/check_layout.py > /dev/null
$ python tests/level/check_level_assets.py > /dev/null
$ python tools/audio/grenade_spatial.py --out build/grenade_spatial_docs > /dev/null
```

| Suite | Covers |
|---|---|
| `runtime_tests` | the runtime shell without a window: fixed-step clock, the timeline (lockstep, realtime, pause), the input map (actions, axes, the virtual device, key names), swapchain choices (the present mode asked for, else the next one down: immediate, mailbox, fifo), the frame log (`sp9_t19_*`, `sp9_t20_*`: a look movement's latency from its poll to the present of the frame showing it, the stage medians, p95 by linear interpolation, the newest frames only, the ring; frame time percentiles and steps per frame), capture images (the PNG writer round trips through stb_image, crop, the preview downscale, JPEG), `render.stats`' per-pass medians, the click that captures the cursor never reaching the input map; sub-project 9 step 3 (`sp9_t21_*`): combat's particles by event (a muzzle burst at the drawn muzzle along the round, sparks and the surface's dust per hit, nothing when not realtime); the audio player (sub-project 11 step 3, `t29_*`, on a mixer without a device): two steps of one frame start a step apart, the newest a step ahead; a command whose time has passed at once and counted late; paused or lockstep at once plus the horizon, then a new anchor; a step back (a restore) a new anchor; the play scene's bank decoded once, no file opened by 240 steps of fighting |
| `log_tests` | the logger: level filter, ring wrap and `dropped`, repeat collapse, channel filter, paging, concurrent writers, sinks, the 1 KB cut, the stderr format |
| `reflect_tests` | registry, schema export, JSON writer and reader, float formatting, the dependency rule, the source lints (nondeterminism, prints, literal light positions, float atomics in shaders) |
| `scene_tests` | engine registrations, ids, the scene codec, error corpus, migration, snapshots, instantiate (fake backend) |
| `api_tests` | methods, commands and inverses, queries, summaries, params structs, the path sandbox, cancellation, the time model (phases, `watch`, `E_STATE`, realtime vs lockstep), `world.hash` and `world.diff`, input (inject, state, record, replay), `render.stats`; levels and physics (`level.info` counts, spawns and size, `level.reload` keeping the hash, `physics.raycast` hits, misses and refused params, `physics.info`, the warnings); `physics.events` after a drop through `world.apply` and `world.step` (the landings, the cursor, refused params); `player.info` at the spawn and after a walk, `player.teleport` (onto the Step, at rest, refused params), `physics.contacts` (the resting crates, an entity's pairs, `max`), a restore mid-fall and a restored walk reaching the same `world.hash`; combat (`sp7_t18_*`): `combat.info` before and after the first steps (the round, three combatants in id order, under 1 KB), `combat.events` (a burst's shots, `round_start`, the cursor), `combat.damage` (on an enemy and on its body box, the dead taking none, both enemies dead: won, the player dead: the restart 3 s later back at the round start's `world.hash`), refused params, `E_STATE` without combat; audio (sub-project 11 test 29, `t29_*`): `audio.info` headless (`device: "none"`, the snapshot, zone, buses, the fight's commands after a cursor, under 2 KB at `max` 16), `audio.mix` handing a host its overrides (a bus's db, mute, solo, a forced snapshot, `clear`; unknown names refused before anything changes) and never touching `world.hash` or the schedule |
| `rpc_tests` | the protocol server in-process: framing, ordering, the error table, ids, cancellation, progress, ping while busy, MCP initialize/tools/logging, the queue cap; the named pipe (one local client, its DACL, a second client and a second server refused, remote clients refused against a control pipe) |
| `systems_tests` | engine-provided systems (spin, the fly controller) |
| `level_physics_tests` | level collision: colliders fitted from meshes (box, sphere, capsule, convex, mesh), `UCX_` hulls as one compound body, a dynamic mesh made convex, non-uniform and mirrored scale; rays against the blockout (spawns, doorway, wall, ramp), its crates stepped to rest, a render-only level, a still world doing no work; physics in the world's step (test 10): dropped crates land and rest at their authored places, contact begin events at the right step and speed, a settled world writing nothing, an edit teleporting a dynamic body, motion vectors over two steps in one frame, a destroyed body ending its contacts, trigger enter and exit, substeps, two runs identical bit for bit, the event log's cursor; the player (test 11): spawns at the player spawn facing its yaw, walks 4.0 m in 60 steps, stopped by the north wall, climbs the 0.3 m step, the platform and the ramp, pushes a crate, jumps 1.22 m, seen by a trigger; kinematic bodies moved by velocity; the camera (test 12): the head at eyeHeight, pitch clamped at ±89° (and set by an edited head), yaw turning the body, render interpolation drawing the head and a falling crate between steps while `world.hash` and the saved file stay the step's, `blockout_play.scene.json` viewed from the spawn before any step; footsteps (test 13): a footstep every 0.75 m walked and 0.95 m sprinted, the surface of the ground body (the blockout's `surf` hints, an edited `Collider.surface`, `default`), one land after a 0.5 m drop at the free-fall speed and none after 0.2 m, none standing or in the air; replay (test 14): `BodyMotion` following awake bodies (and neither saved nor hashed), restores replaying bit for bit mid-fall, mid-jump, from the loaded spawn and with crates resting on each other, sleeping bodies put back to sleep, a body put back however close it settled, a body rebuilt since the snapshot falling back to `BodyMotion` |
| `combat_tests` | combat (test 15): damage and death (the dead take none), 10 Hz exactly at 60 Hz steps, semi-automatic one round a press, the magazine and its reload (none during it, `reload` on a part-empty magazine only), a hit on a box with `Health` (point, normal, 25 damage), a round from inside the shooter's capsule through a trigger, a wall's surface and distance, the spread cone (10,000 draws, mean 2/3 of it), bloom growing and recovering, seeded patterns, recoil kicking and recovering, a turret hitting the player, the dead not firing (id order), two runs and a snapshot's replay bit for bit, the play scene's rifle; enemies (test 16): spawned at the enemy spawns, idle while the player is behind them, seeing and chasing, attacking after the reaction time (21 steps), bursts of 3 at 4 Hz with 0.8 s between, giving up 3 s after losing the line, woken by a hit, dead ones parked with no character, their Transform upright (the death clip falls), their fire killing the player, two runs and a mid-fight snapshot replaying bit for bit, the play scene's two enemies; the round (test 17): `round_start` once, a restart 180 steps after the player's death equal to the round's start (`world.hash`, poses, health, ammo, a knocked crate), the `restart` key, a dead player neither walking, turning nor firing, a win once both enemies die (waiting for `restart`, which brings both back), no round without `Round`, the same play after a restart, and after a won round's restart, bit for bit, and the play scene's won round restarting to the round start's `world.hash` (its enemies' yaw of exactly 180, rotations bit for bit); presentation (`fx_*`): the muzzle light on for its 3 steps then off, impact marks (64 at most, the oldest reused, never saved or hashed, on an enemy's body box and following it, none on the player, cleared by a restart), the HUD list (health, ammunition, the crosshair opened by bloom, the hit marker for 0.15 s and the vignette for 0.3 s, DEAD with the restart countdown, CLEARED, nothing without a player); the first-person gun (sub-project 9 test 21, `t21_*`): the rest pose (the remap only), the remap (every point within `depthScale` of its distance, projected through the scene's 75° where the original lands through 60°), sway lagging a 10° turn by 0.8° and returning, one bob cycle per 0.75 m walked (none standing or in the air), a kick of 3 cm and 2.5° per round returning, a restart resetting it, `world.hash`, saves and snapshots untouched by it; the won screen without crosshair or hit marker; the enemy's animation (sub-project 10, `t23_*`, `t25_*`): each AI mode and combat event giving its state in the same step (move at the chase speed, combat from the step it attacks, a kick per round, a flinch per round that hurt it, the death clip held at its end, the Transform upright, `body_fall` once), the reload clip's `mag_out`, `mag_in` and `bolt` 36, 78 and 105 steps in, the play scene's enemies skinned and posed before any step with the rifle and muzzle in the right hand, and a fight to the restart leaving both Animators and `world.hash` as at the round's start; the player's arms (sub-project 10 step 3): the arms' `mag_in` within one step of the weapon's `reloaded` at `reloadTime` 1.5 and 2.0, ADS (`WeaponState.aim` stepped over `adsTime`, exactly 1 after 12 steps, none for the dead; the cone linear in `aim`, half at 1; the drawn field of view 55° in live frames only, 75° in lockstep and captures, the arms remapped through it), the play scene's arms skinned under the ViewModel with the muzzle on the `gun` joint and the kick at half |
| `anim_tests` | animation (sub-project 10, tests 22 and 23, on the generated rig `tests/anim/fixtures/`, `tools/anim/make_test_rig.py`): the glTF reader (skin joints, parents, inverse binds; every sampler kind and channel path to the bit; morph channels dropped with a warning), sampling (LINEAR nlerp, STEP, CUBICSPLINE; loop and clamp; a key exact at its time), the bind pose's identity palette, Assimp's bone order mapped to the skin's joints by name (and the rig's 96 vertices kept: no join across weights), 1-D blends, masked additive layers, graph errors with their JSON pointers; the step: transitions on a parameter, an event and the clip's end (61 steps for 1 s), fades counted in steps, events once per crossing and in id order within a step, a restore mid-fade replaying the same Animator and `world.hash`, the pose never in the hash; a clip's rate from a parameter (`rateFrom`: its anchor event lands the parameter's seconds in, errors with their paths) |
| `vfx_tests` | effects (sub-project 12, test 31, CPU): `.vfx.json` parse errors at their JSON pointers, the default set (today's muzzle, sparks and dust by surface as data), the closed form against an RK4 integration, the play scene's fight writing an effect per shot and hit, tracers, decals and the grenade's boom, smoke and scorch, a placed `VfxEmitter` by step count, the caps dropping the oldest of a kind, a snapshot's and a restart's records replayed exactly, `world.hash` the same with and without the vfx system |
| `material_tests` | OpenPBR's base (`shaders/material/openpbr.slang`, OpenPBR step 1, T1) run on the CPU through its C++ build (`tools/material/openpbr_cpu`): the defaults are OpenPBR 1.1.1's; the exact dielectric Fresnel; `specular_weight` 0 over smooth EON reflecting exactly its albedo; a near-mirror metal reflecting its F0 head-on; each lobe reciprocal (the layered whole is not: OpenPBR's albedo scaling is at wo alone); `sample()` drawn with `pdf()` (chi-square on a 32x64 hemisphere grid, Sidak-corrected 1%) and weighted `eval/pdf`; `albedo()` the integral of `eval()` within 0.5%; the white furnace (white dielectrics at IOR 1.5 and 3 over white EON, a white metal) within 1% at roughness 0.01 to 1 and views to 85° |
| `openpbr_tables --check` | the checked-in energy tables (`ohao/render/material/openpbr_tables.hpp`) are what this build's module integrates, byte for byte; after changing the module, regenerate them with `build/Release/openpbr_tables ohao/render/material/openpbr_tables.hpp` |
| `audio_system_tests` | the audio facade (handles, volumes, categories) and sub-project 11's test 26 on a mixer without a device (`t26_*`): `.audio.json` parse errors at their JSON pointers, the slice's bank (Feel's curves, a file for every sound), the default bank's categories, a bus's gain on its subtree only, mute and solo, start times to the frame, a duck and the `dead` snapshot linear in dB (and its low-pass by band), a stop's 96-frame fade, two renders bit for bit, an occluded voice through its bus, the local folder first; test 27's mixer half (`t27_*`): an impulse through each of the slice's reverb presets decaying at its design RT60 within 20%, a bus's send scaling the return's tail (+6 dB for twice the send, none without one), 300 mixers with four senders each torn down cleanly |
| `audio_schedule_tests` | test 27's schedule half (sub-project 11 step 2, the blockout and the play scene): `ReverbZone` containment (on a face is inside; priority, then the lower id), one `zone` command with its 30-step blend at the step the listener crosses an edge, a level node's `verb` hint (a trigger box with `ReverbZone`, `col mesh` kept, an unknown preset `W_AUDIO_PRESET` and the default), occlusion graded by walls (none; −8 dB, 1.2 kHz; −14 dB, 700 Hz), a held voice re-cast every 6 steps, 16 rays a step (the rest counted, unoccluded), and the play scene's enemy shot rendered in the open and behind the pillar (8.6 dB down, 25.9 dB above 2 kHz); `--listen DIR` writes `step2_zones.wav` and `step2_cover.wav` with their JSON schedules; test 28 (step 3, `t28_*`): the rules moved from the window's players (footstep surfaces and round robin, combat sounds by shooter and surface, handling sounds at the enemy or at the ear, the voice cap's quietest lost, curves and the occlusion ray from the bank), priority and steal (lower priority first, then the quietest, then the oldest; a `stop` before its `play`; `dropped` when nothing is lower), `holdMs` from the bank, ducks (the deepest rule wins, attack and release), the snapshot from the round (`dead`, `won`, back to `default`), the same fight twice the same schedule, a restore mid-fight and a restart writing what a recording wrote (`stop_world`, `resync`), the world replaying the same with the schedule as without, the enemy's footsteps from its clips' `foot_l`/`foot_r` at its feet, `world.hash` unchanged by the schedule; test 30 (`t30_*`, ruling 23): one G throw, one `explode` voice 149 steps after the throw step where the throw's arc meets the ground (within 1 cm), unoccluded in the open, panned right with the listener facing west and left facing east |
| `env_sun_test` | the environment's sun (`render/rt/env_sun.hpp`): found and painted out with the sky around it (its irradiance, direction and share of the map's power), across the u seam, its dim halo with it and an unconnected bright cloud kept; studio softboxes and a wide bright patch are no sun and leave the map untouched |
| `check_cli.py` | `ohao` CLI goldens: stdout, files and exit codes, each case run twice (a case's `mask` keys, timings such as `loadMs`, compare as 0) |
| `check_scene_schema.py` | the generated JSON Schemas in a stock validator (needs `jsonschema`) |
| `check_tests.py` | `tools/check.py` itself, against fixtures in `build/check_fixture`; every built test executable registered; `render_bench --evaluate` on fixture results (pending budgets reported, a gated miss named by pass, noisy runs never a pass) |
| `test_golden_diff.py` | the golden harness's diff report on synthetic images: statistics, regions, heatmap, crop, `--json` |
| `check_doctor.py` | `ohao doctor` on fake shader trees (missing, stale, includes, Slang entry points and imports) and on the real tree; `--gpu` adds the device checks |
| `check_layout.py` | `shaders/shared/layout.json`'s generated files current, std430 offsets asserted, bad layouts refused, the generated GLSL and Slang compiling |
| `check_level_assets.py` | the checked-in levels (`assets/levels/`): every node's `extras` against the hint schema (`tests/level/extras_schema.json`), the blockout's nodes and hints (the contract the level pipeline's later tests assert), its Blender rebuild byte for byte (`skip` without Blender 5.2.2); the player's arms and rifle (`assets/characters/fps_arms/`, `tools/anim/make_fps_arms.py`) rebuilt the same way; the `verb` hint in the schema, the audio fixture's nodes (`tests/level/fixtures/level_audio.gltf`) valid and regenerated byte for byte by `tests/level/make_audio_fixture.py`; the transparency scene's MASK leaf card (`assets/vfx/alpha_card.gltf`) rebuilt the same way by `tools/vfx/make_alpha_card.py` |
| `grenade_spatial.py` | the test grenade (ruling 23) sampled in space (`tools/audio/`, CTest `grenade_spatial` into `build/grenade_spatial`; by hand it writes `C:/Users/djmax/ohao_refs/listen/sp11_audio`): `audio_bench --grenade-spatial DIR` stands the play scene's player outdoors facing north, the ambience bus muted, and sets the grenade off 4 m and 20 m to its left and right and 4 m in front through the schedule and the window's `AudioPlayer` without a device (`grenade_spatial.wav`, `grenade_spatial_schedule.json`); each 3 s window's L and R RMS, ILD (20 log10 L/R), peak channel, dBFS and a left/right azimuth guess go to `grenade_spatial.csv`/`.json`; passes with LEFT ILD ≥ +6 dB, RIGHT ≤ −6 dB, 20 m at least 6 dB below 4 m on each side, FRONT \|ILD\| ≤ 2 dB, nothing else starting in a window; `--no-run` measures an existing render |
| `check_serve.py` | `ohao serve` as a process: protocol goldens (`tests/serve/sessions` → `expected`, `--update`; `loadMs` and `buildMs` compare as 0), every tool schema through `jsonschema`, `tools/list` at most 28 KB, stdout clean under stray prints, the sandbox and caps, `ohao serve --pipe` as a process |

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
$ build/Release/skinning_gpu_test.exe > /dev/null   # gpu
$ build/Release/vfx_gpu_test.exe > /dev/null   # gpu
$ python tests/cli/check_cli.py > /dev/null   # gpu
$ python tests/validation/check_validation.py > /dev/null   # gpu
$ python tests/renderer/check_fireflies.py > /dev/null   # gpu
$ python tests/renderer/check_env_orientation.py > /dev/null   # gpu
$ python tests/renderer/check_analytic.py > /dev/null   # gpu
$ python tests/renderer/check_material.py > /dev/null   # gpu
$ python tests/renderer/check_parity.py > /dev/null   # gpu # slow
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
$ build/Release/feel_bench.exe --check tests/bench/feel_budgets.json --seconds 10 > /dev/null   # slow
$ build/Release/audio_bench.exe --check tests/bench/audio_budgets.json > /dev/null   # slow
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
  (deferred with RT shadows and GI, deferred with CSM: byte for byte; path traced, whose bounce
  rays meet emitters and weigh them against light sampling: the same light, the mean within 1%
  overall and 5% per 32x32 block), with them in view each emitter is bright and every other
  pixel the same (deferred: 1 LSB within 24 px of an emitter, its denoisers' neighbourhoods;
  path traced: the same light past 24 px); a moved light's emitter carries last
  frame's placement as its prevWorld, and under TAA a moving point light's mesh is as bright
  as the light held still; the first-person gun (`ViewModel`, sub-project 9): a capture with
  the gun against one without it differs only inside the gun's footprint (it shadows and
  lights nothing), where a plain box at the gun's drawn place changes pixels outside it; the
  player's skinned arms under a ViewModel change nothing past 24 px of their footprint.
  `skinning_gpu_test` (sub-project 10, test 24, the rig): the deformed range read back equals a
  CPU skin of the same palette (positions, normals, 1e-5 relative); two instances of one mesh hold
  their own poses; after a window frame (a step, one sync, the frame) the copy's previous positions
  are last frame's skin (the foot, joint 0 only, has no motion); the raised arm changes the deferred
  frame and the path tracer's in the same place; a moving pose refits its BLAS every frame and
  rebuilds it after 120 refits; a still pose runs no skin, bounds or refit work; `render.stats`
  `skinning`. `vfx_gpu_test` (sub-project 12, test 32): the transparent pass's objects back to front from
  four views, two at one distance in id order, the nearest of three overlapping panes winning where
  they overlap; a lit puff by a red lamp redder than the same puff 5 m off (per-particle light read
  back), unlit its own colour; a soft puff crossing the floor stepping no more than 1/8 of its light a
  pixel (a hard one steps); a cut MASK card letting RT shadows and the path tracer through, a kept one
  shadowing; a BLEND pane casting no RT shadow or GI; a capture at a step the same after 0, 7 and 200
  frames; a forward-shaded plane equal to the deferred one within 1 LSB (`--keep` leaves the PNGs in
  `build/`). `cull_parity_gpu_test` test 8: a rig whose bind box is off screen draws its raised arm,
  culling on = off, mesh shaders = fallback = instances. `deferred_determinism_gpu_test` covers the
  rig (deferred exact, path traced within the floor).
  `check_live.py`: `ohao serve --window` (a hidden window, validation on) driven
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
  fight headless reaching the same `world.hash`, `combat.info`, events and HUD picture; a realtime
  window's `render.stats` reports `latency` and `pacing` after injected mouse movements, a lockstep
  one neither (sp9 tests 19, 20); the fight's voice schedule from the window equal, command for
  command, to a headless session's (`OHAO_AUDIO_TRACE=1` through `log.read`, sp11 test 28); a realtime window with `--sound`
  reporting `render.stats` `audio` (`mixMs`, `voices`, `late`), one without none (test 29). `e2e_mcp.py`: a
  stdlib-only MCP client runs a whole session against a hidden window (load, edit, snapshot,
  record, a flight, step with watch, captures, logs, restore, replay to the same hash, a
  capture bit-identical to the first; the blockout's level and physics; the play scene walked,
  `player_info`, `player_teleport`, `physics_contacts`, restored and walked again to the same
  hash and picture; the round fought: `combat_info`, EnemyA shot down, `combat_events`, a look
  with the HUD, `combat_damage` killing the player, the restart, the room cleared and won;
  the fight listened to: `audio_info`'s newest commands, a bus muted and one soloed, `clear`,
  `world_hash` untouched), validation clean through teardown.
- **Validation** (`OHAO_VALIDATION=1`): zero messages, teardown included. Each message
  is a record on the logger's `validation` channel, on stderr as
  `W validation: <VUID> | <text>` (`E` for errors; a run of identical ones ends with
  `... repeated N more times`). `check_validation.py` renders Cornell, the helmet, the
  skinned animation rig and the play scene a second into the fight, deferred and path traced (offline, realtime, realtime through NRD), under the layers;
  `--json FILE` gives each scene's message count, first VUID and per-VUID counts.
  `OHAO_VALIDATION=sync` adds the layer's synchronization validation (hazards between
  commands, as `SYNC-HAZARD-*` messages): slower, for chasing a missing barrier.
- **Fireflies** (`check_fireflies.py`): the play scene from the spawn looking west, 4 frames after
  a reset (a moving camera's GI history), RT GI on against off: pixels GI made a bright speck (what
  GI adds over its 5x5 median by more than 12 levels, and the GI-on image over its own 5x5 median
  by more than 12), at most 150: 14 with the HDRI's sun taken out and GI's bounce shadowed, 524 with
  the sun left in GI's rays (and the bounce unshadowed);
  and the log saying the environment's sun was taken out. The speck test is Frank's ruling of
  2026-10-29: with the sky the right way up, what GI adds alone counted 363, mostly thin features
  (the rifle, fingers) that the real sky's bounce lights differently from what is behind them.
- **Render goldens** (`tests/golden/manifest.json`, 32 scenes; two the transparency scene,
  `transparency.scene.json` (blended panes, the MASK leaf card, lit soft smoke, sparks; deferred exact
  and path traced); 8 examples, 22 through
  `ohao run`; eight at the deferred frame's `reference` quality; three the skinned animation rig
  mid-wave, `anim_rig.scene.json` (deferred, reference, path traced); eight of them the Khronos
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
  `tools/list` size; on the play scene after 120 steps of fire, `audio.info`'s size at `max` 16,
  `audio_info_bytes`; then `ohao serve --window 1280x720 --hidden` on Cornell: a step until
  its frame is presented, a frame's readback, inline and PNG captures, an inline `rt_offline`
  capture at 16 spp (150 ms, Frank's ruling), the reader thread's CPU over 600 idle realtime steps; `--cpu-only` skips the window), results in
  `build/serve_bench.json`. `render_bench --check`: the renderer's GPU budgets in
  `tests/bench/render_budgets.json`, each tagged `gatedFrom` the 2b step that delivers it
  (gated once `currentStep` reaches it, else reported `pending` with its measurement);
  headless captures of `cornell_720p`, `helmet_game_720p`/`_1080p`,
  `bench10k_1080p`/`bench100k_1080p`, `play_hud_1080p` (the play scene with its HUD: the
  `hud` pass, 0.05 ms) and `play_vfx_1080p` (the play scene's effects: a grenade thrown and the
  rifle held down, stepped to the boom and before every frame: `vfx.expand` 0.08, `vfx.sort` 0.04,
  `transparent` 0.30 ms, sub-project 12's proposals) and
  `anim16_1080p` (16 skinned rigs in the blockout, the world stepped before every frame so every
  pose changes: `skin` + `skin.bounds` 0.25 ms, `rt.blas.refit` 0.6 ms, sub-project 10's
  proposals) and `play_anim_1080p` (the play scene stepped the same way: its two enemies' `skin` +
  `skin.bounds` 0.05 ms, `rt.blas.refit` 0.1 ms; `play_hud_1080p`'s frame is gated at its pre-SP10
  median 2.19 ms + 0.2), 5 warm-up and 100 timed frames each, every pass's median
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
  enemies chasing, `enemies16_step_ms`; restoring the play scene's round start, `round_restart_ms`;
  and feel's, gated by `feelStep`: the window host's occlusion ray for an enemy's sound in the
  blockout, per ray, `occlusion_ray_us`, 5 µs; and animation's, gated by `animStep`: the
  `animation` simulate system over 16 rigs, `anim16_step_ms` 0.05 ms, and the `pose` sync system,
  `pose16_sync_ms` 0.1 ms; the play scene's three animators, its two enemies and the player's arms,
  `anim_step_ms` 0.01 ms and `pose_sync_ms` 0.02 ms), CPU, results in `build/physics_bench.json`; the same noisy-run rules.
  `feel_bench --check`: input-to-present latency and frame pacing (sub-project 9) in a *visible*
  1920x1080 window (a hidden one may never present), the play scene played for a scripted minute
  per present mode (`--modes`, `--seconds`, `--warmup`) while a mouse movement is fed every 40–60
  ms; budgets in `tests/bench/feel_budgets.json` (`look_latency_ms` median, gated only on a display
  of at least `minRefreshHz` with present wait, and `frame_p99_over_median`, each in the present
  mode it names, both gated since Feel step 2: latency 4.8 ms passes, p99/median 1.99 misses 1.5,
  `docs/changes/2026-10-27-feel-step2.md`), results in `build/feel_bench.json` and per mode
  `feel_bench_<mode>.csv` (every frame's time, steps and phases: start→poll is the late input wait,
  poll→view the steps, view→submit, submit→present; every movement's latency; a slow frame's
  interval is the previous row's phases) and `feel_bench_<mode>_events.csv` (the round's deaths,
  restarts, wins and reloads, same clock); `--frames-ahead N` tries a `framesAhead`. Other
  processes' CPU is recorded per mode (dwm.exe aside): over half a core, a gated budget is `noisy`,
  exit 2, rerun by `tools/check.py`.
  `audio_bench --check`: the mixer's budgets in `tests/bench/audio_budgets.json` (sub-project 11):
  `mix_10ms_ms`, the slice's bank rendered without a device with 32 voices and every bus under
  `world` filtered and sending, the median of 300 periods of 480 frames (0.5 ms per 10 ms, gated
  from step 1; since step 2 it includes the reverb on the return), `reverb_10ms_ms` (the Freeverb
  alone, stereo, on noise, one preset: 0.15 ms, gated from step 2; `reverb_xfade_10ms_ms`, the same
  mid-crossfade, reported), `bank_load_ms` (gated from step 3); step 3's, gated from step 3:
  `schedule_step_ms` (the schedule's step over the play scene's scripted fight, 0.02 ms),
  `schedule16_step_ms` (16 enemies running in and firing in the blockout, their bursts coinciding past
  the 16 rays a step: 0.1 ms; the run fails if no ray was capped) and `submit_frame_ms` (a window's
  `AudioPlayer` reading and submitting a frame's commands over the same fight, 0.05 ms); the same
  noisy-run rules; `--listen DIR` writes one burst with each bus soloed in turn (`step1_buses.wav`,
  its levels as CSV) and the done demo's fight through the player (`step3_fight.wav`, its schedule
  as JSON, every bus's driven level as CSV), for listening.
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
- **Alpha modes** (`render::AlphaMode { Opaque, Mask, Blend }`, glTF's `alphaMode`; primitives'
  `Material`'s `alpha_mode`/`geometry_opacity`): coverage is base colour alpha x the albedo texture's x
  `opacity`, in material row 3 (`mode, cutoff, opacity, base alpha`). A `Mask` draw (`kDrawMask`) is in
  no G-buffer cull list: the G-buffer draws it after the late lists with `core/gbuffer_mask.frag` (a
  discard, so no forced early depth test). A `Blend` draw (`kDrawTransparent`) is in no list at all,
  cascades included: the `transparent` pass draws it. In the TLAS an instance with an alpha material is
  `FORCE_NO_OPAQUE`; inline ray queries test each candidate in `rt/alpha_test.slang` (mask: the cutoff;
  blend: never a hit, so it casts no shadow and bounces no GI), the path tracer in its `anyHit` (blend:
  stochastic opacity). A new trace must not force opacity on those instances or it shadows through
  every hole. Windows' `OPAQUE` macro is gone from this path (the enum is `Opaque`).
- GLSL `flat` is a reserved qualifier; rename locals called `flat`.
- `GLM_ENABLE_EXPERIMENTAL` before any `<glm/gtx/...>` include.
- Jolt: `dynamic_cast` on Jolt types crashes (no RTTI); `SetMotionQuality` lives on
  `BodyInterface`. `PhysicsWorld(cfg)`, `stepOnce()` not `step(dt)`; bodies need a shape.
- `ohao::physics::math::AABB` is (center, halfExtents); render bounds (`MeshInfo`,
  `DrawRecord`) are (min, max). A box passed between them needs converting.
- ECS writes: `Transform`, `MeshRenderer`, `Light` and `EmissiveProxyLight` reach the
  renderer only through `registry.patch<T>()` /
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
  RGBA8 output (`pt.present`, which follows the tracer's passes because the pass registry is
  in frame order) and presents like the deferred path. What a window presents is a launch
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
- **A directional light stands for the sun.** While the scene has one
  (`RenderScene::hasDirectionalLight`), the HDRI is uploaded with its sun painted out
  (`render/rt/env_sun.hpp`: the peak's connected texels over 64 x the median sky, if the peak is
  over 10^4 x it and they cover at most 0.02 sr), so RT GI's rays, the deferred IBL and the path
  tracer light with the sky alone and the sun arrives once. GI's cosine rays met the outdoor
  HDRI's sun (60,450, 51% of its power on 0.003% of the sphere) about once in 10^4 rays: the
  outdoor fireflies (`docs/changes/2026-10-28-rt-gi-fireflies.md`). The log says what was taken
  out (`gpu` channel). A directional light added or removed reloads the map.
- **An equirectangular environment's row 0 is the zenith**, as the file stores it and `stbi_loadf`
  loads it: every lookup is `u = atan2(z, x) / 2pi + 0.5`, `v = 0.5 - asin(y) / pi` (deferred
  lighting, `equirectUV` in `clustered_lights.slang`, the prefilter's both ways, `gi_trace`,
  the path tracer's miss, the cinematic composite, `env_sun.cpp`), the same number as the path tracer's
  sampling (`envPdf` in `path_tracer.slang`, `acos(y) / pi`). A new lookup writes that expression; `asin(y) / pi + 0.5`
  turns the sky upside down (it did, everywhere, until 2026-10-29) and the unit tests cannot see
  it. `check_env_orientation.py` can: a sky blue above the horizon and orange below must light a
  floor blue in deferred (RT GI and IBL) and the path tracer.
- **Deferred and the path tracer light and show the same scene.** A scene without lights has
  none in either (no fallback sun: `setFrameLights` hands over the scene's raster lights only), and
  with an environment the deferred frame draws it behind the scene (lighting writes prefiltered
  level 0, the source, into sky pixels; the procedural sky pass runs only without one), as the
  path tracer's missing camera rays show it. `check_env_orientation.py` renders a scene with no
  light and checks the sky band in all three captures.
- Reflected light is counted once. Lighting writes, beside the lit image, the
  environment/GI specular it added (`envSpecular`, E) and its weight (`specularWeight`,
  W: split sum times specular occlusion, computed for every pixel, environment or not);
  SSR's resolve (`postprocess/ssr_resolve.slang`) rewrites the lit image in place,
  `lit += W·(c·L) − c·E`, before bloom and TAA. A new reflection source must go through
  E/W, never be added on top in post.
- Indirect diffuse light has one source per frame: RT GI's rays, else the environment's
  irradiance, else (neither: no `VK_KHR_ray_query` or `rtGI` off, and no environment) the
  constant ambient (`ambientIntensity`, 0.35; `setAmbientIntensity` overrides it). With RT GI the constant does nothing, so walls are lit by direct light
  and one bounce, darker than the multi-bounce path tracer, and RT GI's denoiser noise
  shows on them. A test that wants ambient light must turn RT GI off. All three are
  diffuse only, weighed by OpenPBR's diffuse directional albedo beneath the specular layer
  (`diffuseAlbedo`; the split sum's weight is `specularAlbedo`, both from
  `shaders/material/openpbr.slang` and its energy tables). RT GI's rays bounce off the hit
  triangle's own material: `rt/alpha_test.slang`'s `hitMaterial` resolves it at the hit's uv
  (textures included) and the hit returns its OpenPBR albedo at normal incidence over π times
  the direct light it receives, plus its emission.
- **The G-buffer has 8 attachments** (`GBufferPass::GBUFFER_COUNT`; depth is attachment 4,
  so the fragment shader writes 7 outputs and a pipeline drawn in its render pass needs
  `GBUFFER_COUNT - 1` blend states). Emission is `kEmissive` (5, RGBA16F, output 4):
  `emission_color × emission_luminance × the emissive texture` (engine units, ruling O4), the
  same radiance the path tracer's closest hit emits, added by lighting unlit; the normal
  target's alpha keeps only its luminance, to keep emitters out of SSS. `kSpecular` (6, RGBA8:
  `specular_color`, `specular_weight / 1.5`, so 1.0 is code 170) and `kOpenPBR` (7, RG8:
  `base_diffuse_roughness`, `(specular_ior − 1) / 2.55`, so 1.5 is code 50) carry the rest of
  OpenPBR's base to `core/deferred_lighting.slang`; albedo is `base_color × base_weight`, and
  roughness is stored as authored (OpenPBR floors it at 0.01, nothing at 0.04). A back face is
  shaded on its own side: the G-buffer flips the normal when it faces away from the triangle's
  side the camera sees (`cross(dFdx(pos), dFdy(pos))`; `gl_FrontFacing` disagreed with the
  generated sphere's winding).
- **The referee.** A renderer is judged against the closed form first, then the path tracer:
  `check_analytic.py` renders a white floor under one sun, point or spot light (no other surface,
  so no bounce) in both renderers through `ohao run --hdr` and holds each within 5% of
  `albedo/π × irradiance`; `check_parity.py` (slow, a report) renders the reference scenes both
  ways and gives deferred's distance from the path tracer in stops, per scene and on a 4x4 grid,
  with `build/parity/<scene>.error.png`. Compare renderers in linear radiance (`--hdr`), never by
  PNG: the two tonemap differently (deferred at the capture's exposure, the path tracer at 0.5).
  The path tracer is ground truth where closed forms hold it: `check_analytic` (one light, no
  bounce) and `check_material.py`: a white furnace (white OpenPBR materials under a uniform sky
  of radiance 1 render as 1, within 1%) and a closed box of emitting Lambertian walls
  (`Le / (1 - rho)` with every bounce, `Le (1 - rho^(N+2)) / (1 - rho)` with `--bounces N`).
- **The path tracer** is `shaders/rt/path_tracer.slang` (`PathTracerShaderSet`'s default, both
  profiles), a correct estimator of OpenPBR's base
  (`shaders/material/openpbr.slang`): one path a sample, light and environment sampling weighed
  against BSDF sampling by the power heuristic, Russian roulette from the third vertex, 32
  bounces. Its random numbers are Owen-scrambled Sobol (Burley 2020, `shaders/sampling/sobol.slang`):
  draws come in groups of four, one 4-D point each, seeded by the pixel and the group; a bounce's
  draws sit in fixed groups (light, environment, BSDF and roulette), so a branch never shifts the
  next bounce's dimensions. A new draw goes into a group (`Sampler.group`), never after another
  bounce's. `sobol_test` runs the module through slangc's C++ (`tools/sampling`) against the CPU
  generator, bit for bit, and holds its stratification.
- **Realtime denoising and display** (frontier F1 T4). A realtime window's denoiser is DLSS Ray
  Reconstruction where the build has the SDK (`python tools/fetch_dlss.py` into the git-ignored
  `external/DLSS`; never commit it: NVIDIA's licence) and the GPU is NVIDIA's, else NRD
  (`VulkanRenderer::rtDefaults`); captures default to `denoise: none`. DLSS RR upscales: it traces
  at its preset's size and writes the output's (`OHAO_DLSS_QUALITY`: `quality`, the default, 2/3 a
  side; `dlaa` native, `balanced`, `performance`, `ultraperf`), so the tracer's images are at the
  render size and only DLSS's output and the frame's are at the output's. Every denoiser writes the
  frame's output (`m_outputImage`) with the tracer's own curve (ACES at 0.5, gamma 2.2: one display
  transform until F2), so a window presents what a capture reads. Under NRD the camera ray's
  emission (lights, emissive surfaces, the sky) goes to its own guide (binding 13) and never
  through NRD: `nrd_compose` adds it to diffuse x albedo + specular. The first hit's direct light
  is split by lobe (`evalLobes`). NGX logs through the engine's logger (stdout is the protocol's).
- **The path tracer's frame cost** (frontier F1 T6) is timed by stage: `pt.cache.update` (with the
  frame's setup), `pt.cache.resolve`, `pt.trace`, `pt.denoise` (a denoiser and the display
  transform); `render_bench`'s `helmet_rt_1080p`, `play_rt_1080p` and `play_rt_dlss_1080p`. **Any
  buffer a shader reads at random must be device-local**: the environment's sampling table in host
  memory (read over PCIe at every draw) was 85% of a realtime frame. It is a 2D CDF
  (`render/rt/env_cdf.hpp`, binding 17: the marginal, then each row's conditional), uploaded through
  a staging buffer with the map. Keep the draw a continuous warp: an alias table was 1.1 ms faster
  but scattered neighbouring Sobol points across the map, 14% more error at 16 samples. A view of a 1080p render through an image tool may be palette-reduced and look
  posterized; judge renders from crops or pixel statistics.
- **The radiance cache** (frontier F1 T3, `shaders/rt/radiance_cache.slang`, `render/rt/radiance_cache.*`;
  SHaRC's design, our code: RTXGI's licence bars shipping its source in this MIT repo). The realtime
  profile ends a path at a vertex past the first in a world-space hash grid of reflected radiance
  (cells of 0.05 m x 2^level, level = log2 of the distance from the camera, keyed by the side the
  surface faces) when the segment is a voxel long and the last lobe spreads over the voxel; a
  sparse update trace (`cacheUpdate`, 1 pixel in 5x5) fills it every frame and a resolve pass blends
  and evicts. **Integers only**: radiance is summed x1024 by `InterlockedAdd` (no float atomics; the
  sum is order-free, so two runs give the same bits) and `resetAccumulation` empties it, so a
  capture repeats in any host. The offline profile never reads it (its slot holds one entry: the
  shaders bind it either way). The realtime closed box reaches its closed form within 3%
  (`check_material`); realtime Cornell is within 0.3% of the reference. Push constants are shared by
  the bind points: anything recorded between pushing the trace's constants and tracing that pushes
  its own (the resolve) must push the trace's again. A bounce ray (`kRayMaskScene | kTlasMaskEmitter`) that meets a light's emitter mesh
  weighs it by the light's index in `InstanceGpu.emitterLight.w`; a light without a mesh (a
  point of radius 0, emitters off) is flagged by `LightTable::setEmitterLights` and sampled at
  full weight. Emissive proxies are not path tracer lights (the surfaces are). `--bounces N`
  (`render.capture {bounces}`) for the referee: 0 direct light only. The realtime profile is the
  same tracer at 2 bounces with NRD by default, and `check_material.py` holds it to the same closed
  forms. **Two clocks:** the trace's running mean (`pc.control.y` frames) restarts when the view
  moves (`notifyViewChanged`), but a temporal denoiser's history (`m_historyFrameCount`) restarts
  only at `resetAccumulation`, reprojecting through the motion vectors, which are written whenever
  last frame's view exists (`pc.control.z`). Under NRD (realtime) or DLSS RR the trace hands the
  denoiser this frame's sample alone (`tracerAccumulates`), and only then does the firefly clamp
  apply; offline NRD denoises the mean of a still view (`tracerAccumulatesAOVs`). A `--denoise`
  override reaches the tracer whatever it is. Its floor
  across hosts: a ray exactly on a shared triangle edge may take either triangle (the driver
  builds BVHs per process), and the reference tests paths against the hit triangle's geometric
  normal, so a few pixels may differ by any amount between a window and a headless host (one
  process repeats itself bit for bit).
- **RT GI's bounce sees every light** (`gi_trace.slang` `directAt`): up to `kGIWeighedLights`
  (16) each is weighed in one pass, past that `kGILightCandidates` (8) are drawn from an alias
  table in `GILight.cone.zw` and resampled; one shadow ray either way. `check_analytic.py` holds
  it at reference quality: spots aimed at a wall must bounce, together, what one spot of their sum
  does and what each does alone. A one-pass reservoir must always take the first light that gives
  any: a GPU's `w / w` can round below 1 (it left pixels with no light chosen, NaN, which
  reference quality's running mean keeps and TAA and bloom spread).
- Point and spot lights mean the same in both renderers (Frank's ruling, 2026-10-26): a
  light of intensity `I` lights a surface `d` away with `I/d²`, windowed to exactly 0 at
  its `range` (default 10 m) by `(1-(d/range)⁴)²`; a directional light of intensity `I` gives a
  surface facing it irradiance `I`. The path tracer's sphere emitter has
  radiance `I/(πr²)` (`path_tracer.slang`'s light sampling); albedo is base colour × albedo texture
  in both, never the vertex colour (primitives carry their colour as both). An area light
  is the same rectangle in both (Frank's ruling): uniform radiance `I/(4πr²)`, one-sided
  toward `edge1 × edge2`, no range and no `1/d²`; deferred lights it as light type 3
  (`evaluateRect` in `includes/lighting/clustered_lights.slang`: the exact polygon irradiance,
  the representative point for specular), clustered lighting lists it in every froxel, RT GI's
  bounce sees a cosine-profile point at its centre. Both renderers shade with the same BSDF,
  OpenPBR's base (`shaders/material/openpbr.slang`); what stays between them is one GI bounce
  against many and the representative-point specular of sphere and area lights
  (`docs/changes/2026-10-29-openpbr-step1.md`).
- Every point, spot and area light is also a mesh the camera sees (`RenderScene::setLights`
  makes an emitter instance per light: a sphere of its radius, or its rectangle, with the
  light's radiance, `kInstanceEmitter`). It is pure emission and changes no other pixel: its
  TLAS mask (`kTlasMaskEmitter`) is in the path tracer's camera and bounce rays' masks only (a
  bounce ray that meets it is weighed against the light's sampling, so it counts once), so no
  shadow or GI ray hits it (else the light would shadow itself or be counted twice); no CSM cascade draws it
  (`kDrawEmitter`); the
  G-buffer marks it with emissive alpha 1 and metallic −1, so lighting outputs its radiance
  alone, SSR treats it as a miss and RT shadows and GI skip it as sky. A new trace passes
  `kRayMaskScene`; a new screen-space pass that reads the lit image must decide what an
  emitter pixel is to it. `setLightEmitters(false)` turns the meshes off (the lights stay).
  A mesh moves with its light and writes motion vectors as any mesh does: the extract passes
  the light entity's `prevWorld * inverse(world)` as `LightDesc::lastFrameMotion`, and the
  emitter's prevWorld is that times its world (identity, so prevWorld = world, when still).
  Mirrors reflect it: in deferred a point or spot light's specular is its sphere's
  (`LightGpu.params.z` = the mesh's radius, `evaluateSphere`: the representative point,
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
    interpolated across. A capture syncs before it resolves the view. The input-driven player is
    the exception in a realtime window: its turn is the newest step's plus the look lead (see
    "Frame timing", the look lead), its position interpolated.
  - The window reports look input only from a captured cursor, as `MouseDelta`;
    `ohao_game` on a scene with a `PlayerController` captures the mouse while focused,
    `release_mouse` (Esc) frees it, and a click on the window takes it back
    (`LiveHost::setClickToCapture`, `runtime/click_capture.hpp`): that press and its release
    are swallowed before the input map sees them, so the capturing click never fires.
  - Footsteps: the player's step logs a `footstep` event every 0.75 m walked (0.95 m
    sprinting) on the ground and a `land` after a fall of more than 0.3 m, in the physics
    event log (`physics.events`), with the ground's `Collider.surface` (hint `surf`; empty is
    `default`). The voice schedule turns them into `footstep_<surface>` plays (a character whose
    animation graph has `foot_l`/`foot_r` events steps from those instead; "Audio", below); a
    window's `AudioPlayer` plays them from git-ignored `assets/local/audio/footsteps/` (staged
    for local listening only, never committed) else `assets/audio/footsteps/` (ours,
    `tools/audio/make_footsteps.py`).
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
    1,024): `shot`, `hit`, `damage`, `death`, `reload`, `reloaded`, `explode`.
  - **The test grenade** (audio spec ruling 23, `combat::kGrenade*`): `grenade` (G in the play
    scene) throws from the eye along the look; it flies in the combat step after the weapons,
    sticks where a ray along its move first meets something, and 2.5 s after the throw writes
    `explode` at its place, which the schedule plays occluded as a world source. No damage. A
    grenade in flight is runtime only: never saved, hashed or snapshotted (a restore keeps the
    ones in flight, a restart clears them).
  - Saved types hash their names even with no instances, so a new saved component moves every
    `world.hash` in the serve and CLI expectations, not only scenes that use it.
  - Enemies (step 2) are characters on the player's code: `PlayerController` with `spawn`
    (the k-th character with a tag takes the k-th `SpawnPoint` with it, creation order; empty:
    where it stands) and `input: false` (moved by its saved `MoveIntent`, not the world's
    input; weapons fire by input only for `input: true`). The enemy AI (`ecs/enemy_ai.cpp`)
    runs at the start of the combat step, before the weapons: sight is a `raycastFrom` from its
    eye to the player's chest (in its cone only while idle); it writes `EnemyState`,
    `MoveIntent` (the next step walks it) and `WeaponTrigger` (this step fires it), all saved.
    A dead enemy loses its `PlayerController`; its `Transform` stays upright (its death clip is
    the fall, sub-project 10, OPEN ruling 7 provisional); its character is parked
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
    `hud: true`/`--hud`, and only on deferred frames; no crosshair or hit marker while dead or
    once the round is won. Its sounds (`assets/audio/weapons/`, ours:
    `tools/audio/make_weapons.py`) are the voice schedule's plays ("Audio", below).
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
- **Frame timing** (sub-project 9, `runtime/frame_log.hpp`): a window host stamps each realtime
  frame (poll, view, submit, present) and the look movements it shows, in a `FrameLog` that
  `render.stats` (`latency`, `pacing`), `ohao_game --latency` and `feel_bench` read. Presents are
  confirmed with `VK_KHR_present_id` + `VK_KHR_present_wait` where the device has both: a thread
  (`gpu/vulkan/present_waiter.hpp`) polls `vkWaitForPresentKHR` with a zero timeout about every
  0.25 ms, holding `Swapchain::hostMutex()`, which every host access to the swapchain (present,
  rebuild, destroy) takes, as Vulkan requires; a present unconfirmed after 250 ms, or without
  present wait, keeps its call stamp (`source: present_call`, an underestimate). With a look-led
  player a look movement counts as shown by the first frame after its poll; without one, once the
  step after the one that applied it has run. `ohao_game --present immediate|mailbox|fifo` and
  `ohao serve --window --present` choose the present mode (`--vsync` is fifo; the default stays
  mailbox, which measured the same latency as immediate in a window).
  - **The look lead** (step 2): a realtime window hands the World the look no step has consumed
    yet (`World::setPendingLook`: the window's mouse movements since the last step and the
    `input.inject` ones due at the next step, summed in the step's order), and the `interpolate`
    sync system draws the input-driven, living player turned by it through `applyLook`, the step's
    own arithmetic, so the next step lands exactly where the frame already was. **Gotcha: that
    player's rotation is drawn from the newest step plus the lead, never interpolated** (only its
    position is); anything else interpolates as before. Presentation only: `RenderPose`, never
    `Transform`, `world.hash`, a file or a snapshot; no lead in lockstep (alpha 1, nothing pending),
    for the dead, or for characters moved by their `MoveIntent`.
  - **Late input sampling:** a realtime frame polls only once at most `LiveConfig::framesAhead`
    submitted frames are unpresented (`presentToAwait`; it waits on present wait, else the frame's
    fence, at most 20 ms). Automatic (-1): 1 while a player leads its look, unbounded otherwise.
  - **Stalls:** every window on this laptop gets a few frames a minute of 330–500 ms (5–16 a
    minute measured) in which the presentation engine holds the swapchain's images: the frame's
    GPU work is normal and starts late, nothing of the engine's is queued in the gap, and a
    30,000-frame headless loop never shows one. They are not the engine's to fix; `feel_bench`
    counts them (`stalls`).
  - **The first-person gun** (step 3, `ecs/view_model.hpp`): an entity with `ViewModel` (saved; the
    play scene's Gun) and its subtree are drawn by the `view_model` sync system (after
    `interpolate`; `installCombat` installs it): placed in the view by `viewModelRemap` (S =
    diag(d, d, d/k), k = tan(scene/2) / tan(fov/2): every point stays within `depthScale` of its
    distance, so it never reaches into a wall it stands against, and lands on screen where it would
    through `fovDeg`), swayed by the drawn turn, bobbed by ground speed, kicked by the owner's
    `shot` events, all at render rate from `World::presentationDt()` (the window's frame time; 0 in
    lockstep and captures: the rest pose). `RenderPose` only. Its instances are `kDrawViewModel`
    (no CSM cascade) with TLAS mask `kTlasMaskViewModel` (4), in only the path tracer's camera ray
    mask (`kRayMaskCamera` 7): it casts no shadow and no GI. A light below it (the Muzzle) follows
    its `RenderPose`. **Gotcha: the projection is `glm::perspective` without
    `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so under Vulkan's [0, 1] depth anything nearer than about twice
    the near plane is clipped** (0.1 m at the play scene's 0.05): a `depthScale` of 0.25 put the
    gun's near end there and it vanished; 0.55 keeps it at 0.108–0.27 m, inside the 0.3 m capsule.
  - **Effects** are the world's (sub-project 12, "VFX" below): drawn from the step's records in every
    host, windows, captures and `ohao run` alike, so a window's lockstep capture equals a headless one
    with its effects (`check_live` `t32_window_equals_headless`). There is no `--fx`/`--no-fx`.
- **Animation** (sub-project 10, `docs/specs/2026-10-28-animation-design.md`). `ohao/anim` (no ECS,
  no Vulkan) reads a glTF's skins and animations itself (`gltf_anim`, simdjson; Assimp's animation
  import is not used), binds clips to skeletons by joint name, and parses and validates
  `.animgraph.json` (every error with its JSON pointer). `installAnimation(world, assets)` (the CLI
  context and `LiveHost` call it after `installCombat`) adds the `animation` simulate system and the
  `pose` sync system (before `render_extract`).
  - **A pose is presentation; animator state is saved integers.** The saved `Animator` holds
    integers only: each layer's state, its steps, the state faded from and
    its steps, the fade's length and progress. Never keep a clip time or a pose in a saved
    component; force a state with `component.set` on `Animator` (`animation.info` reads it back). A pose is a function of them and alpha, sampled at
    `(steps - 1 + alpha)` steps of 1/60 s; the pose (`AnimPose`) is runtime, never saved or hashed.
    Clip events fire in the step, in id order, into a log of 1,024 (`animation.info`).
  - **A skinned instance is its own mesh.** The render extract gives an entity with `SkinnedMesh`
    a mesh from `RenderScene::registerSkinnedInstance(base)`: its own vertex range (the base's
    copied), the base's indices and triangle slots, copies of its meshlets, an updatable BLAS. The
    `skin`, `skin.bounds` and `rt.blas.refit` passes run before `scene.upload` (whose TLAS build reads
    the refit BLAS), only for the instances whose palettes changed. Nothing changes for consumers:
    they read the instance's mesh as any other.
  - The copy's `boneWeights.xyz` hold last frame's position (draw flag `kDrawSkinned`): the motion
    vectors' source in `gbuffer.vert` and `cull/meshlets.slang`. Nothing may read a skinned copy's
    joints or weights; the skin pass reads them from the base mesh.
  - The previous palette is the previous *sync's*, as `WorldTransform::prevWorld` is: a capture
    (which syncs for the view, then per frame) and `world.step` (which syncs) start without motion.
    A window frame (steps, one sync, the frame) has it.
  - The model loader keeps a skinned file's vertices as written (no `JoinIdenticalVertices`):
    Assimp's join ignores joints and weights and merged seam vertices with different weights.
  - **The enemy** (step 2): `assets/characters/toon_enemy/` (Quaternius, CC0, rebuilt by
    `tools/anim/make_enemy.py`, pure Python, from the staged upstream file; `README.upstream.md` says
    what it changed) and `assets/anim/enemy.animgraph.json` (body `move`/`combat`/`death`/`dead`;
    additive `aim`, `fire`, `reload`, `hit`). Its driver `enemy` (`ecs/enemy_anim.cpp`) reads saved
    state and the step's combat events **after the step's last `restart`**: a restart restores the
    round's start inside the combat step, before the animation step, so a restart step must step the
    restored Animators exactly as the round's first step did. Damage dealt between steps
    (`combat.damage`) lands in the previous step's events and flinches nothing; death still follows
    `Health`.
  - Additive clips are authored as rest × delta (the layer adds rest⁻¹ × clip): a pack clip used
    additively is re-referenced to its own first frame by the tool, never added raw.
  - `JointAttach` poses the attached entity's whole subtree (the rifle's muzzle light follows the
    hand). An enemy's body and head entities have no mesh: rounds hit its capsule (ruling 18), so
    impact marks, which need a `BoxPrim`, are not made on skinned enemies.
  - A skinned palette holds the bones that weight vertices (Assimp's list: the enemy's 41 of 43
    joints), not every joint.
  - **The player's arms** (step 3): the play scene's `Gun` is the `ViewModel` root at the eye; its
    child `Arms` (`assets/characters/fps_arms/`, ours, `tools/anim/make_fps_arms.py` in Blender 5.2.2)
    is skinned and driven by `arms` (`ecs/arms_anim.cpp`, from the owning player's saved state and
    the step's combat events); the `Muzzle` is a `JointAttach` on its `gun` joint. `view_model` places
    the root and skips `JointAttach` children (the pose system draws them after it). A view-model rig
    is drawn as through `ViewModel.fovDeg` and pulled to 0.41 of its depth: author what should show
    at least 0.25 m from the eye and within 30° of the eye line, or the near clip and the screen's
    edge eat it. A clip's `rateFrom: {param, at}` lands its event `at` `param` seconds in (the reload's
    `mag_in` on the weapon's `reloaded`, within a step).
  - **ADS** (provisional ruling 11): `Weapon.aim` (action) and `adsTime`, `WeaponState.aim` (saved,
    stepped before the round; 0 while reloading or dead); the cone is `× (1 − 0.5 · aim)`. The narrowed
    view is a runtime `DrawnFov` on the camera, written by `view_model` in live frames only:
    `resolveSceneView` prefers it, so **lockstep frames and every capture keep `Camera.fovDeg`**.
- **Audio** (sub-project 11, `docs/specs/2026-10-28-audio-design.md`). A game's sound is one
  `.audio.json` bank (`ohao/audio/audio_bank.*`: buses as a tree with caps and a return bus, sounds,
  shapes, ducks, snapshots, reverb presets; every error at its JSON pointer; `ohao schema
  audio-bank` exports its schema), named by the saved singleton `AudioMix { asset }`; the play scene
  names `assets/audio/slice.audio.json`. Without one, the default bank: master over `sfx`, `music`,
  `ambient`.
  - **A bus is an `ma_sound_group` feeding an `AudioBusNode` of ours:** gain linear in dB and
    ramped, a low-pass ramped in log frequency, a 2 ms mute/solo gate, a send. The old
    `SoundCategory` API is a name for a default-bank bus: a category volume is its group's volume,
    master the root's.
  - **Bank voices never use miniaudio's start time or stop fade.** In 0.11.21 a start inside a
    block lands at (block end − start), and `stop_with_fade` cut at once. `AudioVoiceSource` plays
    silence until `startFrame` and fades a stop itself.
  - **Voices and groups are made with `MA_SOUND_FLAG_NO_PITCH`:** the pitch resampler delays a
    voice by a frame per node. Nothing may set pitch on the bank's path.
  - `AudioSystem::initialize(true)` is a mixer without a device; `render(out, frames)` pulls frames
    from it, the same bytes every run. Test the mix that way, never through a device.
  - **Teardown cuts every node link before any node goes.** In 0.11.21 `ma_node_uninit` detaches
    only the first node feeding each input; the others stay linked to the freed node (the return's
    group, with a sender per bus: `audio_bench` faulted in teardown one run in ten). A node with
    several inputs is detached from them explicitly first (`destroyBuses`).
  - **The reverb** (step 2, `ohao/audio/reverb_node.*`: Freeverb, ours, 8 combs and 4 allpasses a
    channel) sits on the bank's first return bus, starting at its `defaultZone`;
    `AudioSystem::setReverb(preset, frames)` crossfades two cores. The bank parser is its own
    library, `ohao_audio_bank` (no miniaudio), which `ohao_ecs` links.
  - **Reverb zones are volumes, not bodies.** `ReverbZone { preset, priority, size }` (saved): half
    extents about the entity, 0 fitted to its mesh. Physics never sees a scene zone, so it adds no
    contacts or trigger events; a level node's `verb` hint makes a `ReverbZone` on a trigger box. An
    unknown preset is `W_AUDIO_PRESET` and counts as the bank's `defaultZone`.
  - **The schedule** (`ecs/audio_schedule.hpp`, `installAudio(world, assets)`, which the CLI context
    and `LiveHost` call after `installAnimation`) is the step's last simulate system. From step 2 it
    writes `zone` commands (the listener's zone changed: preset, bed, a 30-step blend) and
    `occlusion` commands (a held voice's wall count changed on a re-cast every 6 steps). Occlusion
    is graded by walls: none; −8 dB, 1.2 kHz; −14 dB, 700 Hz for two or more. At most 16 rays a
    step: past them a new voice counts as unoccluded, a re-cast keeps its answer, both counted. The
    listener is the input-driven player's head, else the first active camera.
  - **The schedule decides every sound in the step** (step 3): footsteps and landings from the
    physics log, combat sounds from the combat log after the step's last `restart`, `mag_out`,
    `mag_in`, `bolt`, `body_fall` and `foot_l`/`foot_r` from the animation log (a character whose
    graph has foot events steps from its clips, never from the physics footstep). Each `play` has
    a voice id, a variant (round robin per sound), its occlusion and an outcome: over a bus's cap or
    the bank's `voices`, the lowest priority, then the quietest at the listener, then the oldest is
    stopped (`stop` first), or the new one is `dropped`. A sound holds its voice `holdMs` (the
    bank's, OPEN 13). Ducks (`bus` commands, the deepest rule a bus wins, attack down, release up)
    and the snapshot from the round (`default`/`dead`/`won`, OPEN 9) are commands too. A restart or
    any restore writes `stop_world`; after a restore the schedule's state comes back with the
    World's snapshot (its `SnapshotHook` chains in front of physics'), and it resends the mix as
    `resync` commands, which a player ignores when it is already there. `world.hash` never sees any
    of it: the schedule is runtime.
  - **The player** (`runtime/audio_player.hpp`): a window's `LiveHost` owns one (`LiveConfig::sound`;
    `ohao_game --mute` builds the schedule and creates no device) and plays the schedule's commands
    once a frame, one step after their step (OPEN 7: every sound, the player's own shot too), from a
    clock anchored to the device (late: at once, counted; re-anchored when not realtime, when the
    step goes back, or past a step of drift). A bed is the zone preset's looped sound. Headless
    hosts have no player; the schedule's commands are what they read. `OHAO_AUDIO_TRACE=1` logs every
    command on the `audio` channel (`schedule: ...`), so `check_live` compares a window's schedule
    with a headless host's through `log.read`.
  - Sound shapes, curves and occlusion are the bank's (`shapes`, `occlude`), never code's: the
    slice's gunfire is 1–60 m, rolloff 1; near sounds 0.5–20 m, rolloff 1.5.
  - **Agents** (step 4, `ohao/api/audio.cpp`) hear through `audio.info {since, max}`: the
    schedule's half (snapshot, zone, listener, buses with their duck, voice and ray counts, the
    commands after a cursor) everywhere, `device: "none"` headless; a window's `api::Host` adds
    the device's half (sample rate, each bus's driven level and voices, `late`, `mixMs`). It
    stays small: a bus's `db` only when not 0, a play's variant, gain, occlusion and outcome
    only when not the default. `audio.mix {bus, db, mute, solo, snapshot, clear}` is for
    listening: the session keeps the overrides and hands them whole to the host
    (`Host::setAudioMix`), whose `AudioPlayer` applies them in the device and again when a bank
    loads. **Never put them in the World, the schedule or a snapshot**: the schedule's commands,
    `world.hash` and replays are the same with or without them. `ohao serve --window --sound`
    opens a device (a serve window is mute without it); `render.stats` `audio` (`mixMs`, the
    median time the device callback takes per 10 ms of audio; `voices`; `late`) is there only
    when a player has a device.
- **VFX** (sub-project 12, `docs/specs/2026-10-28-vfx-design.md`; steps 1–3 built, step 4 in progress). A world's
  effects are one `.vfx.json` set (`ohao/vfx/vfx_set.*`, errors at JSON pointers, `ohao schema
  vfx-set`), named by the saved singleton `VfxSet`; without one, the default set. `installVfx` (the
  CLI context and `LiveHost`, after `installAudio`) adds the `vfx` simulate system before
  `audio_schedule`: it writes **spawn records** (effect, tracer, decal, light) from the step's
  `shot`/`hit`/`explode` after its last `restart` and from placed `VfxEmitter`s by step count,
  seeded from the vfx step and the event's index, capped by kind (the oldest dropped, counted).
  **Records are runtime: never saved or hashed**; snapshots carry them (its `SnapshotHook` chains in
  front of the schedule's) and a restart puts back the round start's. Particles are a closed form
  of (record, index, age) (`vfx/particle_math.hpp`), never integrated. Agents read them through
  `vfx.info {since, max}`.
  - **The frame** (step 2, `render/vfx/vfx_renderer.*`): the `vfx_draw` sync system (`ecs/vfx_draw.*`,
    after `render_extract`; the CLI's render host and `LiveHost` install it) hands the RenderScene the
    live effect records with their age, `(steps - spawnStep - 1 + alpha) * dt` (`RenderScene::setVfx`,
    `render/scene/vfx_frame.hpp`). `vfx.expand` (after the sky) evaluates every particle slot in
    closed form, as `vfx/particle_math.hpp` does, and lights a `lit` one at its centre: the clustered
    lights through its froxel (every light when it is off screen), one shadow ray to the first
    directional light, RT GI's radiance at its pixel, else the environment's irradiance, else the
    constant ambient. `vfx.sort` orders each alpha batch (<= `kVfxSortMax`) back to front by integer
    keys (distance bits, slot). `transparent` (after `ssr`, before `post.bloom`) draws every blended
    mesh and effect batch back to front, ordered on the host by distance, then kind, then id, each
    with its own blend state (additive, alpha, premultiplied), with the G-buffer's jittered
    projection, soft against its depth, writing coverage to an R8 mask TAA lowers its history weight
    under. Blended meshes reuse `core/gbuffer.vert` and are shaded by `vfx/transparent_mesh.slang`,
    the forward copy of the deferred light loop (`includes/lighting/clustered_lights.slang`; a plane
    forward-shaded equals the deferred one within 1 LSB, `vfx_gpu_test`). The pass needs ray queries
    and the bindless textures; without them nothing transparent draws.
  - **Gotchas:** particle slots are fixed in record order (no compaction; a dead one has size 0), and
    the slots are one buffer shared by the frames in flight (expand waits for the previous frame's
    draws). Slang decorates an unsized resource array with an `ArrayStride` spirv-val rejects
    (VUID-StandaloneSpirv-None-10684): declare the bindless table at its size, `Sampler2D t[4096]`. A
    closed blended mesh draws its far faces over its near ones (no depth write between blended
    faces): a pane is a quad. The G-buffer's normal target is `R16G16B16A16_SFLOAT` (attachment 1;
    recorded for step 3's decals, OPEN ruling 16).
  - **Smoke in RT** (step 4): a smoke record publishes closed-form proxy spheres (`ohao/vfx/smoke_math.hpp`,
    `caps.proxies` at most, 256 uploaded). They reach the RT traces through set 1 binding 7
    (`rt/smoke_proxy.slang`; the header's flag is `render.settings vfx.smokeInGi`): `shadow_trace` draws
    its visibility stochastically with `T`, `gi_trace` multiplies radiance by `T` and adds nothing (OPEN
    ruling 29: a flat in-scatter lit a filled doorway brighter, not darker). With no spheres `T` is exactly 1,
    so a scene without smoke is bit-identical. A sphere is
    the emitter's pose plus up to 0.6 R of offset: a cloud placed on a wall plane is half outside it.
  - **Decals are raster only** (no RT decal); tracers and particles are sorted back to front by integer
    keys (distance bits, then id); the transparent pass runs after SSR, so SSR never sees a particle.
  - **A GPU test that loads a level must install what the CLI installs** (`level::installLevels`, then
    `installPhysics`, as `tools/ohao/main.cpp` does). Without it the blockout is never expanded, the
    scene has no instances, `scene.upload` builds no TLAS, and RT shadows and RT GI silently do not run
    (the frame is the sky, the passes list has no `rt.gi`). `vfx_gpu_test`'s `capture()` does this.
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

The monograph is frozen during the frontier renderer work (Frank, 2026-10-30): its citations
trail the code, so its checks are in no gate (CTest label `monograph`, run by
`python tools/check.py --labels monograph`) and `check_docs` does not run these:

```bash
python site/tools/check_unit.py reflection/api
python site/tests/test_monograph_structure.py
cd site && python -c "from tools.monograph.build import build; build()"
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
