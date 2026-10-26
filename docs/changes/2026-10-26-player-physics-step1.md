# Player and physics, step 1: physics in the fixed step, transform sync, events

**Spec:** `docs/specs/2026-10-26-player-physics-design.md` ("The fixed step", "Physics ↔ transform
sync", "Events", "The agent-facing surface", "Performance budgets", "Test plan" step 1, and "As
built (step 1)"); plan `docs/plans/2026-10-26-player-physics.md`, Task 1.

**Status:** built and tested. `tools/check.py --all` 42/47: every functional suite, the 25 render goldens and the docs pass; the five benches (`scene_bench`, `serve_bench`, `level_bench`, `physics_bench`, `render_bench`) were noisy, pending a quiet rerun (Frank's ruling).

## 1. Summary

Sub-project 6 starts. Its spec and plan are new (the roadmap's row 6 had none); this step makes
physics part of the world:
- **One clock.** `installPhysics` adds a `physics` simulate system, so every
  `World::simulate(dt)` steps Jolt: the window's fixed step, `world.step`, `ohao run --steps`.
  Dropped crates fall in a plain `ohao run`; the `--drop` helper's hand-stepping is no longer how
  anything moves.
- **Physics → transforms.** Every awake dynamic body writes its pose back to its entity's
  `Transform`, so the renderer, `world.query`, `world.hash` and agents see it. A settled world
  writes nothing.
- **Events, first-class** (Frank's ruling): `contact_begin`/`contact_end` per body pair and
  `trigger_enter`/`trigger_exit` for trigger colliders, read by ECS systems
  (`ecs::physicsEvents(world)`), tests, and agents (`physics.events`, a cursor over a log of
  1,024).

## 2. What was built

**The step** (`ecs/physics_scene.{hpp,cpp}`, `PhysicsScene::step`):
1. `propagateTransforms` (new, `ecs/transform_system`): dirty subtrees recomputed inside a step;
   an entity's `prevWorld` is captured once per frame and kept by later steps and by the
   frame's `updateTransforms`, so motion vectors still span the frame.
2. `PhysicsScene::sync` (SP5's), extended: a dynamic body whose entity moved for any reason but
   the write-back (compared with the pose it wrote, 1e-4) jumps there with zero velocity and
   wakes.
3. Jolt in `ceil(dt·60)` substeps (at most 8).
4. Write-back of every dynamic body awake now or at the end of the last step, through the
   parent's world transform (`registry.patch<Transform>`); `W_PHYSICS_PARENT_SCALE` (new code)
   under an unevenly scaled parent.
5. Events: the backend's queue (Jolt callbacks append under a mutex, possibly from job threads)
   drained on the simulating thread; persists dropped; sub-shape contacts counted per body
   pair (0→1 begin, 1→0 end); sorted (begins before ends, then by ids); numbered and logged.

A world without any collider skips all of it.

**Sleep:** Jolt removes a sleeping body's contacts and adds them again on wake. The scene keeps
such pairs dormant (no end, no second begin) and ends them only when the bodies wake apart.
Removing a body (destroy, rebuild, `level.reload`) ends its pairs at once; they are published
first in the next step.

**Triggers:** `Collider.trigger` (saved, default false) builds a Jolt sensor
(`BodyCreationInfo.isSensor`, new); the level hint `"trig": 1` sets it (`ohao schema
level-extras` lists it).

**Jolt settings:** penetration slop 5 mm (was Jolt's 2 cm; dropped crates rested visibly in the
floor).

**API** (`api/level.cpp`): `physics.events {since = 0, max = 32 (1–64)}` →
`{events: [{seq, step, kind, a, b, an, bn, point, normal, speed}], next, dropped?}`;
`physics.info` gains `steps`, `awake`, `contacts`, `triggers?` and loses `unstepped`. 29
methods; `tools/list` 20,260 bytes.

**Bench:** `physics_bench` (new, `tests/bench/physics_budgets.json`, label `bench`):
`blockout_step_ms`, `blockout_rest_step_ms` (with zero writes), `events_1k_ms` gated from step 1;
`player_step_ms` (2), `boxes1k_step_ms` (5) pending. `PhysicsScene::lastStepTiming()` splits a
step into Jolt, events and write-back.

**Tests:** `level_physics_tests` test 10 (10 new: drop and rest, begin events at the right step
and speed, rest writes nothing, edit teleports, two steps in one frame, destroy ends contacts,
trigger enter/exit, substeps, two runs identical, the log cursor); `api_tests` (4 new
`sp6_t11_*`); serve session `physics.jsonl`; CLI case `run_drop`; an `e2e_mcp` leg (lift, step,
the landing event, the resting `Transform`).

**Docs:** the spec and plan (new), ROADMAP row 6, AGENTS.md ("Physics in the step", suites,
benches), ARCHITECTURE's data flow, the CLI guide (`physics.events`, `physics.info`, `Collider`,
`RigidBody`), the serve guide ("Drop something", a run script `docs/agents/examples/drop.jsonl`),
the monograph's transform-system unit.

## 3. Bugs found and fixed

- **Contacts ended when bodies slept.** The first run of `t10_destroy_ends_contacts` found
  resting crates "not in contact" and `t10_contact_begin_events` saw ends at rest: Jolt documents
  that a sleeping body's contacts are reported removed. Fixed with dormant pairs (section 2).
- **Crates rested 2 cm into the floor** after a drop (Jolt's default penetration slop). Now 5 mm;
  `physics_backend_tests` 61/61 unchanged.
- **`-0` in API numbers.** Under `/fp:fast`, `r == 0 ? 0 : r` folds to `r`, so a rounded `-0`
  kept its sign (`"normal":[-0,-1,0]`). The rounding helpers in `api/level.cpp` now test the
  magnitude; `api_tests` asserts no `-0` in events.
- **My own tests:** an `e2e_mcp` leg lifted only CrateA, onto CrateB (which then fell under it);
  it now lifts both. A rest tolerance did not allow two stacked slops (1.5 cm for the top crate).

## 4. Spec corrections and rulings

- Event `step` is the physics scene's own step count (it outlives `scene.load`), not the world
  frame; agents read `physics.info.steps` first (spec "As built").
- Sleep keeps contacts by design, implemented over Jolt's removal (above).
- `physics.info.contacts` and `triggers` added; `unstepped` removed (forward only).
- Kinematic bodies still teleport; `MoveKinematic` moves to step 2 with the player.
- The `RigidBody` and `Motion` docs no longer say "not stepped"; the Kinematic doc no longer
  claims pushing (step 2).

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | 42/47: every functional suite; the five benches noisy (other processes 1.3–2.3 cores) |
| `level_physics_tests` | 24/24 (10 new) |
| `api_tests` | 55/55 (4 new) |
| `ecs_tests` / `scene_tests` / `reflect_tests` | 37/37 / 84/84 / 42/42 |
| `engine_tests` / `physics_backend_tests` / `systems_tests` | 85/85 / 61/61 / 9/9 |
| `check_cli` / `check_serve` / `e2e_mcp` | 33/33 (29/29 CPU) / 198/198 / 33/33 |
| render goldens | 25/25, every one unchanged |
| `check_docs` | 81/81 (`check_live` 42/42, `check_validation` 6/6) |

**The drop** (blockout, CrateA lifted 1 m, CrateB 2 m, 1/60 s steps): CrateA lands on the Floor
in step 28 at 4.57 m/s (free fall: 0.45 s, 4.43 m/s), CrateB on CrateA in step 39 at 6.19 m/s
(0.64 s, 6.26 m/s); both asleep by step 90 at y 0.2950 and 0.7900 (authored 0.3 and 0.8; 5 mm
slop per contact). Two runs: identical events and poses, bit for bit. A step of 0.05 s runs 3
updates and lands where 1/60 s steps do (identical to 1e-5).

**Benchmarks** (`physics_bench`): every gated budget met in both runs, both flagged noisy by the machine (Unreal Editor and other processes 0.9–1.4 cores; the first run's calibration x1.98).
- `blockout_step_ms` (a step while the crates fall and land, 900 steps): median 0.0149 ms, p95 0.0316 (gate run); 0.0273 / 0.0402 (standalone). Budget 0.25.
- `blockout_rest_step_ms` (settled, 200 steps): median 0.0035 ms, p95 0.0043, 0 Transform writes (gate run); 0.0024 / 0.0024 (standalone). Budget 0.02 and zero writes.
- `events_1k_ms` (1,000 begins in one step: drain, collapse, sort, log): median 0.2075 ms, p95 0.25 (gate run); 0.3846 / 0.7357 (standalone). Budget 0.5.
- The other benches, noisy, pending a quiet rerun: `scene_bench` met every budget (`save_pretty` 9.441 ms clean rounds against 10; `apply_100_field_set` 0.181 against 0.2); `serve_bench` every printed measurement inside its budget; `level_bench` `blockout_load_ms` median 8.723 ms (budget 20); `render_bench` `cornell_720p lighting` 0.375 ms against 0.32, the standing miss of SP5 step 4 (this step touches no renderer code).

**Goldens re-baselined, each looked at:** the engine and fixture catalogues and scene schemas
(`Collider.trigger`, `W_PHYSICS_PARENT_SCALE`, the `RigidBody` and `Motion` docs), the hint schema
(`trig`), the CLI and serve expectations (schema hash `e9e1c34d831dc812` → `6ada12af3219dfec`; the
`physics.info` shape; the level session's `world.hash`, which hashes Collider's fields, with
`level.reload` still keeping it; one more method in an `E_METHOD` hint), and the new `run_drop`
and `physics.jsonl`. No render golden changed (nothing they render steps a collider).

**Look shots** in `C:\Users\djmax\ohao_refs\look_shots\sp6_player\` (real `ohao run`s of the
blockout scene; a script lifts the crates, steps N frames, renders deferred at 1280x720):
`drop_000.png` (lifted), `drop_022.png` (falling), `drop_032.png` (CrateA landed, CrateB in the
air), `drop_045.png` (both landed), `drop_150.png` (at rest), `drop_strip.png` (the five), and
`spawn_view.png` (from the player spawn at eye height 1.62 m, facing −Z, fov 75°: what step 3's
first-person camera will see).

## 6. Known gaps

- Kinematic bodies teleport (no velocity: they do not push); step 2.
- No render interpolation: a frame shows the last step's pose (step 3).
- Velocities are not in snapshots: a restore puts dynamic bodies at rest (step 5).
- Events are polled; no MCP notification while a long `world.step` runs (an open decision).

## 7. How to run

```
build/Release/level_physics_tests.exe
build/Release/api_tests.exe
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
python tests/serve/check_serve.py
python tests/cli/check_cli.py --cpu-only
python tests/serve/e2e_mcp.py
build/Release/ohao.exe run assets/scenes/blockout_room.scene.json --script docs/agents/examples/drop.jsonl
```
