# Player and physics, step 5: agent surface and replay

**Spec:** `docs/specs/2026-10-26-player-physics-design.md` ("The agent-facing surface",
"Snapshots and replay", "Performance budgets" and "As built (step 5)"); plan
`docs/plans/2026-10-26-player-physics.md`, Task 5. Sub-project 6 is done with this step.

**Status:** built and tested.
- `tools/check.py --all`: every functional suite passed except `check_docs`.
  - `check_docs` failed 81/83 on two stale expectations: the CLI guide's tool count (29 → 32) and
    the serve guide's walk, whose feet now read y 0 instead of -4.1e-08 (the character is grounded
    from creation). Both were updated and rerun: 83/83.
- The 26 render goldens are unchanged.
- The five benches were flagged noisy: calibration was x1.00–1.02, but other processes used 1.6–2.2
  cores.

## 1. Summary

An agent can walk the level as a player would and replay what it did:
- **Reading the player.** `player.info` gives its feet, facing, look, velocity, whether it stands
  on the ground, on what and on which surface, and its footsteps.
- **Placing it.** `player.teleport` puts its feet somewhere at rest, grounded at once.
- **Contacts.** `physics.contacts` lists the body pairs touching now.
- **Exact replay.** A snapshot carries the physics simulation, so a restore followed by the same
  input reaches the same `world.hash`. This holds mid-fall, mid-jump, from a loaded spawn, and
  with crates resting on each other.

The done demo:
- **Over MCP.** A client walked the blockout in a hidden window: turned by the mouse, walked the
  room, climbed the Step and Platform, jumped, teleported and climbed the Ramp. It looked
  through the player's eyes at every stop.
- **In `ohao_game`.** The play scene ran 300 frames with validation clean.

## 2. What was built

**API (`ohao/api/level.cpp`, `params.*`, `session.cpp`)**
- `player.info {entity?}` → `{entity, name, pos, yawDeg, pitchDeg, velocity, ground, on?,
  surface?, footsteps}`, about 170 bytes.
  - `E_STATE` without a player or without physics.
  - `E_NOT_FOUND` for an entity that is not a player.
  - `E_COMMAND` for two players and no `entity`.
- `player.teleport {entity?, pos, yawDeg?}` → as `player.info`.
  - `pos` within ±100 km on each axis, `yawDeg` within ±360 (`E_RANGE`).
  - It changes the world (`changeCount`).
- `physics.contacts {entity?, max = 32}` → `{contacts: [{a, b, an, bn, trigger?, point, normal,
  since}], count, more?}`.
  - `max` is 1–64.
  - `E_NOT_FOUND` for an entity without a body.
- `scene.load` now runs `player_spawn` after `level`, so the world it leaves is the spawned one.
- `tools/list`: 32 tools, 21,867 bytes (the cap is 24 KB).

**Physics scene (`ohao/ecs/physics_scene.*`)**
- `PhysicsScene::contacts()`, `players()`, `playerInfo(e)` and `teleportPlayer(e, pos, yaw)`.
- `PlayerRecord` gains `ground`, `footsteps` and `placed`.
- Contact pairs gain `since`, the step they began in.

**`BodyMotion` (runtime, snapshotted)**
- `{linear, angular, asleep}`, registered with docs and units. `rad/s` joins the unit
  vocabulary.
- Written:
  - for every dynamic body awake in a step, and once more as it falls asleep;
  - for each player whose velocity changed.
- Snapshots hold it as a column. `world.hash` and `world.diff` leave it out
  (`snapshotTypes(types, false)`, `Snapshot::motionColumn`).
- A write by anything but the scene (a restore) goes into the body after its teleport, or into
  the player.

**Simulation in snapshots (`ecs/snapshot_hook.hpp`)**
- `ecs::SnapshotHook` lives in the registry's context.
  - `takeSnapshot` keeps what its `save` returns (`Snapshot::systems`).
  - `restoreSnapshot` hands it back after the components.
- `installPhysics` installs the physics scene's hook.
  - `saveSimulation` saves the backend's `saveState()`: Jolt `SaveState(All)` (bodies with sleep
    timers, the contact cache, constraints, gravity), then each `CharacterVirtual`.
  - It also saves which body is whose, the players' records, and the contact pairs with their
    dormant count.
- The next sync after a restore puts it back, after rebuilds and before teleports, provided the
  bodies are the snapshot's: the same handles for the same entities, and the same players.
  - A body whose entity the restore put where the restored body already is gets no teleport.
  - Otherwise (a body rebuilt since) the teleports and `BodyMotion` apply.

**Backend (`ohao/physics/backend`)**
- `setPose(h, pos, rot)`: one `SetPositionAndRotation`.
- `saveState()` and `restoreState(state)`. Restore refuses a different character set before
  changing anything.
- `setCharacterPosition` and character creation call `RefreshContacts`.

**Tests**
- `level_physics_tests` test 14, nine cases:
  - `BodyMotion` follows awake bodies;
  - `BodyMotion` is neither saved nor hashed;
  - restore mid-fall is exact;
  - restore puts bodies back to sleep;
  - restore mid-jump is exact;
  - restore from the loaded spawn replays a walk;
  - a body is put back however close it settled;
  - resting contacts replay exactly;
  - a body rebuilt since the snapshot falls back to `BodyMotion`.
- `api_tests`, five `sp6_t14_*` cases: player.info, player.teleport, physics.contacts, restore
  mid-fall, and a restored walk.
- `tests/serve/sessions/player.jsonl`: a serve session golden.
- `check_live.py` (sp6 test 14): a hidden-window walk. The same walk headless gives the same
  place, hash and picture.
- `e2e_mcp.py`: a walking leg. Load the play scene, look, snapshot, walk, footsteps, teleport,
  contacts, restore, the same walk to the same hash and picture.

**Bench (`tools/bench/physics_bench.cpp`)**
- `boxes1k_step_ms`, gated at 4 ms from step 5 (`currentStep` 5): 1,000 0.4 m boxes fall and pile
  into a walled 6 × 6 m pit, and none may leave it.
- `boxes1k_snapshot_ms` is reported without a budget.

## 3. Bugs found and fixed

- **Restore without the spawn.** A snapshot taken right after `scene.load` held the player's
  authored `Transform`, because the spawn ran at the first sync. A restore therefore teleported
  the player to (0, 0, 0.1). Fix: `scene.load` spawns players.
- **Restores inside the write-back tolerance were ignored.**
  - A settled crate sinks 4.2 µm below its snapshot pose. The restore wrote the snapshot's
    `Transform`, but the difference was inside the 1e-4 tolerance the scene used to recognise
    its own write-back. The body stayed asleep where it was.
  - Fix: the scene marks its own `Transform` writes (`writePose`) and treats every other write as
    a placement. Test: `t14_restore_puts_a_body_back_however_close`.
- **A placed character's stale contacts.** A new or teleported character kept the contacts of
  where it had been until its next move. As a result, `player.info` after a load said it was in
  the air, and a replay's first step differed. Fix: `RefreshContacts` on creation and teleport,
  and again once the level's bodies exist for a player placed before them.
- **Warm-start impulses.** `BodyMotion` alone left resting contacts replaying 1.9–3.8·10⁻⁵ m off.
  - Jolt copies each contact's impulse from the previous step's cached manifold when its points
    are within 1 cm, across teleports. `ContactConstraintManager.cpp` reads `ccp->mNonPenetrationLambda`.
  - The guide's example, a 30-step walk and jump from a spawn snapshot, then missed its hash.
  - Fix: snapshots carry the simulation (section 2).
- **Teleports set position and rotation separately.** `SetRotation` re-derives the position
  through the centre of mass. They are now one call (`setPose`).

## 4. Spec corrections and rulings

These are in the spec's "As built (step 5)":
- **`player.info`'s `steps` is `footsteps`.** "steps" would read as physics steps. It also gains
  `on`, the body stood on.
- **Both player methods take `entity?`.** With two players and none named, they answer
  `E_COMMAND`, as `level.info` does with two levels.
- **Snapshots carry the backend's whole state, not only `BodyMotion`.** The spec's `BodyMotion` is
  built as written and is the fallback when bodies were rebuilt. The contact cache belongs to
  pairs, not entities, so it travels through a hook.
- **The id seed.** Without `--seed`, a load seeds ids at random and `world.hash` covers them.
  Replays are compared within a session or with a seed. This is unchanged, and now stated in the
  serve guide.

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | functional suites green after the two doc expectations; `render_golden` 26/26 unchanged; benches noisy |
| `level_physics_tests` | 53/53 (9 new, test 14) |
| `api_tests` | 61/61 (5 new) |
| `scene_tests` / `reflect_tests` / `runtime_tests` | 84/84 / 42/42 / 32/32 |
| `ecs_tests` / `engine_tests` / `physics_backend_tests` | 37/37 / 85/85 / 61/61 |
| `check_serve` / `check_cli` / `check_docs` | 212/212 / 33/33 / 83/83 |
| `check_live` / `e2e_mcp` | 49/49 (7 new) / 45/45 (12 new) |
| `restore_render_gpu_test` / `deferred_determinism_gpu_test` | 14/14 / 65/65 |

In the gate's bench runs (all noisy):
- `scene_bench` `save_pretty` 9.278 ms against 10;
- `serve_bench` `world_apply_100_rpc` 0.105 ms against 0.4;
- `level_bench` `blockout_load_ms` 9.18 ms against 20;
- `render_bench` Cornell lighting 0.372 ms against 0.32 (the standing miss).

**Replay** (`level_physics_tests` test 14, `api_tests`, `ohao run`):

| Case | Before this step | Now |
|---|---|---|
| Crates restored mid-fall, 60 steps | at rest, then fall late | bit for bit |
| Player restored mid-jump, 50 steps | velocity lost | bit for bit |
| Spawn snapshot, walk, restore, walk | player at (0, 0, 0.1) | same hash |
| Crates resting on each other, restored after 1 step | — | bit for bit (BodyMotion alone: 1.9e-5 / 3.1e-5 m off) |
| Guide example, `--seed 0000000000000001` | — | `10f1fdd1b04ebbed` both runs |

**Benchmarks** (`physics_bench`; calibration x1.01, but other processes used 1.6 cores, so
noisy):

| Budget | Median | p95 | Limit |
|---|---|---|---|
| `blockout_step_ms` | 0.0147 ms | 0.0245 ms | 0.25 |
| `blockout_rest_step_ms` | 0.0013 ms | 0.0013 ms | 0.02 |
| `events_1k_ms` | 0.317 ms | 0.416 ms | 0.5 |
| `player_step_ms` | 0.0030 ms | 0.0066 ms | 0.1 |
| `boxes1k_step_ms` (new, gated) | 2.120 ms | 2.743 ms | 4 |
| `boxes1k_snapshot_ms` (reported) | 0.499 ms | — | — |

- The first `boxes1k_step_ms` run, at calibration x1.86 with Unreal busy, gave 2.487 ms (p95
  4.059).
- `events_1k_ms` at 0.317 ms resolves step 4's noisy 0.53 ms.

**The done demo over MCP** (`build/sp6/mcp_walk.py`, `ohao serve --window 1280x720 --hidden`):

| Stop | `player_info` |
|---|---|
| 00 spawn | (1.5, 0, 2.2), yaw 0, on Floor (concrete) |
| 01 a quarter turn left by the mouse | yaw 90 |
| 02 along the south wall, turned north | (-0.83, 0, 2.2), 3 footsteps |
| 03 up the lane | (-0.83, 0, -1.67), 8 footsteps |
| 04 onto the Step | (-1.71, 0.3, -1.67), on Step (metal) |
| 05 on the Platform, turned round, looking down 10° | (-3.41, 0.6, -1.67), on Platform (metal) |
| 06 jumping | y 1.52, not on the ground |
| 07 teleported to the Ramp's foot, walked up | (2.7, 0.29, -0.94), on Ramp (metal) |

- The walk made 14 footsteps and 1 landing.
- The server exited 0.

**The demo in `ohao_game`:** `assets/scenes/blockout_play.scene.json --frames 300 --mute`,
`OHAO_VALIDATION=1`. It rendered 300 frames with 0 validation messages through teardown and 0
unrequested readbacks.

**Goldens, each diff looked at:**
- **Catalogue and scene schema:** `BodyMotion` with its docs and units, and `rad/s` in the unit
  list. The reflect fixture catalogue changes by the unit.
- **Serve and CLI expectations:** only the schema hash, `94d642166fb6b4e2` → `568c4633526fa55a`.
- **New:** `tests/serve/expected/player.jsonl`.
- **Unchanged:**
  - every `world.hash` in the existing sessions;
  - the physics session's events;
  - the play-scene render golden.

**Look shots** in `C:\Users\djmax\ohao_refs\look_shots\sp6_player\`:
- `step5_walk_00.png` … `step5_walk_07.png`: what the player saw at each stop above, from the
  MCP walk.
- `step5_walk_strip.png`: all eight, labelled.

## 6. Known gaps

- **Fallback restores.** A restore across a rebuilt body uses the fallback (teleport, then
  `BodyMotion`). That is exact for free motion, not for resting contacts.
- **Snapshot size.** `Snapshot::bytes()` does not count the saved simulation, about 0.5 ms to
  save for 1,000 bodies.
- **Not played by a person.** `ohao_game`'s play mode was run under validation, but nobody has
  walked it with a keyboard and mouse, or listened to the footsteps, in this step.
- **Levels and players.** Re-expanded levels still do not respawn the player (step 3's gap).
- **Standing gaps.** The RT GI fireflies at the doorway and the Cornell lighting budget miss
  remain.

## 7. How to run

```
build/Release/level_physics_tests.exe
build/Release/api_tests.exe
python tests/serve/check_serve.py
python tests/serve/check_live.py
python tests/serve/e2e_mcp.py
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
build/Release/ohao.exe run assets/scenes/blockout_play.scene.json --seed 0000000000000001 --script docs/agents/examples/player.jsonl
python build/sp6/mcp_walk.py
build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json
```
