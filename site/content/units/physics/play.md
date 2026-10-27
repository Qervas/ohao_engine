---
module: physics
id: play
title: Physics in the step, the player and replay
standard: v2
---

## One clock, inside the world

Physics does not have a loop of its own. `installPhysics` adds a `physics` simulate system, so
every `World::simulate(dt)` steps it: the window's fixed step, an agent's `world.step`, a
headless `ohao run --steps`. A step propagates dirty transforms, brings bodies up to date with
the entities, moves the players, then steps Jolt in substeps of at most 1/60 s:

{{cite ohao/ecs/physics_scene.cpp "for (int i = 0; i < n; ++i) backend->step(dt / float(n));"}}

Awake dynamic bodies are written back to their entities' `Transform`s, and the step's contact,
trigger, footstep and landing events are sorted and logged for `physics.events`. Because there
is one clock, record and replay, lockstep captures and `world.hash` all see physics the way
they see any other system.

## Telling its own writes from everyone else's

The scene writes `Transform`s (the write-back, the player) and so does everything else: an
agent's `world.apply`, a snapshot restore, a parent moving. A body whose entity was moved by
anyone but the scene must jump there at rest; one the scene moved must not be touched. The
first build compared the entity's pose with the one last written, within 1e-4. A restore found
that wrong: a crate settles 4.2 micrometres into the floor, a restore puts its `Transform`
back to the snapshot's, and the difference fell inside the tolerance, so the body stayed asleep
where it was. The scene now marks its own writes and treats any other as a placement:

{{cite ohao/ecs/physics_scene.cpp "if (!posedExternally.erase(e) && !differs(p.position, r.wpos, 1e-4f) && sameRotation(p.rotation, r.wrot, 1e-4f))"}}

The tolerance is left only for poses that change through a parent.

## The player

A `PlayerController` is a Jolt `CharacterVirtual` whose position is the feet, moved in the step
before Jolt's substeps by the actions it names. Vertical motion is integrated exactly: the move
takes the step's average vertical velocity,

{{cite ohao/ecs/physics_scene.cpp "const float vMove = falling ? p.vy + 0.5f * gravity.y * dt : 0.0f;"}}

so a jump follows the parabola it would in continuous time and peaks where the formula says:

$$h = \frac{v_0^2}{2g} = \frac{4.9^2}{2 \cdot 9.81} \approx 1.22\ \text{m}$$

$v_0$ is `jumpSpeed` (4.9 m/s), $g$ gravity and $h$ the rise of the feet; `t11_jumps_one_point_two_two_metres`
measures 1.2238 m. A character placed anywhere (made, teleported, restored) finds its ground at
once, not at its next move:

{{cite ohao/physics/backend/jolt/jolt_backend.cpp "character.RefreshContacts({}, layerFilter, {}, {}, *m_tempAllocator);"}}

Without it, `player.info` right after a load said the player was in the air, and a replay from
the spawn started one step differently from the run it replayed.

## Snapshots carry the simulation

A snapshot holds the saved components, the entity table and the id generator. That is not
enough to replay physics. The first answer was `BodyMotion` (velocities and sleep, a runtime
component snapshots hold), and it replays free falls and jumps exactly. It does not replay crates
resting on each other: Jolt warm-starts each contact from the previous step's impulses, kept in
its contact cache, and a teleport keeps the cache. Resting crates replayed two to four hundredths
of a millimetre off, and a world hash missed.

So a snapshot also carries what a system cannot express as components. A system puts an
`ecs::SnapshotHook` in the registry's context, and `restoreSnapshot` hands back what `save`
returned, after the components:

{{cite ohao/ecs/snapshot.cpp "if (const auto* hook = world.registry().ctx().find<SnapshotHook>(); hook && hook->restore) hook->restore(s.systems);"}}

The physics scene's is Jolt's whole state, bodies (with sleep timers) and the contact cache,
then each character:

{{cite ohao/physics/backend/jolt/jolt_backend.cpp "m_physicsSystem->SaveState(rec, JPH::EStateRecorderState::All);"}}

plus the players' records and the contact bookkeeping the events are built from. The next sync
after a restore puts it back, before any teleport, if the bodies are still the snapshot's:

{{cite ohao/ecs/physics_scene.cpp "if (!backend || !s || s->backend.empty() || bodyIdentity() != s->bodies || s->players.size() != players.size())"}}

A body rebuilt since (its collider edited) has a new handle; then nothing is restored from the
saved state and the components' path applies: teleports at rest, then `BodyMotion`. Saving the
state of 1,000 boxes costs about half a millisecond (`boxes1k_snapshot_ms`).

:::why
A component per body was the spec's plan, and it is still what agents read and what a partial
restore falls back to. But the state that decides a resting contact belongs to the pair, not to
either body, and Jolt already knows how to save and restore all of it consistently. Carrying
Jolt's own state through one hook keeps the snapshot module free of physics and makes "restore,
then the same input" a bit-for-bit replay. That is what `input.replay` checkpoints and agents'
`world.hash` comparisons rely on.
:::

## What an agent drives

`player.info`, `player.teleport` and `physics.contacts` sit beside `physics.events` and
`physics.raycast`. `scene.load` spawns the characters and builds the bodies, so the snapshot an
agent takes after a load is the whole simulation at the spawn:

{{cite ohao/api/session.cpp "puts its characters at their spawns and builds the"}}

A teleport goes through the scene, not through `Transform`, so the reply already says where the
player stands and on what:

{{cite ohao/api/level.cpp "physics->teleportPlayer(e, p.pos, p.yawDeg);"}}

:::key
Physics steps inside `World::simulate`. The scene tells its own `Transform` writes from
placements by marking them, not by comparing poses. Players are characters moved before the
substeps with exact vertical integration, grounded at once wherever they are put. Snapshots
carry Jolt's whole state through a `SnapshotHook`, so restores replay bit for bit, resting
contacts included, and fall back to teleports and `BodyMotion` when bodies were rebuilt.
:::

## Contracts

- A restore uses the saved simulation only when every body has the snapshot's handle and entity.
  A rebuilt body, or a player made or removed since, means teleports at rest plus `BodyMotion`:
  exact for free motion, not for resting contacts.
- `world.hash` and `world.diff` cover the saved state only: `BodyMotion` and the saved
  simulation are neither hashed nor diffed.
- Without a seed, a load seeds the id generator at random and `world.hash` covers it, so compare
  hashes within one session or load with a seed.
- `Snapshot::bytes()` counts the columns, not the saved simulation.
