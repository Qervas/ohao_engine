# Player and physics, step 2: the player

**Spec:** `docs/specs/2026-10-26-player-physics-design.md` ("The player (step 2)", "Performance
budgets", "Test plan" step 2, and "As built (step 2)"); plan `docs/plans/2026-10-26-player-physics.md`,
Task 2.

**Status:** built and tested. Every functional suite passes; the benches are noisy (other processes
0.5–2 cores, Unreal Editor among them), with every physics budget met. `tools/check.py` (quick, gpu, docs) 40/40;
`--all` 39/47 before the schema-hash goldens were updated (section 5).

## 1. Summary

A world can now hold a player: an entity with a `PlayerController` walks, climbs, jumps and pushes
crates on the actions it names, inside the same fixed step that moves every body. It spawns at the
level's `player` spawn, facing the spawn's yaw. Triggers and rays see it. Every number the spec
gives for the player holds in a test: 4.0 m in 60 steps, a 0.3 m step, the ramp, a 1.22 m jump.
Kinematic bodies now move by velocity, so they push what they meet.

## 2. What was built

**Components** (`ecs/components.hpp`, registered in `scene_types.cpp` with docs, units and
limits):
- `PlayerController` (saved): `radius` 0.3 m, `height` 1.8, `eyeHeight` 1.62, `walkSpeed` 4 m/s,
  `sprintSpeed` 6.5, `acceleration` 40 m/s2, `airControl` 0.3, `jumpSpeed` 4.9, `maxSlopeDeg` 45,
  `stepHeight` 0.35 (Frank's ruling), `mass` 80 kg, `pushForce` 200 N, `lookDegPerUnit` 0.1, and the
  action and axis names (`move_forward`, `move_right`, `jump`, `sprint`, `look_x`, `look_y`).
- `PlayerState` (runtime): `velocity`, `onGround`, `yawDeg`, `pitchDeg`, `character`.

**Backend** (`physics/backend/physics_backend.hpp`, `jolt/jolt_backend.*`):
- `CharacterCreationInfo` gains `positionAtFeet` and `innerBody`.
  - `positionAtFeet` sets Jolt's shape offset (0, half cylinder + r, 0) and a supporting plane
    at −r, so the position is the feet.
  - `innerBody` asks for a kinematic inner body on `CHARACTER`, registered as a body of its own so
    contacts, triggers and rays map to the entity.
- New calls:
  - `moveCharacter(h, dt, velocity, gravity, stepUp, stickDown)`: `ExtendedUpdate` with stair
    walking and stick-to-floor.
  - `getCharacterInnerBody(h)`.
  - `moveKinematic(h, pos, rot, dt)`: Jolt's `MoveKinematic`.
- The layer matrix no longer pairs `CHARACTER` with `DYNAMIC` or with itself.

**The step** (`ecs/physics_scene.cpp`): the players move after `sync` and before Jolt's substeps.
- **Look:** yaw −= (mouse x + `look_x`) · `lookDegPerUnit`; pitch is clamped to ±89°.
- **Horizontal motion:** the wanted velocity is taken in the yaw frame, at walk or sprint speed.
  The player accelerates toward it by `acceleration`, or by `airControl` of that in the air.
- **Jump and gravity:** a jump on the ground sets `vy` to `jumpSpeed`. The move takes the step's
  average vertical velocity, which makes a jump follow the exact parabola.
- **Write-back:** the feet and yaw are written to `Transform` with `registry.patch`, and the rest
  to `PlayerState`.
- **Teleports:** a `Transform` written by anything else teleports the character there, at rest.
- **Spawn:** at the first `SpawnPoint` tagged `player`, in creation order. Without one, the player
  starts where it is, with `W_PLAYER_NO_SPAWN`.
- **Edits:** an edited `PlayerController` rebuilds the character where the player stands.
- **Timing:** `StepTiming::playerMs`.

**Kinematic bodies:** one moved during a step goes there by `moveKinematic` and is stopped after the
step. One moved between steps teleports.

**API:**
- `physics.info` gains `players` (non-zero only).
- New warning codes: `W_PLAYER_NO_SPAWN`, `W_PLAYER_NO_CHARACTER` (49 codes).

**Bench:** `physics_bench` section 4, `player_step_ms`: a player walking forward and turning (look_x
6), 200 steps after 20, the step's player part. Gated from step 2 (`currentStep` 2, budget 0.1 ms).

**Tests:** `level_physics_tests` test 11 (10 new):
- spawn and yaw (a turned spawn, no spawn);
- 4 m in 60 steps, twice, bit for bit;
- the north wall;
- the Step and the Platform;
- the ramp;
- pushing CrateA;
- the jump;
- a trigger and a ray;
- an edited controller;
- kinematic bodies by velocity.

`scene_tests` round-trips a random `PlayerController` and checks the new types are complete.

**Docs:**
- AGENTS.md: the player in "Physics in the step", the suite row, `player_step_ms`.
- ARCHITECTURE's data flow.
- The CLI guide: the `PlayerController` row, `physics.info` `players?`, three warning rows,
  `W_PHYSICS_PARENT_SCALE` among them (missing since step 1).
- The serve guide's `world.hash` values.
- The spec's "As built (step 2)" and the plan's ticks.

## 3. Bugs found and fixed

- **Kinematic bodies did not push.** They teleported to their entity's pose, so a pusher entering a
  box moved it at 0 m/s. With `MoveKinematic` the box moves at 1.000 m/s
  (`t11_kinematic_bodies_move_by_velocity`).
- **Editing a `PlayerController` sent the player back to the spawn**, because the edit rebuilt the
  character through the spawn path. Found by `t11_an_edited_controller_keeps_the_player_where_it_is`
  (it failed with the player at (1.5, 0, 2.2)). The character is now rebuilt where the player
  stands, keeping its yaw and pitch, or taking the yaw of an edited rotation.
- **Unit `m/s^2`** is not in the unit table: `m/s2`.
- **My own tests:**
  - The crate test first checked at step 150, by which time the player had slid past the turned
    crate. It now checks at step 80, while the player is pushing.
  - The random scene world drew `PlayerController` values outside their limits (`E_RANGE` on
    load). They are now scaled into range.
- **Look shots:**
  - The first overview camera stood over the south wall, which hid the player.
  - The player rendered white: albedo ignores vertex colour (Frank's ruling), so the shots give it
    a `MaterialOverride`.

## 4. Spec corrections and rulings

These are in the spec's "As built (step 2)":
- **No separate `player` system:** the players move inside `physics`'s step, in the order the spec
  asks for.
- **`CHARACTER` no longer collides with `DYNAMIC`.** The virtual character pushes crates, limited
  by `pushForce`. The inner body is for triggers and rays.
- **Grounded** means `ON_GROUND` with vy < 0.1 m/s. The vertical motion is integrated exactly.
- **Spawn happens once, when the player is created.** A re-expanded level does not respawn it.
  This was the conservative choice; Frank may rule otherwise.
- **The player must be a root entity.**
- **Look is always on** until step 3 brings the window's mouse capture. `pitchDeg` waits in
  `PlayerState` for the head.
- **Rulings applied:** walk 4 m/s, jump 1.22 m and step height 0.35 m. Footsteps are step 4.

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` (first run) | 39/47: four golden mismatches (below) and five benches; `level_bench` passed |
| `tools/check.py` (rerun after the goldens) | 40/40 (quick, gpu, docs) |
| `level_physics_tests` | 34/34 (10 new) |
| `scene_tests` / `reflect_tests` / `ecs_tests` | 84/84 / 42/42 / 37/37 |
| `api_tests` / `physics_backend_tests` / `engine_tests` | 55/55 / 61/61 / 85/85 |
| `check_cli` / `check_serve` / `e2e_mcp` | 33/33 (29/29 CPU) / 198/198 / 33/33 |
| render goldens | 25/25, unchanged |
| `check_docs` | 114/114 (`check_live` 42/42, `check_validation` 6/6) |

**The player** (blockout, 1/60 s steps, injected input):
- **Spawn:** at (1.5000, 0.0000, 2.2000), facing −Z.
- **Walking:** 60 steps at speed: 4.0000 m.
- **The wall:** it stops at z −2.6800 (wall face −3.0, radius 0.3, Jolt's 2 cm character padding).
- **Climbing:**
  - on the Step, y 0.3000;
  - against WallWest on the Platform, at (−3.68, 0.60);
  - at z −1.0047 on the ramp, y 0.3073, where the surface is at 0.3009.
- **Pushing:** at step 80, CrateA has gone from z −1.700 to −2.651, with the player at −2.096.
- **Jumping:** the apex is 1.2238 m and the player lands at 0.0000.

**Benchmarks** (`physics_bench`, in the `--all` gate): every budget met. The run was flagged
noisy, with other processes at 0.51 cores and calibration x1.04.

| Budget | Median | p95 | Budget limit |
|---|---|---|---|
| `player_step_ms` | 0.0032 ms | 0.0069 ms | 0.1 |
| `blockout_step_ms` | 0.0172 ms | 0.0345 ms | 0.25 |
| `blockout_rest_step_ms` (0 writes) | 0.0014 ms | 0.0014 ms | 0.02 |
| `events_1k_ms` | 0.2101 ms | 0.325 ms | 0.5 |

The other benches were noisy and are pending a quiet rerun:
- `scene_bench` met its budgets: `save_pretty` 9.208 ms against 10, `apply_100_field_set` 0.192
  against 0.2.
- `serve_bench`: `world_apply_100_rpc` 0.1115 ms against 0.4; other processes were at 1.96 cores.
- `render_bench`: `cornell_720p lighting` 0.376 ms against 0.32, the standing miss since SP5
  step 4. This step touches no renderer code.
- `level_bench` passed.

**Goldens re-baselined, each looked at:**
- The engine and fixture catalogues and scene schemas: `PlayerController`, `PlayerState`, the two
  codes and the `Motion` docs.
- The CLI and serve expectations: schema hash `6ada12af3219dfec` → `36655761f296b8a0`, from the new
  types and `physics.info`'s `players?`. Also every `world.hash`, which walks every saved type, so a
  new saved type changes them all. Nothing else in those files changed.
- The serve guide's two quoted hashes.

No render golden changed.

**Look shots** are in `C:\Users\djmax\ohao_refs\look_shots\sp6_player\`. Each is a real `ohao run`
of the blockout with a red 0.6 × 1.8 m player box and a yellow nose showing its facing, seen by an
overview camera 9.5 m up looking north. Input is injected (forward, a turn left, a turn back, a
jump); the shots are rendered deferred at 1280x720.

| File | Step | Player |
|---|---|---|
| `step2_walk_000.png` | 0 | the spawn |
| `step2_walk_030.png` | 30 | walking north |
| `step2_walk_060.png` | 60 | turned west, at (−0.50, 0, 0.14), yaw 90 |
| `step2_walk_090.png` | 90 | turning north |
| `step2_walk_120.png` | 120 | on the Step, at (−2.67, 0.30, −1.74) |
| `step2_walk_145.png` | 145 | the jump, rising |
| `step2_walk_160.png` | 160 | near the top, y 1.39 |
| `step2_walk_200.png` | 200 | landed at the north wall, (−2.67, 0.30, −2.68) |
| `step2_walk_strip.png` | — | the eight shots |

## 6. Known gaps

- No head or camera yet: pitch is stored but nothing uses it (step 3). There is no render
  interpolation either (step 3).
- Mouse look is always on, with no capture or Esc (step 3).
- No footsteps (step 4).
- A re-expanded level does not respawn the player (a ruling for Frank if a game wants it).
- A player under a parent writes its world pose as its local `Transform`.
- Character velocity is not in snapshots: a restore puts the player at rest (step 5, `BodyMotion`).

## 7. How to run

```
build/Release/level_physics_tests.exe
build/Release/scene_tests.exe
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
python tests/serve/check_serve.py
python tests/cli/check_cli.py --cpu-only
```
