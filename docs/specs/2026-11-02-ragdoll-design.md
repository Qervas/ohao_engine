# Sub-project 13 — Joints and ragdoll: constraints as scene data, the enemy's death as physics

Status: written 2026-11-02, Day 1 of sub-project 13 (documents only; no code), after the frontier renderer closed
(roadmap row 18). Frank's rulings on scope, content and the death's look are written under "Rulings for Frank" as
**PROVISIONAL** (recommended, not yet ruled), and the design follows them. Every other judgment call is listed there as
**OPEN**, with a recommendation. The spec is binding once Frank accepts it.

Roadmap: row 13 of `docs/ROADMAP.md`. Done when a shot enemy falls as a ragdoll and replays bit for bit from a mid-fall
snapshot, and 16 ragdolls have a budget.
- **Before it:** 12, VFX (built, waiting for Frank's look shots and hand test). Nothing here depends on them.
- **After it:** 14, the game module and hot reload. Ragdolls stay engine code (`ohao/ecs`, `ohao/physics`); what decides
  *when* a character falls (combat's death) moves with combat in 14.

Builds on:
- **Sub-project 6** (`2026-10-26-player-physics-design.md`): Jolt in the step (substeps of 1/60 s, no worker threads),
  `PhysicsScene` building one body per `Collider`, write-back through `writePose`, `RenderPose` interpolation, and
  snapshots that carry Jolt's whole state (`SaveState` with `EStateRecorderState::All`: bodies, contacts, constraints).
- **Sub-project 7** (`2026-10-27-combat-design.md`): the combat step, its `hit`/`damage`/`death` events, the enemy AI's
  `die` (the `PlayerController` removed, the character parked by `setCharacterInWorld(false)`), the round's restart
  through snapshots.
- **Sub-project 10** (`2026-10-28-animation-design.md`): the enemy rig (43 joints, 41 bones), the `pose` sync system
  (`AnimPose.model`, `palette`, `prevPalette`, `boneBoxes`), the death clip (OPEN ruling 7 there: "a clip-driven fall is
  what 13's ragdoll will blend from"), and its ruling 18: per-bone hitboxes come with 13's bodies.
- **Sub-project 11** (`2026-10-28-audio-design.md`): `body_fall`, today a death-clip event at 0.55 s.

## Thesis: a ragdoll is bodies the world always had

A character's ragdoll is built **when the character is**, parked out of the broadphase, and only switched on at its
death. Its bodies and constraints therefore have the same handles for the character's whole life, and every snapshot
the World takes (a mid-fight restore, the round's start, a mid-fall replay) finds the same bodies and the same
constraints it saved. That is the condition `PhysicsScene::restoreSaved` already needs to put Jolt's state back exactly
(`bodyIdentity() == saved.bodies`), and the condition Jolt's `ConstraintManager::RestoreState` needs (it restores
constraints by index; it cannot add or remove one). It is the same trick the parked character already plays.

A fall is simulation, not presentation: the bodies' poses are written back to a saved component every awake step, so
`world.hash`, a saved file and an agent see where a body lies, and a replay that drifted is a hash that differs. The
skinned mesh is drawn from those bodies by the `pose` system (presentation, interpolated at render alpha as every
dynamic body is).

Joints are scene data first: one `Joint` component makes one constraint between its entity's body and another's (or the
world). A ragdoll is the same constraints, described by a `.ragdoll.json` asset per rig rather than one entity per bone.

## Goal

On `ohao_game assets/scenes/blockout_play.scene.json`, an enemy killed by the player falls as a ragdoll from the pose it
died in, pushed along the killing round, and comes to rest on the floor, the Step or a crate without passing through
them, popping or jittering. Its `body_fall` sound plays when its torso lands. Rounds fired at a corpse push it. The
round's restart stands it back up at its spawn, bit for bit as today.

A `joints.scene.json` shows the constraints as data: a hinged door with limits, a chain of boxes hanging from the
ceiling, a swinging lamp, a breakable plank.

Everything replays bit for bit from any snapshot (mid-fall included), renders validation-clean in deferred and the path
tracer (shadows and GI see the fallen mesh), and the cost of 16 ragdolls falling at once and at rest is gated.

## Scope: in 13 vs later

In 13:
- the backend's constraints fixed and completed: anchors in each body's local space, as the header says; a swing-twist
  type (the ragdoll joint); limits and motors for every type that has them; enabling and disabling saved with the state;
- `Joint` (saved) and the `physics` sync building, rebuilding and removing constraints with their bodies;
- snapshot identity extended to constraints;
- `.ragdoll.json` (parts on joints, shapes fitted from the skin, swing and twist limits, masses), `ohao schema ragdoll`;
- `Ragdoll` (saved: the asset) and `RagdollState` (saved: active, the step it started, each part's pose);
- parked ragdolls built with their character, activated at a pose with that pose's velocity plus an impulse;
- the `RAGDOLL` layer and its collision rules;
- the `pose` system drawing a skinned mesh from its ragdoll's bodies;
- the enemy's death as a ragdoll; `body_fall` from the torso's landing; rounds pushing corpses;
- `physics.info` constraints, `combat.info` `ragdoll`; tests 36–38; budgets; goldens; look shots; Frank's hand test.

Later:
- **Powered ragdolls** (motors driving the body toward a clip, active ragdolls, getting up): OPEN ruling 6.
- **Per-bone hitboxes** (headshots): OPEN ruling 7 recommends they wait; rounds keep hitting the capsule.
- **The player's death** stays the camera's (no third-person body exists).
- **Soft bodies, cloth, vehicles:** not in this row.

## Why (what the code does today)

- **Constraints exist and nothing uses them.** `IPhysicsBackend` has seven types (`ConstraintType`: fixed, point, hinge,
  slider, cone-twist, distance, six-DOF) and a Jolt implementation (`jolt_backend.cpp` 1130–1274); the only caller is
  `PhysicsWorld::createConstraint`, which nothing in `ohao/ecs` calls. `physics_backend_tests` covers them.
- **The anchors are in world space**, though `ConstraintSettings` says "local space of each body": no setting passes
  `mSpace`, and Jolt defaults to `EConstraintSpace::WorldSpace`. A joint authored in scene data must be in its body's
  space, so a moved door keeps its hinge.
- **Cone-twist has no twist limit** (only `limitMax` as the half cone angle); motors and limits can be changed for hinges
  and sliders only. Jolt's `SwingTwistConstraint` and `Ragdoll` are compiled into the library (Jolt 5.1) and unused.
- **Layer 9 (`RAGDOLL`) is declared and never assigned**; it collides with every layer, itself included.
- **A death today** removes the enemy's `PlayerController` (its character parked), holds the `death` clip's last frame
  at 0.75, and leaves the saved `Transform` upright (SP10 ruling 7). The corpse is a standing capsule nobody can hit
  (parked), drawn lying down.
- **The pose has no override:** `poseEntity` recomputes every joint from the `Animator` each frame; nothing between the
  evaluation and the palette accepts a joint from elsewhere.
- **Combat's events carry no round direction past the shot:** `hit` has the surface normal, `damage` and `death` only a
  point. No code applies an impulse (`applyImpulse` exists in the backend).
- **Snapshots** save Jolt's constraints, but the backend's own constraint map and broken list are not in the state, and
  the identity check covers bodies only.

## Assets and content

- `assets/characters/toon_enemy/enemy.ragdoll.json` (ours, hand-authored, MIT): the parts and joints below, by joint
  name. Shapes are not authored: each part's capsule is fitted at load from the skin (the bind-pose vertices weighted
  above 0.5 to the part's bones, the same data as `AnimPose.boneBoxes`), so a new rig needs names and limits only.
- `assets/scenes/joints.scene.json` (ours): the constraint demo, in the blockout's room.
- `tests/anim/fixtures/` rig: a `rig.ragdoll.json` for the GPU and CPU tests (the rig's few segments as parts).
- No new third-party content.

The enemy's parts (11, OPEN ruling 3):

| Part | Joint | Parent part | Joint type and limits (swing cone / twist, degrees) |
|---|---|---|---|
| pelvis | Hips | — | root |
| spine | Torso | pelvis | swing 30 / twist ±20 |
| head | Head | spine | swing 40 / twist ±45 |
| upper arm L/R | UpperArm.L/R | spine | swing 80 / twist ±60 |
| forearm L/R | LowerArm.L/R | upper arm | hinge 0 to 140 |
| thigh L/R | UpperLeg.L/R | pelvis | swing 70 / twist ±30 |
| shin L/R | LowerLeg.L/R | thigh | hinge 0 to 140 |

Joints that are not parts (Abdomen, Neck, Shoulder, the fingers, the feet) keep their animated local transforms under
the nearest part above them, frozen at the death step's pose.

## The data model

### Backend (`ohao/physics/backend`)

- `ConstraintSettings.space`: `Local` (default, each anchor and axis in its body's space) or `World`. The Jolt backend
  passes `mSpace` accordingly; existing tests state `World`.
- `ConstraintType::SWING_TWIST` (Jolt's `SwingTwistConstraint`): twist axis, plane axis, normal and plane half cone
  angles, twist min and max, optional swing and twist motors (unused in 13, OPEN ruling 6).
- `setConstraintLimits` and `setConstraintMotorState` for every type that has them (hinge, slider, swing-twist,
  six-DOF), else a refusal logged once.
- `createConstraintGroup(span<ConstraintSettings>, groupFilter)`: one call for a ragdoll's constraints, with a Jolt
  `GroupFilterTable` so a part never collides with the parts it is jointed to (Jolt's ragdoll practice), and a
  `setConstraintGroupEnabled(group, bool)` that switches all of them.
- `IPhysicsBackend::saveState`/`restoreState` add the backend's constraint table: handles, enabled flags, broken
  constraints. A restore whose constraint set differs is refused, as a different character set is today.

### Components (`ecs/components.hpp`, registered with docs, units and limits)

- `Joint` (saved): `type` (`fixed`, `point`, `hinge`, `slider`, `swing_twist`, `distance`), `other` (an `EntityId`; 0
  is the world), `anchor` and `axis` in this entity's space, `otherAnchor` and `otherAxis` in the other's (defaults:
  the same world point at creation), `limits` (`min`, `max`, the swing cones), `spring` (frequency, damping),
  `breakForce`, `breakTorque` (0: never). Several joints on one entity are child entities with a `Joint` each (an entity
  holds one).
- `JointState` (saved): `broken` (the step it broke, or none). A broken joint stays broken through saves and hashes and
  comes back with a restore of a snapshot taken before it broke.
- `Ragdoll` (saved): `asset` (a `.ragdoll.json`), `impulseScale` (default 1), `blendSteps` (default 4: OPEN ruling 4).
- `RagdollState` (saved): `active`, `startStep`, and per part `{position, rotation}` in world space, written back every
  step a part is awake (as a dynamic body's `Transform` is); a settled ragdoll writes nothing.
- Runtime, never saved or hashed: `RagdollBodies` (the part body handles and the constraint group), `RagdollPose`
  (each part's last two step poses, for interpolation).

### The physics scene

- **Joints** are built in the `physics` sync after the bodies they join, rebuilt when either body is rebuilt or the
  `Joint` changes, removed with either. A joint between two static bodies, to a missing entity, or to itself is a
  warning (`W_JOINT_*`) and no constraint.
- **Ragdolls** are built in the same sync when an entity has `Ragdoll` and a skinned mesh: one dynamic body per part on
  the `RAGDOLL` layer, kept out of the broadphase, its constraints disabled, until activated. An entity whose
  `RagdollState.active` is true on build (a loaded file, a restore after a rebuild) is built active at the saved poses,
  at rest.
- **Layers:** `RAGDOLL` collides with `STATIC`, `TERRAIN`, `DYNAMIC`, `DEBRIS` and `RAGDOLL` (other ragdolls; its own
  jointed neighbours are excluded by the group filter), not with `CHARACTER` (the player walks through corpses, as
  through crates it pushes: OPEN ruling 5) or `TRIGGER`. Rays (`raycast`, `raycastFrom`) hit active parts.
- **Identity:** `bodyIdentity()` includes the parts (parked or not), and a new `constraintIdentity()` (sorted handles
  with their entity) joins it in `SavedSimulation`. Since parts and constraints live as long as their character, a
  restore anywhere in a round finds them all.

## The step

Order: `physics` (simulate: the substeps, write-back) → `combat` → `impact_marks` → `animation` → **`ragdoll`** →
`vfx` → `audio_schedule`.

- **Activation.** The `ragdoll` simulate system reads the step's combat `death` events after its last `restart`. For a
  dead entity with `Ragdoll`:
  1. Evaluate its animator at this step with alpha 1 (the pose a capture of this step shows; the same CPU code as the
     `pose` system) and at the step before; place each part at its joint (the model matrix times the part's offset), with
     the linear and angular velocity of the difference over 1/60 s.
  2. Add the parts to the broadphase, enable the constraint group, set `RagdollState.active` and `startStep`.
  3. Apply the killing round's impulse: `combat::kRoundImpulse` x `impulseScale` along the round's direction, at the
     round's point on the part nearest it. `death` events gain the round's `dir` (combat writes it; `hit` gains it too).
  The bodies move from the next step's `physics`. Nothing else about the death changes: `die` parks the character one
  combat step later as today, and the character's layer never collides with `RAGDOLL`.
- **Corpses.** A round whose ray meets an active part applies the same impulse there (no damage: the dead take none) and
  writes its `hit` event with the part's entity, so effects and sounds follow as on any surface (`surface: flesh`).
- **Write-back.** After the substeps, every awake active part writes its pose into `RagdollState` (one `patch` per
  ragdoll per step, never per part), and the entity's `Transform` follows the pelvis's position (its rotation upright,
  its yaw kept) so the entity's bounds, the listener and `combat.info` `pos` stay where the body is.
- **`body_fall`**: the first contact of the spine or pelvis with a non-ragdoll body above 1.5 m/s after activation
  writes a `body_fall` event (the physics log), which the schedule plays as the clip event did. The death clip's event is
  removed from the graph.
- **Restart and restore.** A restore puts `RagdollState` back with the registry, and the snapshot's simulation (bodies,
  constraints, enabled flags) back with Jolt's state: an enemy restored alive has its parts parked again, one restored
  mid-fall continues from the same bits.

## The frame

- The `pose` system, for an entity whose `RagdollState.active` is true: each part's joint model matrix is the part's
  interpolated pose (`RagdollPose`, last two steps at render alpha; alpha 1 in lockstep and captures) times the part's
  offset inverse, in the entity's space; a non-part joint is its parent's model times its local transform frozen at the
  start step's pose. Then the palette as today.
- **Blend** (OPEN ruling 4): for `blendSteps` after `startStep` the drawn pose is the animated pose slerped toward the
  ragdoll's by `(steps since start + alpha) / blendSteps`; past it the ragdoll alone. Since the parts start at the pose,
  the blend only hides the first steps' constraint correction; it is presentation, never in the step.
- Motion vectors: `prevPalette` is the previous sync's, as today; activation is not a placement (no reset).
- RT: the skinned instance's BLAS is refit from the deformed vertices as for any pose; a lying body may stretch a bind
  box, which skinning's bounds pass already handles (`t24_cull_parity_skinned`).

## The agent-facing surface

- `physics.info` gains `constraints` (count, broken count) and `ragdolls` (active, awake).
- `combat.info`: a dead combatant's `ragdoll: {active, asleep, pos}` (the pelvis), within the existing size budget.
- `component.get`/`world.query` read `Joint`, `JointState`, `Ragdoll`, `RagdollState` as any saved component; `ohao
  schema ragdoll` exports the asset's schema.
- No new tools: `tools/list` stays under its 28 KB cap.

## Performance budgets

`physics_bench`, gated by a new `ragdollStep` key (proposals, confirmed from step 2's measurements before they gate,
OPEN ruling 8):

| Key | What | Budget |
|---|---|---|
| `chain_step_ms` | the joints scene's 20-box chain swinging, a whole step | 0.10 ms |
| `ragdoll_fall_step_ms` | one enemy falling, the step's physics part | 0.10 ms |
| `ragdolls16_fall_step_ms` | 16 enemies killed in one step in the blockout, the step during the fall (the worst of the first 60) | 1.5 ms |
| `ragdolls16_rest_step_ms` | the same 16 asleep | 0.05 ms |
| `ragdolls16_parked_step_ms` | 16 living enemies with parked ragdolls against the same without `Ragdoll` (the parked cost) | +0.02 ms |
| `ragdoll_activate_us` | one activation (two pose evaluations, 11 bodies placed) | 50 µs |

`render_bench` `anim16_1080p` keeps its skinning budgets with the 16 rigs ragdolled (`skin` + `skin.bounds` 0.25 ms,
`rt.blas.refit` 0.6 ms).

## Determinism

- **Simulation:** Jolt on one thread is deterministic for one binary on one machine (cross-platform determinism stays
  off: OPEN ruling 9). Two runs of the same fight give the same `RagdollState` bytes and `world.hash`; a restore at
  any step (alive, the death step, mid-fall, at rest) replays the rest bit for bit; a restart is the round's start.
- **A rebuilt body** (a file loaded mid-fall, a ragdoll whose asset changed) falls back to the saved poses at rest, as
  `BodyMotion` does for dynamic bodies; a file is not a snapshot.
- **Presentation:** the pose from `RagdollState` and `RagdollPose` at alpha 1 in captures: deferred captures of a
  ragdoll bit-identical across hosts, the path tracer within its BVH floor.
- **Hashes:** four new saved types move every `world.hash` expectation once (saved types hash their names); the play
  scene's goldens move only if an enemy dies in their steps (`cli_blockout_play_hud` at step 45 does not: checked in
  step 3).

## Test plan

**Test 36, joints** (`level_physics_tests`, `t36_*`): a hinge's anchor in local space (the door moved before the
build swings about its own edge); the hinge's limits held at 0 and 110 degrees under a push; a 1 m pendulum's period
within 1% of 2π√(L/g) at small angles; a swing-twist cone and twist limit held; a chain of 20 boxes stretching under
0.5%; a plank's joint breaking at its `breakForce` and `JointState.broken` set at that step; a removed entity removing
its joints (no dangling constraint, `physics.info` count); the backend's state refusing a restore with another
constraint set; a swinging chain restored mid-swing replaying bit for bit (pose and hash), and a broken joint restored
from before it broke whole again; two runs identical.

**Test 37, ragdolls** (`combat_tests` and `level_physics_tests`, on the rig, then the enemy, `t37_*`): a parked ragdoll
costs no contact, no ray hit, no write; activation places every part within 1e-5 m of its joint in the pose of the
death step, with that pose's velocity; the killing round's impulse moves the pelvis away from the shooter; parts never
collide with their jointed neighbours, and do collide with the floor, the Step, a crate, another ragdoll; the body comes
to rest within 3 s and then writes nothing; every joint within its limits (a tolerance of 2 degrees) through the fall;
no part below the floor's surface by more than the 5 mm slop; `body_fall` once, at the torso's landing step; a round
into a corpse moves it and damages nothing; a restore at the death step, mid-fall and at rest replays bit for bit; the
restart puts the round's start `world.hash` back and the parts parked; a file saved mid-fall loads with the bodies at
the saved poses, at rest; the player walks through a corpse; two runs and the agents' fight (`combat.damage`) identical.

**Test 38, the drawn ragdoll** (`skinning_gpu_test`, `cull_parity_gpu_test`, `deferred_determinism_gpu_test`,
`t38_*`): the deformed range read back equals a CPU skin of the ragdoll's palette; the blend's first frame equals the
animated pose's (no pop: palette difference 0 at alpha 0 of the start step); a fallen rig culled on = off, mesh shaders
= fallback = instances; the path tracer and RT GI see the fallen mesh (its shadow on the floor where deferred shows
it); a lockstep window capture of a fall equals a headless one; captures bit for bit across processes (deferred).

`check_live`, `e2e_mcp`: an enemy shot down falls (`combat.info` `ragdoll.active`), a restore mid-fall and the same
steps reach the same `world.hash`; `check_validation` adds the play scene a second after a kill, raster and RT.

## Hand-test acceptance (from Frank)

| # | What | Pass |
|---|---|---|
| R1 | Kill an enemy standing, running, on the Step | it falls from the pose it was in, no pop, no float |
| R2 | Kill one against a wall and beside a crate | no limb through the wall or crate, no explosion |
| R3 | Shoot the corpse | it moves with the round, believably heavy |
| R4 | Watch it rest | no jitter or creep after it settles |
| R5 | Walk over it | the player is not blocked or launched |
| R6 | Die and restart, kill again | the same enemies stand, fall again |
| R7 | `body_fall` | it sounds when the body hits the floor, not before |
| R8 | The joints scene | the door, chain, lamp and plank read as physical |

## Done criteria

- A shot enemy on the play scene falls as a ragdoll from its pose, pushed by the round, rests on the level, and replays
  bit for bit from a mid-fall snapshot.
- `joints.scene.json` holds its door, chain, lamp and plank as scene data.
- Tests 36–38 pass; every other simulation test unchanged except the stated death edits; validation clean in raster
  and RT.
- The ragdoll budgets gated; `check.py --all` green.
- The hand-test table closed by Frank's notes.

## Build order

1. **Joints:** the backend's local anchors, swing-twist, limits and motors for every type, the constraint table in the
   saved state; `Joint`, `JointState`, the `physics` sync's joints, constraint identity; `joints.scene.json`; test 36;
   `chain_step_ms`. Nothing in the play scene changes.
2. **Ragdolls on the rig:** `.ragdoll.json` and its schema, shapes fitted from the skin, `Ragdoll`/`RagdollState`,
   parked bodies, the `RAGDOLL` layer and group filter, activation by a test's call, write-back, the `pose` override and
   blend; tests 37 and 38 on the rig; the ragdoll benches pending.
3. **The enemy's death:** the enemy's ragdoll asset, `death`'s round direction, activation from combat, corpses pushed,
   `body_fall` from the landing, the graph's death states replaced (OPEN ruling 2), the play scene's goldens and hashes,
   agents' fields, test 37 on the enemy, the budgets gated.
4. **Close:** `check.py --all` and the benches; look shots in `C:\Users\djmax\ohao_refs\look_shots\sp13_ragdoll\`; the
   change records; AGENTS.md, ARCHITECTURE.md ("add a joint", "add a ragdoll"), the guides; the roadmap row; Frank's hand
   test.

Each step ends with the engine running, every gate green and a change record
(`docs/changes/YYYY-MM-DD-ragdoll-stepN.md`).

## Risks

- **Disabled constraints between parked bodies.** Jolt solves a constraint only between active bodies, and a disabled
  one not at all; whether `Constraint::SaveState` carries the enabled flag decides whether a restore across an
  activation is exact. Step 1 tests it first (`t36_*` restore across an enable); if not, the backend saves the flags
  itself.
- **Parked bodies in the identity.** Bodies outside the broadphase still count in `SaveState`; Jolt restores them in
  place. A ragdoll added after a snapshot (an enemy spawned mid-round) changes the identity and falls back, as any new
  body does today.
- **Pose evaluation in the step.** The death pose is evaluated in the simulate phase; it must be the same function the
  `pose` system runs at alpha 1, or activation pops. One code path, tested by `t38` no-pop.
- **Stiff chains and light limbs.** Mass ratios over 10:1 between jointed parts make Jolt's solver soft; masses come
  from fitted volume at one density, clamped to a 4:1 ratio between neighbours. More solver iterations
  (`mNumVelocitySteps`) only for ragdoll bodies if the limits test fails.
- **The sleeping corpse.** A ragdoll that never sleeps writes every step and breaks `ragdolls16_rest_step_ms`; Jolt's
  sleep thresholds are per body and may need the ragdoll's own.

## Review focus

- A death and a restart in the same step; a death on the round's last enemy (the win) and its restart.
- A restore to before a death, then a different death: the same parts, activated at another pose, no leftover velocity.
- An enemy removed (destroyed) mid-fall: its parts and constraints removed with it, the identity changed, the next
  restore falls back cleanly.
- A joint whose `other` entity is destroyed and recreated with the same id by a restore.

## Rulings for Frank

PROVISIONAL (recommended; the spec is designed to them):
1. **Scope:** passive ragdolls for the enemy's death, constraints as scene data, hit impulses, exact replay, the 16-ragdoll
   budget. Not powered ragdolls, per-bone hitboxes, cloth or vehicles.
2. **The death clip goes:** a death activates the ragdoll at once (with the blend of ruling 4); the graph's `death` and
   `dead` states become one `ragdoll` state that plays nothing, and `body_fall` comes from the landing. SP10's tests that
   assert the death clip's end change to the ragdoll's activation. Forward only: no clip-then-ragdoll path is kept.
3. **Content:** our own `.ragdoll.json` per rig, shapes fitted from the skin; no third-party ragdoll data.

OPEN (judgment calls; recommendations given):
4. **The blend** of 4 steps (67 ms) from the animated pose to the ragdoll's. Recommended: the parts start at the pose, so
   the blend only hides solver correction; 0 if the hand test sees no pop.
5. **Corpses and the player:** the player walks through them (no `CHARACTER`–`RAGDOLL` collision). Recommended: blocking
   bodies on the floor trap players in corridors; shooters do this.
6. **Powered ragdolls** (motors toward the death clip for a staggered fall) later. Recommended: a passive fall first, then
   motors if Frank's hand test finds the falls too limp.
7. **Hitboxes stay on the capsule** in 13 (SP10 ruling 18 deferred them here). Recommended: per-bone hitboxes need the
   pose in every step for every living enemy (an evaluation per step, not only at death) and a damage table by bone;
   a row of their own when the combat design asks for headshots.
8. **The budget numbers** are proposals until step 2 measures them.
9. **Cross-platform determinism** stays off (`CROSS_PLATFORM_DETERMINISTIC`). Recommended: replay is promised per build
   and machine, as for every physics result today.
10. **11 parts** for the enemy (no hands, feet, neck or abdomen bodies). Recommended: the common game ragdoll; more
    parts cost solver time for little visible change at this rig's scale.
11. **Rounds push corpses** with the same impulse as the killing round. Recommended: reads as weight; turn it off by
    `impulseScale` 0 per enemy.
