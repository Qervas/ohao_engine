# Combat, step 3: death, restart and win

**Spec:** `docs/specs/2026-10-27-combat-design.md` ("Death, restart, win (step 3)", "As built (step
3)", "Rulings for Frank" 24–28)
**Status:** done 2026-10-27 (plan `docs/plans/2026-10-27-combat.md`, Task 3)

## 1. Summary

The play scene is a round. At its first step the world snapshots itself; when the player dies,
3 s later (or at once on Enter) the round goes back to that snapshot exactly: the same
`world.hash`, poses, health, ammunition, crates and enemies. Killing both enemies wins (a `win`
event, once); the round then waits for Enter. A dead player stands where it fell and can neither
move, look, jump nor fire. A restart, after a death or after a win, replays the same fight bit
for bit.

## 2. What was built

- `Round` (saved singleton: `restartDelay` 3 s, `restart` action) on the play scene's Controls
  entity; `restart` bound to Enter.
- `RoundHook` (`ohao/ecs/round_hook.hpp`, in the registry's context) and
  `ecs::installRoundHook(world, types)` (`ecs/snapshot.cpp`); `api::Session` installs it, so the
  CLI, `ohao serve`, `LiveHost` and `ohao_game` all have rounds.
- The combat step's last phase (`stepRound`): `round_start` and the snapshot at the end of the
  first step with a `Round`; the restart at the end of the step it triggers; `win` once all
  enemies are dead. Events `RoundStart`, `Restart`, `Win`; `CombatSystem::round()` (`started`,
  `phase` Playing/Dead/Won, `restartIn`, `restarts`, `lastRestartMs`).
- Physics: a dead input-driven character skips look, walk and jump.
- `IPhysicsBackend::setCharacterInWorld` (Jolt: `RemoveBody`/`AddBody` of the inner body, kept
  with its BodyID); `PhysicsScene` parks the character of a removed `PlayerController` and gets
  it back on a restore; a character a restore creates starts at its restored pose, not a spawn.
- `combat_tests` test 17 (8 tests); `physics_bench` `round_restart_ms` (100 restarts through the
  `restart` key), gated from `combatStep` 3.

## 3. Bugs found and fixed

- **A won round's restart was not the round's start.** Found by the look shots: after both enemies
  died and Enter was pressed, `world.hash` differed from the start's (live window: `699e21a9…`
  against `f219ecec…`). The dead enemies' `PlayerController`s, put back by the restore, made new
  characters at their spawn points facing the spawn's yaw (the start had them turned one step
  toward the player). Fixed first by placing a restored character at its restored pose (the
  hash after the restart step then matched). A new test then showed the play after it drifting:
  with new characters the snapshot's simulation could not restore (`bodyIdentity` differed), the
  bodies fell back to teleports, and a crate stack resettled 3e-5 m off after 120 steps. Fixed by
  parking a removed controller's character out of the broadphase instead of destroying it, so the
  restore finds the same character and restores bodies, contact cache and characters exactly
  (`t17_restart_after_win_replays_identically`, failing before, passing after).
- Test of mine: `startRound` used `CHECK` inside a function returning a value (C2561); it returns
  its event count through an out-parameter now.

## 4. Spec corrections

- The round runs inside the combat step: it starts at the end of its first step (the world after
  that step is the start) and a restart lands at the end of the step that triggers it.
- A won round waits for `restart`; `restart` pressed while playing restarts too.
- A removed `PlayerController`'s character is parked, not destroyed (the spec said "its character
  is removed").

## 5. Results

- `python tools/check.py --all`: all 43 functional suites green, among them `combat_tests` 37/37,
  `level_physics_tests` 53/53, `api_tests` 61/61, `scene_tests` 84/84, `physics_backend_tests`
  61/61, `engine_tests` 85/85, `ecs_tests` 37/37, `check_serve` 212/212, `check_cli` 33/33 (29/29
  CPU), `check_live` 49/49, `e2e_mcp` 45/45, `check_validation` 6/6, `render_golden` 26/26,
  `check_docs` 85/85. The five benches were noisy in the first run and both reruns: Grok Bot and
  its webview used 0.7–3 cores (calibration up to ×3.13). Not a verdict, pending a quiet rerun.
- `physics_bench` (first run of the gate, noisy ×1.46): every budget passed; `round_restart_ms`
  median 0.0060 ms, p95 0.0173 (100 restarts; budget 2 ms); `combat_shot_ms` 0.0014,
  `combat_step_ms` 0.0020, `enemies16_step_ms` 0.0612. `round_restart_ms` times the components'
  restore; the bodies come back in the next sync, inside the step's own time.
- `render_bench`: `cornell_720p lighting` 0.401 ms against 0.32, the standing miss since SP5 step 4
  (no renderer code changed).
- No golden or expectation changed in this closing pass (the `Round` type's goldens landed with
  `b20d56f2`, looked at then: the schema hash, the catalogue, the hashes in serve and CLI
  expectations and the guides' examples, all from the new saved type's name).
- `combat_tests` test 17: the restart 180 steps after the death; 66 events in the round's first 120
  steps, the same after a restart and after a won round's restart.
- Look shots (`C:\Users\djmax\ohao_refs\look_shots\sp7_combat\`): `step3_round_00..06.png` and
  `step3_round_strip.png`, through the player's eyes over MCP: the round's start (hash
  `f219ecec293dbcf9`); the player cut to 8 hp and shot dead by the enemies walking in; the step
  the round restarted 3 s later (hash `f219ecec293dbcf9` again, the same picture); EnemyA shot
  down, then EnemyB (won); the room from above, both lying; Enter pressed, the start again (hash
  `f219ecec293dbcf9`).

## 6. Known gaps

- Nothing on screen says dead or "CLEARED" yet (the HUD is step 4); the round's state is in
  `CombatSystem::round()` only (an agent method comes in step 5).
- A parked character keeps a Jolt body out of the broadphase until its entity is destroyed.

## 7. How to run the new tests

```console
$ build/Release/combat_tests.exe > /dev/null
$ build/Release/physics_bench.exe --check tests/bench/physics_budgets.json > /dev/null   # slow
$ python build/sp7/look_step3.py   # the look shots (needs a GPU)
```
