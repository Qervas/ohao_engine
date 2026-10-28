---
module: physics
id: combat
title: Combat: rounds, enemies, the round and the agent's fight
standard: v2
---

## A round is a ray in the step

Combat is more simulation, not a separate loop. `installCombat` adds a `combat` simulate system
after `physics`, so every `World::simulate(dt)` (the window's fixed step, an agent's
`world.step`) runs it once. Every `Weapon`, in id order, reads its trigger, advances cooldown,
reload, bloom and recoil, and fires at most one round. The cooldown carries its remainder, so a
10 Hz rifle fires on every sixth 60 Hz step without drift:

{{cite ohao/ecs/combat.cpp "s.cooldown += 1.0f / w.fireRate;"}}

A round is one ray from the shooter's eye. It must not stop inside the shooter's own capsule or
in a trigger volume, so the physics scene casts it past both:

{{cite ohao/ecs/combat.cpp "if (const auto hit = physics.raycastFrom(eye, dir, w.range, e)) {"}}

What it hits, or that entity's nearest ancestor with `Health`, takes the weapon's damage; at 0
it is dead, takes no more and fires no more.

## Spread without a generator

The round leaves within a cone that blooms with sustained fire. Its direction is drawn
uniformly over the cone's disc one unit ahead, from random numbers that are a hash of
`(seed, shot, i)`:

{{cite ohao/ecs/combat.cpp "const glm::vec3 dir = combat::spreadDirection(aim, cone, w.seed, shot);"}}

There is no generator state to save, so a snapshot mid-burst replays the same rounds. Uniform
over a disc of radius $R$ means a radius drawn as $R\sqrt{u}$, whose density and mean are

$$p(r) = \frac{2r}{R^2}, \qquad \mathbb{E}[r] = \int_0^R r \, \frac{2r}{R^2} \, dr = \frac{2}{3} R$$

$r$ is a round's distance from the aim on that disc and $R = \tan(\text{cone})$; for small
cones the angle is proportional to $r$, so the mean angle off the aim is two thirds of the
cone (`t15_spread_cone_distribution` draws 10,000 rounds and checks it within 3 %).

## Aiming down the sights tightens the cone

Holding `aim` (the right mouse button) steps the saved `WeaponState.aim` from 0 toward 1 at
$\Delta t / t_{ads}$ a step ($t_{ads}$ = `Weapon.adsTime`, 0.2 s), and back once released. It is
updated before the step's round, and it drops for the dead and while the weapon reloads (the
arms leave the sights for the reload clip):

{{cite ohao/ecs/combat.cpp "aiming = aiming && !(s.reloading > 0.0f);"}}

The cone narrows linearly with it, to half at full aim:

{{cite ohao/ecs/combat.cpp "const float cone = (w.spreadDeg + s.bloomDeg) * (1.0f - 0.5f * s.aim);"}}

$$\theta = (\theta_{spread} + \theta_{bloom})\,(1 - \tfrac{1}{2}\,a)$$

Linear means the 0.2 s ramp is not a dead zone and there is no cliff on its last step; at
$a = 0$ the cone is the old one bit for bit, so enemies, which never aim, fire as before. The
drawn view narrows too, from the camera's 75° toward 55°, but only in a realtime window's frames
(presentation; `runtime/feel`). Captures and lockstep frames keep 75°. Both values and the linear
cone are provisional (ruling 11), as is the view kick: the play scene's first-person arms carry a
fire clip of their own, so the procedural kick plays at half amplitude on top of it (`kickBack`
0.015 m, `kickPitchDeg` 1.25°; ruling 8).

## Enemies are players without a keyboard

An enemy is a character on the player's code: a `PlayerController` with `input: false`, moved
by a saved `MoveIntent` the enemy AI writes at the start of the combat step, and firing through
a saved `WeaponTrigger`. Everything the AI decides is in components, so files, `world.hash` and
snapshots hold a fight exactly. A dead enemy loses its controller; its `Transform` stays upright
and its animation's death clip is the fall (sub-project 10):

{{cite ohao/ecs/enemy_ai.cpp "reg.remove<PlayerController>(e);"}}

Its character is parked out of the broadphase, not destroyed, so it blocks nothing and rays pass
where it stood:

{{cite ohao/ecs/physics_scene.cpp "backend->setCharacterInWorld(it->second.character, false);"}}

## The round goes back to its start exactly

A world with a `Round` snapshots itself at the end of its first combat step. The player dead for
`restartDelay` (3 s), or `restart` pressed, restores that snapshot at the end of the step, inside
the combat system, so the sync systems after it see the restored world:

{{cite ohao/ecs/combat.cpp "hook->restore(roundStart);"}}

The parked characters come back as the same characters, so the snapshot's simulation (bodies,
contact cache, characters) restores and the play after a restart is the play after the start.
One more thing had to hold: `world.hash` after the restart step equal to the start's. A
returning character is placed where its restored entity stands, and placing it once wrote the
rotation again from the yaw read back. The play scene's enemies face exactly 180 degrees, and
`atan2` reads a quaternion stored at -180 back as +180: the same rotation, another quaternion,
another hash. A restored entity now keeps its rotation bit for bit:

{{cite ohao/ecs/physics_scene.cpp "if (sameRotation(rotNow, p.writtenRot, 1e-4f) && rotNow != p.writtenRot) {"}}

:::why
A restart could re-spawn everything from the scene file. It would be simpler and would lose
what the round's first step did (the enemies already turned toward the player, a crate already
settled), and nothing would check it. Restoring a snapshot makes "the round's start" one thing,
a world with a hash, and every restart is tested against it.
:::

## What the player sees is built at the step

The muzzle flash is a light the step turns on for `Muzzle.time`; the HUD is a list of
rectangles and glyphs laid out from combat state; impact marks are 64 runtime quads attached to
what was hit. A hit leaves a decal record, a projected box on the mesh box the round crosses (a
character's) or on what it hit. The `vfx` system takes the records from the log right after `combat`,
against the step's poses:

{{cite ohao/ecs/vfx.cpp "hitBoxes(reg, character, e.point - dir * 0.05f, dir, best, 0);"}}

A headless host makes its renderer at its first capture. When marks were taken by the renderer,
it read the whole log then and attached each mark by its target's pose *then*: an enemy that had
moved since carried its marks elsewhere, and a window and a headless capture of the same
fight differed. Taken at the step, they are the same picture.

## An agent's fight

Three tools fit beside the others under the 24 KB `tools/list` cap: `combat.info` (the round and
every combatant: health, feet, weapon, enemy state), `combat.events` (the log, numbered from 1 as
`physics.events`) and `combat.damage`. Firing stays `input.inject`, the path a window takes.
`combat.damage` takes hit points as a round would, from the entity's `Health` or an ancestor's:

{{cite ohao/api/combat.cpp "const float after = combat->damage(target, p.amount, source, positionOf(reg, owner));"}}

`check_live.py` plays a whole round this way: EnemyA shot down (aim with `player.teleport`, fire
bursts), the player killed, the restart, the room cleared and won. The same fight in a hidden
window and headless reaches the same `world.hash`, the same `combat.info` and events, and the
same picture with the HUD, bit for bit.

:::key
Combat runs in the world's step after physics: one ray a round, past the shooter and triggers,
spread drawn from a hash of (seed, shot), all weapon and enemy state in saved components. A
round restarts by restoring the snapshot of its first step, bit for bit, parked characters and
rotations included. What the player sees is built at the step too, so every host draws the same
fight, and an agent plays it through three tools and input.
:::

## Contracts

- Combat events go to a log of 1,024 that survives restarts; `combat.events` numbers them from 1
  and reads after `since`, as `physics.events`.
- Damage outside a step (`combat.damage`) takes effect at once; a dead enemy plays its death clip
  from the next step.
- The round's start is the world at the end of its first step: damage dealt before that step is
  in the start, and a player dead there stays dead after each restart.
- Impact marks, the HUD and sounds are never saved or hashed.
