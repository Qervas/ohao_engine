# Sub-project 7 — Combat: a hitscan weapon, health and damage, enemies, death and restart, a HUD

Status: written 2026-10-27 on Frank's "go SP7 Combat". Frank delegated the technical choices;
this document makes them, and every judgment call is listed under "Rulings for Frank". It is
the binding spec once Frank accepts it.

Roadmap: row 7 of `docs/ROADMAP.md` ("hitscan weapon (recoil, spread, muzzle flash, impact
decals), health/damage, enemy AI, death/restart, HUD"; done when the slice is playable). Before
it: 6 (player and physics, done 2026-10-26). After it: 8 (by need).

Builds on:
- **Sub-project 4** (`2026-10-23-agent-control-surface-design.md`): system phases (simulate
  once per fixed step), `world.step`, `input.inject`, record/replay, `world.hash`, snapshots,
  `ohao serve`, MCP.
- **Sub-project 5** (`2026-10-26-level-pipeline-design.md`): the blockout level, its
  `SpawnPoint`s (`SpawnPlayer`, `SpawnEnemyA`, `SpawnEnemyB`, tag `enemy`), `Collider.surface`,
  `physics.raycast`.
- **Sub-project 6** (`2026-10-26-player-physics-design.md`): the `physics` simulate system,
  `PhysicsScene::raycast`, the player (`PlayerController` on a Jolt `CharacterVirtual` whose
  inner body is what rays and triggers see), the head camera, render interpolation, the event
  log pattern, footsteps through `AudioSystem` in the window host, snapshots that replay
  exactly (`SnapshotHook`), `blockout_play.scene.json`.

## Thesis: combat is more simulation

A shot, a hit, a wound and a death are things the world's fixed step decides, in the same
step, from the same state, for the window, the headless hosts, the tests and the agents
alike. They are deterministic (seeded), observable (an event log beside the physics one),
saved and hashed where they are state (health, ammo), and replayable from a snapshot bit for
bit. Presentation (flash, marks, sound, HUD) reads the events and never feeds back.

## Goal

The first playable slice, on `blockout_play.scene.json`:
- The player holds a rifle: fires on the left mouse button at 10 rounds a second, a 30-round
  magazine, R reloads (and an empty magazine reloads itself); the view kicks up with each round
  and recovers; spread blooms with sustained fire and recovers; a muzzle flash lights the room
  for a moment; every hit leaves a mark; it sounds.
- Two enemies stand at the enemy spawns. One that sees the player chases it and, in range with
  a clear line, shoots back (slower, less accurate). Each dies after four hits.
- The player dies at 0 health; a few seconds later the round restarts: enemies, health, ammo,
  crates and the player back where the round started, exactly.
- A HUD shows health, ammo, a crosshair, hit markers and a red flash when hurt, "DEAD" and
  "CLEARED" (all enemies dead).
- An agent can fight the same fight over MCP with `input.inject`, read `combat.info` and
  `combat.events`, and see it with `render.capture`.

## Scope: in 7 vs later

In 7: one hitscan weapon per combatant; `Health`; damage and death; the combat step and its
event log; enemies (idle, chase, attack, dead) on the player's character code; round restart
by snapshot; muzzle flash (a light); impact marks (a capped pool of quads); generated sounds;
a HUD pass; three agent methods; budgets; a done demo.

Later (8 or by need): projectiles, grenades, more than one weapon, weapon switching, aim down
sights, headshots and hit zones, damage falloff, armour, healing and pickups, navigation
meshes and pathfinding, cover, squads, ragdolls, animation, a view-model with animation,
networked play, a real decal projector (2b's renderer has none), a HUD font beyond the glyphs
the slice needs.

## Why (what the code does today)

- `PhysicsScene::raycast(from, dir, max)` returns the nearest body and maps a character's inner
  body to its entity. It stops at the first body: the shooter's own capsule and trigger volumes
  would block a shot.
- The player's look lives in the `PhysicsScene` (`PlayerRecord` yaw and pitch, written to the
  player's rotation and the head's). Nothing else can turn it except an external write (which
  the step treats as a placement).
- Nothing in the world has health, nothing shoots, and the enemy spawn points place nothing.
- Snapshots replay exactly (SP6 step 5), so a round start can be a snapshot.
- There is no 2D drawing in the renderer (no ImGui, no font): the HUD needs a pass.

## The combat step

`installCombat(World&)` (`ohao/ecs/combat.{hpp,cpp}`, after `installPhysics`, which it
requires) adds a `combat` simulate system after `physics`. Each step:

1. Every combatant with a `Weapon`, in id order (deterministic; a shooter killed earlier in the
   same step does not fire): read its trigger (a player's from the World's `InputMap`:
   `held`/`pressed` of `Weapon.fire` and `Weapon.reload`; anyone else's from its
   `WeaponTrigger`), advance cooldown, reload, bloom and recoil recovery, and fire at most one
   round.
2. A round: the eye and aim (a character: its feet + `eyeHeight`, along its yaw and pitch as
   this step left them; anything else: its world origin along its −Z), a direction drawn in the
   spread cone, one ray (`PhysicsScene::raycastFrom`, ignoring the shooter and triggers) to
   `Weapon.range`. A hit on an entity with `Health`, or whose ancestor has one, damages it.
3. Events go to the step's list and the combat log (1,024, sequenced, as the physics log).

The system does nothing for a world without a `Weapon` or `Health`.

### Firing, magazine, reload

- `fireRate` (Hz) sets the interval; the cooldown carries its remainder across steps, so 10 Hz
  at 60 Hz steps fires on every sixth step exactly. At most one round a step.
- `automatic` fires while held; otherwise one round per press.
- Each round takes one from `WeaponState.ammo`. At 0 the reload starts by itself; `reload`
  starts it with a partly empty magazine; it takes `reloadTime` and fills the magazine
  (infinite reserve in the slice). Firing during a reload does nothing.

### Spread (bloom)

The round leaves within a cone of half angle `spreadDeg + bloomDeg` around the aim, uniform
over the cone's disc (radius `tan(angle)·sqrt(u1)`, angle `2π·u2`). Each round adds
`bloomPerShotDeg` to `bloomDeg`, up to `bloomMaxDeg`; bloom falls at `bloomRecoveryDegPerSec`
on every step without a round.

The random numbers are counter-based: `u = hash(seed, shot, i)` (`WeaponState.shots` is the
shot index), so a weapon needs no generator state, two worlds with the same seed fire the same
pattern, and a snapshot replays it.

### Recoil

Each round turns the shooter's view up by `recoilPitchDeg` and sideways by a seeded amount in
±`recoilYawDeg`, now (`PhysicsScene::turnPlayer`: the step's own write of the player's yaw and
the head's pitch, never a placement). The vertical kick is kept in `WeaponState.kickDeg` and
comes back down at `recoilRecoveryDegPerSec` once the fire interval has passed without a round
(the sideways kick stays, as in most shooters). The next round aims where the view now is.

### Damage and death

`damage(target, amount, source, point)` (the hit, `combat.damage`, the enemies' rounds):
`current = max(0, current − amount)`; a `damage` event (source, target, point, amount, health
after); at 0 a `death` event, once. The dead take no more damage and do not fire. A player at
0 health stops moving and looking (step 3).

## Components

Saved (scene files, `world.hash`, snapshots), registered with docs, units and limits:
- `Health { max 100, current 100 }` (hp).
- `Weapon`: `damage` 25, `fireRate` 10 Hz, `automatic` true, `magazine` 30, `reloadTime` 1.5 s,
  `range` 100 m, `spreadDeg` 0.25, `bloomPerShotDeg` 0.35, `bloomMaxDeg` 3,
  `bloomRecoveryDegPerSec` 6, `recoilPitchDeg` 0.35, `recoilYawDeg` 0.15,
  `recoilRecoveryDegPerSec` 6, `seed` 1, action names `fire` and `reload`.
- `WeaponState`: `ammo` 30, `cooldown` s, `reloading` s left, `bloomDeg`, `kickDeg`, `shots`,
  `triggerHeld` (the last step's trigger, for semi-automatic edges). The combat step emplaces
  it (a full magazine) for a `Weapon` without one, so a scene file need not author it.
- `WeaponTrigger { pull, reload }`: what a non-player pulls (an enemy's AI, a turret, a test);
  `reload` is consumed when acted on.
- `Enemy` (step 2): the AI's parameters (below).
- `Round` (step 3): the round's rules (below).

Runtime: `EnemyState` (step 2, snapshotted), the impact marks' pool (step 4, never saved).

## Events

`CombatEvent { kind, step, seq, source, target, point, normal, amount, health, surface }`,
kinds `shot` (source; point = eye, normal = direction), `hit` (source, target body, point,
normal, surface, amount = distance), `damage`, `death`, `reload` (start), `reloaded`, and from
step 3 `round_start`, `restart`, `win`. `combatEvents(world)` is the latest step's;
`CombatSystem::eventsSince(seq, max, dropped)` the log, as `physics.events`.

## Enemies (step 2)

An enemy is an authored entity in the scene: `PlayerController` with `spawn: "enemy"` and
`input: false`, `Health`, `Weapon` (its settings), `Enemy`, and a body mesh child. So enemies
are characters on the player's code (the same capsule, ground, steps, slopes, pushing), saved
and hashed like the player, and an agent can add one with `entity.create`.
- `PlayerController` gains `spawn` (the `SpawnPoint` tag it spawns at, `player`; empty: where
  it stands) and `input` (false: it moves by its `MoveIntent`, written by the AI, not by the
  World's input). The k-th character with a tag (creation order) takes the k-th spawn point
  with it; one without a free point stays where it stands (`W_NO_SPAWN`).
- `Enemy`: `sightRange` 30 m, `fovDeg` 140, `attackRange` 14 m, `reactionTime` 0.35 s (from
  first sight to first round), `aimErrorDeg` 2.5 (added to its weapon's spread), `burst` 3
  rounds then `burstPause` 0.8 s, `loseTime` 3 s (chases the last seen place this long), turn
  rate 360°/s. Its weapon: `damage` 8, `fireRate` 4, `magazine` 12, `reloadTime` 2.
- States: `idle` (stands, faces its spawn's yaw) → sees the player (a ray from its eye to the
  player's chest reaches the player, within `sightRange` and `fovDeg`; or it is hit) → `chase`
  (walks straight at the last seen place at `walkSpeed`; no pathfinding in the slice) → in
  `attackRange` with a line → `attack` (stops, turns to the player, fires bursts after the
  reaction time) → loses the line → `chase` → `loseTime` without sight → `idle`. At 0 health:
  `dead` (its character is removed so it blocks nothing and rays pass; its body lies on the
  floor).
- The AI is a simulate system `enemy_ai` before `physics` (it writes `MoveIntent` and
  `WeaponTrigger` from the last step's state); the sight rays use `raycastFrom`.

## Death, restart, win (step 3)

- `Round` (saved, one per world, on the scene's Controls entity): `restartDelay` 3 s, action
  `restart` (Enter). A world without `Round` never restarts.
- At the first combat step of a world with `Round`, the round starts: a snapshot of the world
  (taken through a `RoundHook` the host installs with its type registry, as `SnapshotHook`,
  since ECS system sources include no `reflect/`) and a `round_start` event.
- Player dead → `restartDelay` later (or `restart` pressed) the step restores the round-start
  snapshot: the world after that step equals the round start (`world.hash` too), with a
  `restart` event in the log (the log is not part of the snapshot) and `Round` counting
  restarts in its runtime state.
- Every `Enemy` dead → `win` event, the HUD says "CLEARED"; `restart` starts again.

## Presentation (step 4)

- **Muzzle flash:** `Weapon.muzzle` (an entity reference, optional) names a `Light` the step
  turns on at `flashIntensity` for `flashTime` (0.05 s) after each round; the play scene puts a
  small point light at the gun's muzzle under the head. Its emitter mesh (SP 2b) is the
  visible flash. Enemies have one too.
- **Gun:** a dark box under the head, right and down of the view (a stand-in view model).
- **Impact marks:** a pool of 64 runtime quads (dark, 8 cm), created on `hit` events at the
  point + 2 mm along the normal, facing it; the oldest is reused. Never saved or hashed. Not on
  characters.
- **Sound:** `assets/audio/weapons/` generated by `tools/audio/make_weapons.py` (ours, MIT,
  as the footsteps): `fire`, `enemy_fire`, `impact_<surface>`, `hurt`, `death`, `reload`,
  `empty`. The window host plays them from combat events beside the footsteps
  (`LiveConfig::footsteps` covers both; `--mute` off).
- **HUD:** a `hud` render pass on the final LDR image (before present and readback) drawing a
  `HudList` the host fills from combat state each frame: rectangles (crosshair spread with
  bloom, health bar, hit marker for 0.15 s after a hit that damaged, a red vignette for 0.3 s
  after taking damage) and text from a built-in 5×7 bitmap font (digits, `A–Z`, `/`). The
  window always draws it; `render.capture` draws it with `hud: true` (default false, so
  goldens and existing captures are unchanged).

## The agent-facing surface (step 5)

Three tools (the 24 KB `tools/list` cap leaves room for about three):
- `combat.info {}` → the round (state, steps, restarts) and every combatant (id, name, health,
  max, dead, weapon: ammo, magazine, reloading, bloomDeg, kickDeg, shots; enemy: state,
  target), in id order.
- `combat.events {since?, max?}` → the log, as `physics.events`.
- `combat.damage {entity, amount, source?}` (mutates) → the target's health after; `E_STATE`
  without combat, `E_NOT_FOUND`, `E_RANGE` for an amount outside 0..10,000.
Firing is `input.inject` (`fire` down/up), the same path the window takes; no `weapon.fire`.

## Performance budgets

`physics_bench` (CPU, `tests/bench/physics_budgets.json`, the same noisy-run rules), each
tagged `gatedFrom` its step:
- `combat_shot_ms` (step 1): the combat system's part of a step in which the player fires one
  round in the blockout (the ray, the hit, the events), median of 200 rounds: ≤ 0.02 ms.
- `combat_step_ms` (step 2): the combat and AI systems' part of a step with two enemies
  attacking and the player firing: ≤ 0.05 ms.
- `enemies16_step_ms` (step 2): the whole step (AI, physics, combat) with 16 enemies chasing
  the player in the blockout: ≤ 1.0 ms.
- `round_restart_ms` (step 3): restoring the round-start snapshot of the play scene: ≤ 2 ms.
- `hud` pass (step 4, `render_bench`): ≤ 0.05 ms at 1080p.

## Determinism

The same scene, seed and inputs give the same rounds, hits, damage, deaths and `world.hash`,
in lockstep and realtime, headless and in a window, run to run; a snapshot mid-fight replays
the rest bit for bit.

## Test plan

New suite `combat_tests` (CPU, label `quick`, `tests/combat/combat_tests.cpp`), test 15:
- Step 1: `t15_damage_and_death`, `t15_fire_rate_ten_hz`, `t15_semi_automatic_one_per_press`,
  `t15_magazine_and_reload`, `t15_reload_key`, `t15_hit_damages_target`,
  `t15_shot_ignores_shooter_and_triggers`, `t15_hit_wall_reports_surface`,
  `t15_spread_cone_distribution`, `t15_spread_blooms_and_recovers`, `t15_spread_is_seeded`,
  `t15_recoil_kicks_and_recovers`, `t15_turret_shoots_player`, `t15_dead_do_not_fire`,
  `t15_runs_are_identical`, `t15_snapshot_replays_a_burst`, `t15_play_scene_has_a_rifle`.
- Step 2 (test 16): enemies spawn at the enemy spawns facing their yaw; idle without sight;
  see and chase; attack in range at the reaction time with bursts; lose the player and return
  to idle; hit wakes an idle enemy; dead enemies block nothing and stop; the player killed by
  enemy fire; two runs identical.
- Step 3 (test 17): restart after the delay equals the round start (`world.hash`, poses,
  health, ammo, crates); restart key; win when all enemies die; a world without `Round` never
  restarts.
- Step 4: impact pool caps at 64 and reuses the oldest; marks never saved or hashed; muzzle
  flash on for `flashTime`; HUD list contents from state; a GPU test that the `hud` pass draws
  only where the list says (captures with and without `hud`); sounds chosen by event and
  surface.
- Step 5: `api_tests` (`sp7_t18_*`), a `combat.jsonl` serve session, the CLI, `check_live.py`
  and `e2e_mcp.py` fight legs: kill both enemies, die, restart, the same fight headless and in
  a window reaching the same `world.hash`.

## Done criteria

- The slice is playable: `ohao_game assets/scenes/blockout_play.scene.json` — fire, reload,
  recoil and bloom you can feel, flash, marks, sounds; enemies that see, chase and shoot; you
  can kill both ("CLEARED"), and die and restart.
- An agent can play the same fight over MCP and see it (`check_live.py`, `e2e_mcp.py`).
- Every gate green; combat budgets met (or noisy, pending a quiet rerun).

## Build order

1. **Weapon, health, damage.** `Health`, `Weapon`, `WeaponState`, `WeaponTrigger`; the combat
   step (fire rate, magazine, reload, spread, recoil, hits, damage, death); `raycastFrom`;
   `turnPlayer`; the event log; `installCombat` in the CLI context and `LiveHost`; the play
   scene's rifle and `fire`/`reload` bindings; `combat_tests` step 1; `combat_shot_ms`.
2. **Enemies.** `PlayerController.spawn`/`input`, `MoveIntent`, `Enemy`, `EnemyState`, the
   `enemy_ai` system, two enemies in the play scene, test 16, `combat_step_ms`,
   `enemies16_step_ms`.
3. **Death, restart, win.** `Round`, `RoundHook`, the restart, the dead player, test 17,
   `round_restart_ms`.
4. **Presentation.** Muzzle flash, the gun, impact marks, sounds, the HUD pass, look shots of a
   fight, a golden with `hud`.
5. **Agent surface and the done demo.** `combat.info`, `combat.events`, `combat.damage`, serve
   and CLI goldens, `check_live.py` and `e2e_mcp.py` fight legs, docs, monograph, roadmap.

Each step ends with the engine running, every gate green, a change record
(`docs/changes/2026-10-27-combat-stepN.md`) and look shots in
`C:\Users\djmax\ohao_refs\look_shots\sp7_combat\` (`stepN_*.png`).

## Risks

- **Rays and characters:** an enemy's eye sits inside its own capsule; `raycastFrom` must skip
  the shooter's inner body (tested in step 1 with the player).
- **Recoil through the look path:** the kick must not read as a placement (no teleport, no
  interpolation break); `turnPlayer` writes as the step does.
- **Restart inside a step:** restoring the registry while systems run. The restore happens in
  the combat system, the last simulate system, and the sync systems after it see the restored
  world; tested by `world.hash` equality.
- **No pathfinding:** enemies walk straight and can stick on the pillar or crates; the
  blockout is small and open. A stuck enemy still shoots when it sees the player.
- **HUD in captures:** off by default so goldens do not move.

## Review focus

- Two combatants killing each other in the same step: id order decides; the second never fires
  (step 1, `t15_dead_do_not_fire`).
- A round fired the same step the magazine empties and `reload` is pressed: one round, then the
  reload, never a round during it.
- Mouse look and recoil in the same step: both apply, the round aims where the view is after
  this step's look (the combat step runs after the physics step).
- A snapshot taken mid-reload or mid-recovery replays the same rounds (all of it is in
  `WeaponState`, saved).
- An edit of `Weapon.magazine` below the current ammo: ammo clamps to the magazine on the next
  step.

## Rulings for Frank

Judgment calls made without Frank, each conservative; any can be reversed.
1. **Fire on the left mouse button, reload on R, restart on Enter.** The play scene also binds
   the left button to `capture_mouse`; the click that captures the cursor is consumed by the
   window host and does not fire (step 4; until then that one click fires one round).
2. **One weapon, hitscan only, infinite reserve ammunition.** Only the magazine is counted.
3. **Combat runs after physics in the step,** so a round aims along this step's look; recoil
   turns the view at once and the next round aims from there.
4. **Counter-based seeded randomness** (`hash(seed, shot, i)`), no generator state.
5. **Health, weapon state and the trigger are saved components,** so they are in scene files,
   `world.hash` and snapshots with no new snapshot machinery; a scene file may omit
   `WeaponState`.
6. **Recoil:** vertical kick recovers after the fire interval, sideways kick does not.
7. **Enemies are characters on the player's code** (`PlayerController` with `input: false`),
   authored in the scene file, not spawned at runtime: saved, hashed, editable by agents.
8. **No pathfinding:** enemies walk straight at the last place they saw the player.
9. **Restart restores a round-start snapshot** through a host-installed `RoundHook`; the event
   log survives a restart.
10. **Dead enemies lie down without collision;** no ragdolls.
11. **Impact marks are quads, not projected decals,** 64 at most, oldest reused, not on
    characters.
12. **The HUD is a new render pass with a built-in bitmap font,** in windows always and in
    captures only with `hud: true`.
13. **Three agent methods** (`combat.info`, `combat.events`, `combat.damage`), firing through
    `input.inject`, to stay under the 24 KB `tools/list` cap.
14. **Budgets** (`combat_shot_ms` 0.02, `combat_step_ms` 0.05, `enemies16_step_ms` 1.0,
    `round_restart_ms` 2, `hud` 0.05) are targets set here; they change only by Frank's ruling.
15. **Sounds are generated** by a script of ours (MIT), like the footsteps; nothing from
    Somnabulate.
