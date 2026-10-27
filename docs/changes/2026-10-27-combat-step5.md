# Combat, step 5: the agent surface and the done demo

**Spec:** `docs/specs/2026-10-27-combat-design.md` ("The agent-facing surface (step 5)", "As built
(step 5)", "Done criteria", "Rulings for Frank" 37–42)
**Status:** done 2026-10-27 (plan `docs/plans/2026-10-27-combat.md`, Task 5); sub-project 7 done

## 1. Summary

An agent plays the play scene's round the way a person does, through input, and reads it
through three tools: `combat.info` (the round and every combatant), `combat.events` (the log) and
`combat.damage` (hit points off anything with `Health`). A whole round, EnemyA shot down, the
player killed, the restart 3 s later, the room cleared and won, plays the same in a hidden
window and headless: the same `world.hash`, the same `combat.info` and events, the same picture
with the HUD, bit for bit. Building it found two bugs, fixed in their own commits: a won round's
restart one quaternion sign off the round start's hash, and impact marks a late renderer
attached in the wrong place. Sub-project 7 is done: the slice is playable.

## 2. What was built

- `ohao/api/combat.cpp`: `combat.info {}`, `combat.events {since, max}`, `combat.damage {entity,
  amount, source?}` (params in `params.{hpp,cpp}`, docs and dispatch in `session.cpp`;
  `combat.damage` mutates). `combatEventKindName` in `ecs/combat`.
- `tools/list`: 35 tools, 23,419 bytes (cap 24,576).
- The `impact_marks` simulate system after `combat` (installed by `installCombat`): marks taken
  at the step against the step's poses; the render-side system only draws.
- `PhysicsScene::createPlayer` keeps a restored entity's rotation (and its head's) bit for bit.
- Tests: `api_tests` `sp7_t18_combat_info`, `_events_cursor`, `_damage_kills`, `_params_refused`;
  `combat_tests` `t17_restart_after_kills_keeps_restored_rotations`; the serve session
  `tests/serve/sessions/combat.jsonl` and check_serve's examples; `check_live.py` sp7 test 18 (a
  fight, window against headless); `e2e_mcp.py` the fight over MCP.
- Docs: the serve guide's "Fight" section with a run example (`docs/agents/examples/fight.jsonl`,
  run by `check_docs`), the CLI guide's method and error tables, AGENTS.md (suites, combat notes,
  the agent surface, restored rotations), ARCHITECTURE.md (the marks system), the roadmap (row 7
  done), the monograph unit `physics/combat`.
- The done demo: look shots `step5_demo_00..07` and a strip.

## 3. Bugs found and fixed

- **A won round's restart was one quaternion sign off the round start's `world.hash`.** Found by
  `sp7_t18_combat_damage_kills` (kill both enemies, the player killed, the restart: the hash
  differed; `world.diff` showed only the enemies' `Transform.rotation`, `(-0,-1,-0,w)` against
  `(0,1,0,w)`). The play scene's enemies spawn facing exactly 180 degrees; the restore put their
  Transforms back exactly, then `createPlayer`, bringing the parked characters back, re-derived
  the yaw from the rotation (`atan2` reads -180 back as +180) and wrote `yawQuat(+180)`: the same
  rotation, another quaternion. The next step wrote the bits back, so step 3's replay tests did not
  see it. Fixed: a restored entity keeps its rotation (and its head's) when it is the rotation
  that would be written; `rotNow` was a reference into the storage `writePlayer` rewrites and is
  a copy now. `t17_restart_after_kills_keeps_restored_rotations` failed without the fix (both
  enemies' rotations flipped) and passes with it.
- **Impact marks were attached by a late renderer.** Found by check_live's fight: the window and
  headless hosts reached the same `world.hash` but not the same HUD picture. A headless host's
  renderer starts at its first capture and read the whole combat log then, attaching each mark by
  its target's pose at that moment: a dead, tipped-over enemy carried its marks elsewhere. Fixed:
  marks are taken by a simulate system right after `combat`, against the step's poses (Transforms
  up the chain); the pictures are now equal bit for bit.
- Tests of mine: e2e_mcp expected a refused `combat_damage` as a tool error, but refused params
  are a JSON-RPC error (-32602); the rotation test first loaded the scene without the syncs
  `scene.load` runs, so the round started before anyone spawned.

## 4. Spec corrections

- `combat.info` gives each combatant's `pos` and an enemy's `lastSeen` (the spec's `target`,
  always the player) (ruling 37).
- `combat.damage` acts as a round: an ancestor's `Health`, a dead enemy lying down at the next
  step (ruling 38). `combat.events` numbers events from 1 (as `physics.events`).
- Impact marks are taken at the step, not by the renderer (ruling 39, superseding 36 for marks).
- The done demo is played by an agent through `ohao serve --window` (ruling 41).

## 5. Results

- `python tools/check.py --all` (build and doctor ok): every functional suite green:
  `api_tests` 65/65, `combat_tests` 48/48, `level_physics_tests` 53/53, `scene_tests` 84/84,
  `engine_tests` 86/86, `runtime_tests` 34/34, `ecs_tests` 37/37, `reflect_tests` 42/42,
  `rpc_tests` 18/18, `check_serve` 228/228, `check_cli` 33/33 (29/29 CPU), `check_live` 57/57,
  `e2e_mcp` 53/53, `check_validation` 6/6, `render_golden` 27/27 (both play-scene goldens exact,
  max_abs 0), `deferred_determinism_gpu_test` 65/65, `cull_parity_gpu_test` 45/45,
  `light_cluster_gpu_test` 25/25, `light_emitter_gpu_test` 28/28, `check_docs` 86/86 (the
  serve guide's fight example among them), `monograph_structure` and the `ohao_game`
  presentation gate passed.
- Benches: `physics_bench` passed clean (calibration ×1.01): `combat_shot_ms` 0.0014 ms (p95
  0.0017, budget 0.02), `combat_step_ms` 0.0019 (0.0043; 0.05), `enemies16_step_ms` 0.0622
  (0.0732; 1.0), `round_restart_ms` 0.0034 (0.0098; 2); `serve_bench` 16/16 and `level_bench`
  passed. `scene_bench` noisy in all three runs (39 of 50 rounds contended; every op within its
  budget in clean rounds, `apply_100_field_set` 0.187 ms against 0.2) and `render_bench` noisy in
  all three (GPU calibration over ×1.25; the `hud` pass 0.006 ms against 0.05). Not a verdict.
- `tools/list`: 35 tools, 23,419 bytes (cap 24,576). `combat.info` on the play scene: 700 bytes.
- check_live sp7 test 18: EnemyA down in 2 bursts (the player at 92 hp), the player killed, the
  round dead with 2.98 s to go, restarted 3 s later with everyone at 100, the room cleared
  (EnemyA in 2 bursts, EnemyB in 2) and won; window and headless: `world.hash`
  `5af5886679490c93` both, `combat.info` and events equal, the HUD picture equal bit for bit.
- `ohao_game assets/scenes/blockout_play.scene.json --frames 600 --resize-at 200` under
  validation: 600 frames, 0 messages, 0 unrequested readbacks, the combat sounds loaded (10).
- Look shots (`C:\Users\djmax\ohao_refs\look_shots\sp7_combat\`), the done demo played by an agent
  over MCP through `ohao serve --window` with the HUD: `step5_demo_00.png` engage (the round's
  start, facing EnemyA, 100 and 30/30); `_01` firing: the flash, marks on EnemyA, the hit
  marker (EnemyA at 50); `_02` EnemyA down, lying (the player at 84, hit meanwhile by EnemyB);
  `_03` hit by EnemyB: the vignette (76); `_04` dead: DEAD, RESTART 3 (cut to 8 hp by
  `combat.damage`, finished by EnemyB's round); `_05` the restart: everyone back at 100, 30/30;
  `_06` clearing the room: EnemyA down again, EnemyB under fire (50); `_07` won: CLEARED, ENTER
  TO RESTART; `step5_demo_strip.png` all eight.

## 6. Known gaps

- A player killed before the round's first step starts the round dead and restarts dead (the
  round's start is the world after its first step; ruling 42, documented in the serve guide).
- On the won screen the crosshair is still drawn and overlaps "ENTER TO RESTART".
- `ohao_game` was not played by hand in this session (no keyboard here): its run under
  validation and the same live host driven over MCP stand in. The checklist below is for Frank.
- Bench runs on this machine are noisy from outside load (see Results); none is a verdict.
- From step 4: the speckle column on the left wall's end (RT GI fireflies, not combat's), the
  muzzle light's strong wash on near walls, dark marks hard to see on the enemies at range.

## 7. Try it by hand (`ohao_game`)

```
build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json
```

- The mouse is captured while the window has focus: move it to look. WASD walk, Shift sprint,
  Space jump.
- Left mouse button fires (automatic, 10 rounds a second, 30 a magazine); R reloads; an empty
  magazine reloads itself (1.5 s). Watch the crosshair open with sustained fire and the view kick
  up, then settle when you stop.
- Each round: the muzzle flash, a mark where it hit (walls, crates, the enemies), the impact
  sound by surface; red ticks at the crosshair when a round hurts an enemy.
- The enemies (red, on the step and the platform) see you, chase and shoot in bursts of three:
  a red vignette and the hurt sound when they hit; health bottom left.
- Kill both: CLEARED, Enter to restart. Die: DEAD with a 3-2-1 countdown, then the round
  restarts exactly where it began. Enter restarts at any time.
- Esc frees the mouse; a click takes it back without firing. `--mute` silences footsteps and
  weapons.

## 8. How to run the new tests

```console
$ build/Release/api_tests.exe > /dev/null
$ build/Release/combat_tests.exe > /dev/null
$ python tests/serve/check_serve.py > /dev/null
$ python tests/serve/check_live.py > /dev/null   # gpu
$ python tests/serve/e2e_mcp.py > /dev/null   # gpu
$ python build/sp7/look_step5.py   # the done demo's look shots (needs a GPU)
```
