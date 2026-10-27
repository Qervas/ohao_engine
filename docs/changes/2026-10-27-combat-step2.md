# Combat, step 2: enemies that see, chase and shoot

**Spec:** `docs/specs/2026-10-27-combat-design.md` ("Enemies (step 2)", "As built (step 2)", "Rulings
for Frank" 19–23)
**Status:** done 2026-10-27 (plan `docs/plans/2026-10-27-combat.md`, Task 2)

## 1. Summary

The play scene has two enemies. They stand at the level's enemy spawns until they see the player
(a ray from the eye to the player's chest, inside their cone) or are hit; then they chase the
place they last saw it, and in range with a clear line stop, turn to the player and fire bursts
after a reaction time. Out of sight for 3 s they give up. At 0 health they lose their character
and fall over. They are characters on the player's code, so they walk, climb and collide like
the player, and all their state is saved: a snapshot mid-fight replays it bit for bit.

## 2. What was built

- `PlayerController.spawn` (the k-th character with a tag takes the k-th `SpawnPoint` with that
  tag; empty: where it stands) and `PlayerController.input` (false: moved by `MoveIntent`).
- Saved components: `MoveIntent` (move, yaw, pitch, jump, sprint), `Enemy` (sight range, cone,
  attack range, reaction time, aim error, burst, burst pause, lose time, turn rate), `EnemyState`
  (mode `Idle`/`Chase`/`Attack`/`Dead`, last seen, lost for, reaction, burst left, pause, last
  health, shots seen) and the enum `EnemyMode`.
- `ohao/ecs/enemy_ai.{hpp,cpp}`: `stepEnemies`, run at the start of the combat step before the
  weapons.
- Physics: input-less characters move by their `MoveIntent`; spawns by tag.
- Combat: weapons read input only for input-driven characters.
- API: `player.info`/`player.teleport` without `entity` mean the input-driven player;
  `scene.load` also runs the transform and physics sync (bodies built at load).
- `assets/scenes/blockout_play.scene.json`: EnemyA and EnemyB (red box bodies and heads).
- `combat_tests` test 16 (12 tests); `physics_bench` `combat_step_ms` and `enemies16_step_ms`.

## 3. Bugs found and fixed

- A snapshot taken right after `scene.load` held the characters but no level bodies (built by
  the first step); its restore rebuilt the bodies, and EnemyB, standing on the ramp, replayed one
  float off (`world.diff`: its y 0.50609356 against 0.5060937). Found by SP6's
  `sp6_t14_restore_and_replay_a_walk_through_the_api` once the scene had enemies. `scene.load`
  now builds the bodies.
- The SP6 teleport target (-2.2, 0.3, -2) is 0.3 m from EnemyA's spawn: the player was pushed out
  of the enemy's capsule. Moved to (-2, 0.3, -1.35) in the API test, the serve session, e2e and the
  guide.
- Tests of mine: B's spawn on the ramp settles 1 mm (tolerance 1e-3 → 0.01); the chase test ran
  long enough for the chase to end in an attack (now 30 steps, then 60 to the attack); the
  reaction-time test measured from a step after the enemy had already seen the player (now the
  player is teleported into view).
- The `enemies16` bench at first measured idle enemies (facing away, then blocking each other's
  sight); its correctness check (they must close in) caught it.

## 4. Spec corrections

- The AI runs inside the combat step, not as a system before physics; `EnemyState` and
  `MoveIntent` are saved, not runtime. See "As built (step 2)".

## 5. Results

- `python tools/check.py --all`: all 43 functional suites green, among them `combat_tests` 29/29,
  `level_physics_tests` 53/53, `api_tests` 61/61, `scene_tests` 84/84, `engine_tests` 85/85,
  `check_serve` 212/212, `check_cli` 33/33, `check_live` 49/49, `e2e_mcp` 45/45 (both play the
  scene with the enemies firing), `check_validation` 6/6, `render_golden` 26/26, `check_docs`
  85/85. Benches: `scene_bench`, `serve_bench` (16/16), `level_bench` and `physics_bench` passed;
  `render_bench` 21/22, its one miss `cornell_720p lighting` 0.362 ms against 0.32, the standing
  miss since SP5 step 4 (0.366–0.409 ms in every gate since; no renderer code changed here).

- `combat_tests` 29/29 (17 of test 15, 12 of test 16): bursts 15, 15, 48 steps apart; first round
  21 steps after the attack began; idle 181 steps after the line was blocked.
- `physics_bench` (calibration ×1.02, quiet): `combat_step_ms` 0.0019 ms (budget 0.05, 90 enemy
  rounds), `enemies16_step_ms` 0.057 ms (budget 1.0, the 16 closing 0.71 m on average),
  `combat_shot_ms` 0.0013 ms (0.02); every physics budget passed.
- Goldens: catalogue and scene schema (+ `EnemyMode`, `MoveIntent`, `Enemy`, `EnemyState`,
  `PlayerController.spawn`/`input`; schemaHash `9a90b74e98d2d96c` → `d04016663f613766`); CLI
  (`schema`, `run_script`: schemaHash; `summary`: `WorldTransform` counted after a load); serve
  (schemaHash, world hashes, the play scene's 13 entities, the snapshot 5,816 → 8,056 bytes, the
  moved teleport, the player nudged by EnemyA chasing in the player session); the play-scene render
  golden re-baselined (the enemies in view; elsewhere RT GI/shadow/SSAO noise only, looked at).
- Look shots (`C:\Users\djmax\ohao_refs\look_shots\sp7_combat\`): `step2_enemies_00..06.png` and
  `step2_enemies_strip.png`: the enemies idle, then turned on the player (player 100 → 68 hp), the
  room from above, a chase (their attack range cut to 2 m through `world_apply`), EnemyA shot dead
  and lying, EnemyB fighting on (player 52 hp). Frame 06's overhead view is turned 90° to frame
  02's (the player had turned; the shot script's head-frame maths does not cancel it).

## 6. Known gaps

- No pathfinding: enemies walk straight at where they last saw the player.
- No flash, marks, sound or HUD yet (step 4); no death of the player beyond its health reaching
  0, no restart, no win (step 3).
- Enemies have no hit reaction beyond waking.

## 7. How to run the new tests

```console
$ build/Release/combat_tests.exe > /dev/null
$ build/Release/physics_bench.exe --check tests/bench/physics_budgets.json > /dev/null   # slow
$ python build/sp7/look_step2.py   # the look shots (needs a GPU)
```
