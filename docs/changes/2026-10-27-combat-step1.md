# Combat, step 1: a hitscan weapon, health and damage

**Spec:** `docs/specs/2026-10-27-combat-design.md` ("The combat step", "Components", "Events",
"Performance budgets", "As built (step 1)", "Rulings for Frank" 1–18)
**Status:** done 2026-10-27 (plan `docs/plans/2026-10-27-combat.md`, Task 1)

## 1. Summary

The world's fixed step fires weapons. `installCombat` adds a `combat` simulate system after
`physics`; every entity with a `Weapon` reads its trigger (a player's from the World's input:
`fire`, `reload`; anything else's from `WeaponTrigger`), and fires at most one round a step: a ray
from the eye within a seeded spread cone that blooms with sustained fire, which damages what it
hits if that (or an ancestor) has `Health`. Each round kicks the player's view up; the kick comes
back down once the trigger rests. A magazine of 30 empties and reloads (1.5 s), or reloads on R.
Health reaching 0 is death: the dead take no more damage and do not fire. Everything is saved
state or a function of it, so a fight replays bit for bit from a snapshot. The play scene's player
carries the rifle.

## 2. What was built

- Components (saved, registered with docs, units and limits; writers; a corpus case
  `21_combat`): `Health {max, current}`, `Weapon` (damage, fireRate, automatic, magazine,
  reloadTime, range, spread and bloom, recoil and its recovery, seed, action names),
  `WeaponState` (ammo, cooldown, reloading, bloomDeg, kickDeg, shots, triggerHeld),
  `WeaponTrigger {pull, reload}`.
- `ohao/ecs/combat.{hpp,cpp}`: `CombatSystem` (the step, `damage`, a 1,024-event log with a
  cursor), `installCombat`, `combatSystem(world)`, `combatEvents(world)`; events `shot`, `hit`,
  `damage`, `death`, `reload`, `reloaded`; counter-based random numbers
  `combat::unitRandom(seed, shot, i)` and `combat::spreadDirection` (uniform over the cone's disc).
- `PhysicsScene::raycastFrom(from, dir, max, ignore)`: the nearest hit that is not the shooter's
  body and not a trigger. `PhysicsScene::turnPlayer(e, yaw, pitch)`: recoil, written as the step's
  own move (no teleport, interpolation kept).
- Hosts: the CLI context (`ohao run`, `ohao serve`) and `LiveHost` (`ohao serve --window`,
  `ohao_game`) install combat after physics.
- `assets/scenes/blockout_play.scene.json`: the player has `Health` and a `Weapon`; `fire` on the
  left mouse button, `reload` on R.
- `combat_tests` (new suite, label `quick`, test 15, 17 tests); `combat_tests --dump-burst FILE`
  writes a 30-round burst as JSON for the look shots.
- `physics_bench`: `combat_shot_ms`, gated by a new `combatStep` (1) in
  `tests/bench/physics_budgets.json`; `combat_step_ms`, `enemies16_step_ms` (step 2) and
  `round_restart_ms` (step 3) listed, pending.
- Docs: AGENTS.md (suite row, combat notes, the bench), ARCHITECTURE.md (the step's data flow),
  the serve guide (the rifle, `docs/agents/examples/fire.jsonl`), the CLI guide's component table,
  the spec's "As built (step 1)".

## 3. Bugs found and fixed

- Two tests were wrong, not the code: `t15_dead_do_not_fire` expected one round from the surviving
  turret in 30 steps (it fires on at 10 Hz: five), and `t15_runs_are_identical` put its target
  18° off the aim, so the fight never hit it. Both fixed in the test.
- The first `combat_shot_ms` run damaged the target with 170 of 2,000 rounds: at 60 Hz the kick
  never recovers and 200 rounds climbed the view off the box. The bench now turns spread and
  kick off (it measures a round's cost, not aim) and fails if any round misses.

## 4. Spec corrections

- Bloom (and kick) recover only once the fire interval has passed without a round. The spec said
  bloom falls on every step without a round; at 10 Hz that is five steps in six, and bloom never
  built up under sustained fire. Corrected in "Spread" (ruling 16).

## 5. Results

- `python tools/check.py --all`: 42 of the 43 functional suites green (`check_docs` below), among them
  `combat_tests` 17/17, `scene_tests` 84/84, `api_tests` 61/61, `level_physics_tests` 53/53,
  `engine_tests` 85/85, `ecs_tests` 37/37, `check_serve` 212/212, `check_cli` 33/33,
  `check_live` 49/49, `e2e_mcp` 45/45, `check_validation` 6/6, `render_golden` 26/26 (no
  golden moved), `deferred_determinism_gpu_test` 65/65, `cull_parity_gpu_test` 45/45. The
  five benches (`scene`, `serve`, `level`, `physics`, `render`) were `noisy` on their first run and
  both reruns (other processes 62–200 % of a core; calibration up to ×3.03): neither a pass nor
  a regression. `check_docs` failed 82/85 there on three serve-guide examples whose world hashes
  the new saved types moved; refreshed, it is 85/85.

- `combat_tests` 17/17. The spread cone: 10,000 draws inside 3°, mean 2.004° (2/3 of the cone)
  for three aims. Bloom: rounds 1–10 leave in 0.25, 0.60, 0.95 … 3.25°. Recoil: +0.350° pitch a
  round, sideways 0.018° on the first, back to 0 (±1e-4) a second later. Fire held 300 steps: 30
  rounds, the reload on the 30th's step, 90 steps, firing again the step it ends.
- Goldens: the catalogue and scene schema gain the four types (schemaHash
  `5a3481357bee4b05` → `9a90b74e98d2d96c`); the CLI's `schema` and `run_script` and six serve
  sessions change in the API schemaHash (`568c4633526fa55a` → `06331bfe6f10f1d5`) and in
  `world.hash` (saved type names are hashed even when no entity has them, so every world's hash
  moves), and the `player` session's spawn snapshot grows 5,680 → 5,816 bytes (the rifle's
  components). Nothing else in them changed. No render golden changed.
- `physics_bench`: `combat_shot_ms` median 0.0032 ms, p95 0.0044 (2,000 rounds, all damaged;
  budget 0.02) by hand (calibration ×2.40); in the gate 0.0061 ms, p95 0.0081 (×3.03). Both
  noisy runs; the budget holds with a factor of 3 to spare. `events_1k_ms` (code this step does
  not touch) read 0.571 and 0.611 ms in those runs, over its 0.5 budget, and 0.326 at ×1.96: noise,
  pending a quiet rerun.
- Look shots (`C:\Users\djmax\ohao_refs\look_shots\sp7_combat\`): `step1_burst_00..06.png` and
  `step1_burst_strip.png`, the player's eyes over MCP holding `fire` on the play scene (pitch 0,
  1.75, 3.50, 5.25, 7.00, 10.50 at 0/5/10/15/20/30 rounds; 5.00 a second after release);
  `step1_spread_plot.png`, a 30-round burst on the north wall (all 30 hit; the group climbs with
  the kick and widens with bloom) and pitch/yaw over the 180 steps.

## 6. Known gaps

- Nothing shows a hit yet: no muzzle flash, impact mark, sound or HUD (step 4). The look shots
  show recoil only.
- The click that captures the cursor in `ohao_game` also fires a round (ruling 1; step 4).
- No enemies (step 2), no death handling beyond "the dead do not fire", no restart (step 3), no
  combat methods for agents (step 5): read a fight through `world.query` on `WeaponState` and
  `Health`.
- A full magazine's kick takes 1.75 s to recover at 6°/s; tune `recoilRecoveryDegPerSec` if it
  plays slow.

## 7. How to run the new tests

```console
$ build/Release/combat_tests.exe > /dev/null
$ build/Release/physics_bench.exe --check tests/bench/physics_budgets.json > /dev/null   # slow
$ python build/sp7/look_step1.py   # the look shots (needs a GPU)
```
