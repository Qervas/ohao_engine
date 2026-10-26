# Sub-project 6 — Player: physics in the world, a first-person controller, you can walk the level

Status: written 2026-10-26 on Frank's "go" for SP6. Frank's ruling, binding on every step:
**physics includes events** (contact begin/end and trigger enter/exit), first-class in the live
world, readable by ECS systems, tests and agents; not bodies only. The contact log of the
`level_physics_tests --drop` helper (23e737d) was a preview and is replaced by this.
Frank delegated the rest of the technical choices; this document makes them. It is the binding
spec once Frank accepts it; the questions only he can answer are under "Open decisions".

Roadmap: row 6 of `docs/ROADMAP.md` ("Jolt at fixed step in the ECS, physics ↔ transform
sync, first-person controller, camera, footsteps"; done when you can walk the level). Before
it: 5 (level pipeline; its step 6, kit2k, is deferred). After it: 7 (combat).

Builds on:
- **Sub-project 1** (`2026-10-02-runtime-shell-design.md`): the fixed-step clock (`Timeline`,
  `core/timeline.hpp`), the window, input actions.
- **Sub-project 4** (`2026-10-23-agent-control-surface-design.md`): system phases (simulate
  once per fixed step, sync once per render), `world.step`, `input.inject`, record/replay,
  `world.hash`, `ohao serve`, MCP.
- **Sub-project 5** (`2026-10-26-level-pipeline-design.md`): `Collider`/`RigidBody`, the
  `PhysicsScene` (`ecs/physics_scene.hpp`) with its `physics` sync system that builds one Jolt
  body per collider from dirty state, spawn points, `physics.raycast`/`physics.info`.

## Thesis: one simulation, observable

A game world has one simulation: the world's fixed step. Physics is a system in it, not a
second clock. Everything physics decides (where a body is, what touched what, what entered
which volume) is visible as world state or as events in the same step, to gameplay systems,
to tests and to agents, with the same numbers in all three. A world at rest costs nothing.

## Goal

- Dynamic bodies move in every host (`ohao run --steps`, `ohao serve`, the window) at the
  world's fixed step, their entities' `Transform`s follow, and a world at rest does no work.
- Contacts and triggers are events: begin/end per body pair, enter/exit per trigger and body,
  with the step they happened in, the point, normal and impact speed.
- A player spawns at the level's `player` spawn point, walks, runs, jumps, climbs the step and
  the ramp, cannot pass walls or crates, pushes nothing it should not, and looks through a
  first-person camera; footsteps sound by surface.
- An agent can do all of it over MCP: step, read events, read the player, drive it with
  `input.inject`, look with `render.capture`.

## Scope: in 6 vs later

In 6: the fixed-step physics system; physics → transform sync; contact and trigger events;
triggers on colliders and level hints; kinematic bodies driven by their entities; the
first-person controller (Jolt `CharacterVirtual`); the camera; footsteps (events and audio);
render interpolation; physics velocities in snapshots; the agent methods below; budgets.

Later: ragdolls, vehicles, constraints authored in levels, physics materials beyond friction
and restitution and a surface tag, networked play, crouch/prone/lean/mantle (7 or later as
combat needs), animation-driven characters, multithreaded stepping (the backend runs
single-threaded on purpose, below).

## Why (what the code does today)

- `PhysicsScene` builds bodies (step 4 of SP5) but nothing steps them: its header says so, and
  `physics.info.unstepped` counts the dynamic bodies left frozen.
- `World::simulate(dt)` already runs once per fixed step in every host: the window's
  `Application` from its `Timeline` (1/60 s, at most 5 steps a frame, the rest discarded),
  `world.step` and `ohao run --steps` in lockstep. A physics system there inherits the clock,
  record/replay and lockstep for free.
- The Jolt backend queues contact callbacks (thread-safe, a mutex-guarded vector) and replays
  them to one listener after `Update`; `OnContactAdded/Removed` arrive per sub-shape pair, so
  a compound body touching the floor with two hulls reports two begins. Nothing reads them in
  the world; with no listener the queue grows without bound once something steps.
- The backend has a `CharacterVirtual` (create, update with `ExtendedUpdate`, ground state);
  unused.
- The transform system sets `prevWorld` (motion vectors) whenever it recomputes an entity.
  Running it inside a step would make a frame's motion vector one step long instead of the
  frame's. Physics needs current world transforms before it steps (a gameplay teleport, an
  agent's `field.set`), so this has to be split (below).

## The fixed step

- **One clock.** The world's fixed step is the physics step. `installPhysics` adds a simulate
  system `physics`, appended after the engine's simulate systems (spin, fly controller) and
  before any added later, so gameplay added later reads the step's events and poses.
- **The accumulator** stays where it is: the host's `Timeline` (`fixedStep` 1/60 s, `maxSteps`
  5 per frame; time past that is dropped and reported as `discarded`). Physics keeps no second
  accumulator.
- **Substeps.** A step of `dt` runs `n = ceil(dt / (1/60 s) - 1e-4)` Jolt updates of `dt / n`
  (one at the default), at most 8: a world stepped with `dt` 0.1 is integrated at 1/80 s, not
  in one 100 ms leap. A longer `dt` is integrated as 8 updates (and logged once at Warn).
- **Order inside a step**, in the `physics` simulate system:
  1. *Propagate*: world transforms of entities whose `Transform` changed since the last
     propagation (`propagateTransforms`, below), so a teleport or an agent's edit this step
     is where the body starts.
  2. *Build*: the existing `PhysicsScene::sync` (bodies for new or changed colliders, moved
     static/kinematic bodies follow, teleported dynamic bodies jump).
  3. *Step*: the substeps.
  4. *Write back*: every awake dynamic body's pose into its entity's `Transform` (below).
  5. *Events*: the step's contact and trigger events, collapsed and sorted (below).
- **Determinism.** Jolt runs single-threaded (as in SP5), bodies are built in creation order,
  every write is in a deterministic order and events are sorted, so the same scene and the same
  inputs give the same poses, events and `world.hash` on this machine, in any host. Across
  machines Jolt would need `JPH_CROSS_PLATFORM_DETERMINISTIC`; out of scope.

### Transforms inside a step: `propagateTransforms`

The transform system becomes two entry points over the same code:
- `updateTransforms(reg)` (the `transform` sync system, once per render, unchanged): settles
  last frame's movers, recomputes dirty subtrees, sets `prevWorld` to the frame's start.
- `propagateTransforms(reg)` (inside a step): recomputes dirty subtrees only. An entity's
  `prevWorld` is captured the first time it is recomputed after a render and left alone by
  later propagations in the same frame (a set in the registry's context records who was
  captured; the next `updateTransforms` clears it). So a frame of three steps gives motion
  vectors spanning the three steps, as now.

## Physics ↔ transform sync

| Motion | Direction | How |
|---|---|---|
| Static | entity → body | as SP5: a moved entity moves its body; a rescaled one is rebuilt |
| Kinematic | entity → body | step 1: as static (teleport). Step 2: `MoveKinematic` to the entity's pose over the step, so it pushes dynamic bodies and carries the player |
| Dynamic | body → entity | after each step, every awake body's pose is written to its entity |

- **Write back.** The body's world position and rotation become the entity's local
  `Transform` through its parent's world transform (position by the inverse parent matrix,
  rotation by the inverse parent rotation; the local scale is kept), written with
  `registry.patch<Transform>` so the renderer, the hash and every observer see it. A body
  under a parent with non-uniform scale is approximate and warns once
  (`W_PHYSICS_PARENT_SCALE`). Sleeping bodies are not written, so a settled level writes
  nothing.
- **Teleports.** The scene remembers the pose it last wrote for each dynamic body. When the
  transform system tags `PhysicsMoved` on a dynamic body's entity, the scene compares the
  entity's world pose to that pose (1e-4 m, 1e-4 in each quaternion component): equal means
  the move was physics' own write and nothing happens; different means something else moved
  it (an agent, gameplay, a restore), so the body jumps there, its velocities are zeroed and
  it wakes.
- **Interpolation** (step 3): the renderer draws dynamic bodies and the player's camera at
  `alpha` (the `Timeline`'s leftover fraction) between the last two step poses, as a render-only
  matrix (never written to `Transform`, so the hash and the saved world stay step-exact). Until
  then a frame shows the last step's pose.

## Events

### What fires

| Kind | When | a, b |
|---|---|---|
| `contact_begin` | two solid bodies start touching (the first sub-shape contact of the pair) | a = the lower entity id, b = the other |
| `contact_end` | the pair's last sub-shape contact goes away, or either body is removed | same |
| `trigger_enter` | a body (or the player's inner body, step 2) starts overlapping a trigger | a = the trigger, b = the body |
| `trigger_exit` | it stops overlapping, or either is removed | same |

Each event carries `step` (the world frame it happened in), `point` and `normal` (world, from
the first contact of the begin manifold; an end repeats its begin's), and `speed` (the relative
normal speed at the begin, m/s; 0 for ends). Persisted contacts are not events: a pair is in
contact from its begin to its end, and `physics.contacts` (step 5) lists them.

Sleeping keeps contacts: a crate that settles on the floor and sleeps stays in contact with it
(no end); waking it does not begin it again.

### How they are produced

1. Jolt calls the backend's listener during `Update`, possibly from job threads; the listener
   only appends `{type, body1, body2, point, normal, relative velocity}` to the backend's
   mutex-guarded queue. Nothing in the world is touched from a Jolt callback.
2. After the step's substeps, on the simulating thread, the scene drains the queue
   (`getContactEvents`), drops persist events and events whose bodies are gone, maps body
   handles to entities, and keeps a per-pair count of sub-shape contacts: 0 → 1 is a begin,
   1 → 0 an end.
3. Removing a body (a destroyed entity, a rebuilt collider, a removed level) ends its pairs at
   once, in the step or sync that removed it; Jolt's later `OnContactRemoved` for it is ignored
   (its handle no longer maps).
4. The step's events are sorted by (kind order begin/enter before end/exit, a's id, b's id) and
   published.

### How they are read

- **ECS systems**: `ecs::physicsEvents(world)` is the span of the latest step's events. A
  simulate system added after `installPhysics` sees this step's; one added before sees the
  previous step's. Events hold entities and their ids (an entity may be gone by the time a
  system reads it).
- **Tests**: the same span after `world.simulate`, or the log below.
- **Agents**: the scene keeps a log of the last 1,024 events with a sequence number each.
  `physics.events {since?, max?}` returns those after `since` (the next cursor in `next`,
  `dropped` when older ones were overwritten). Every reply is small: at most 64 events, each
  under 200 bytes.
- **Scripts**: there is no scripting yet; when it comes it reads the ECS span.

### Triggers

`Collider.trigger` (bool, default false; saved) makes the collider a sensor: it detects
overlaps and blocks nothing. In a level, the hint `"trig": 1` on a node with `col` sets it.
A trigger is static unless the node or entity has a `RigidBody` (a kinematic trigger follows
its entity). Jolt sensors detect dynamic and kinematic bodies, and (step 2) the player through
its inner body; two static bodies never overlap-test.

## The player (step 2)

- **Component** `PlayerController` (saved): `radius` 0.3 m, `height` 1.8 m, `eyeHeight`
  1.62 m, `walkSpeed` 4.0 m/s, `sprintSpeed` 6.5 m/s, `acceleration` 40 m/s², `airControl` 0.3,
  `jumpSpeed` 4.9 m/s (a 1.22 m jump), `maxSlopeDeg` 45, `stepHeight` 0.35 m, `mass` 80 kg,
  `pushForce` 200 N; and the action/axis names (`move_forward`, `move_right`, `jump`,
  `sprint`, `look_x`, `look_y`).
- **Body**: a Jolt `CharacterVirtual` capsule (`physics::backend` character API) with an inner
  rigid body on the `CHARACTER` layer so triggers and rays see it. The `player` simulate
  system (after `physics`'s propagate and build, before its step: the character moves first,
  then bodies) reads the input, sets the desired velocity (ground: accelerate toward the input
  direction in the yaw frame; air: `airControl` of that), applies gravity, jumps when on ground,
  runs `ExtendedUpdate` (stick to floor, walk stairs up to `stepHeight`), pushes dynamic bodies
  it touches with `pushForce`, and writes the capsule's foot position to the entity's
  `Transform` (rotation: yaw about +Y only).
- **Spawn**: a world with a `PlayerController` entity places it at the first `SpawnPoint` with
  tag `player` (node order) when it is created or the level is (re)expanded, facing the spawn's
  yaw; without one, where it is, with `W_PLAYER_NO_SPAWN`. A scene for play holds the level and
  one player entity (`assets/scenes/blockout_play.scene.json`, step 3).
- **Look**: `look_x`/`look_y` turn yaw (the body) and pitch (the head child entity, clamped to
  ±89°), in degrees per mouse unit (`lookDegPerUnit`, 0.1).

## The camera (step 3)

- The player entity has a child `Head` at `eyeHeight` with the `Camera` (fov 75°, near
  0.05 m); first person is that camera being active. The fly camera stays for editing.
- Render interpolation (above) applies to the head, so the view is smooth at any frame rate.
- `ohao_game --scene assets/scenes/blockout_play.scene.json` plays it: mouse captured while
  the window has focus, Esc releases it.

## Footsteps (step 4)

- **Surface**: `Collider.surface` (a short tag, default `"default"`; saved) and the hint
  `"surf": "metal"` on level nodes. The player's ground body gives its surface.
- **Events**: the player emits `footstep {surface, point, speed}` every stride while on
  ground and moving (stride 0.75 m walking, 0.95 m sprinting), and `land {surface, speed}`
  when it touches ground after falling more than 0.3 m. These are player events, in the same
  log as physics events (`physics.events` returns both; kinds `footstep`, `land`).
- **Sound**: the window host's `footsteps` system plays `assets/audio/footsteps/<surface>_<n>.wav`
  (round robin over what exists, falling back to `default`) through `AudioSystem` at the foot,
  volume by speed. Headless hosts play nothing; the events are the test surface. The sounds
  are ours (generated or CC0), never Somnabulate's.

## The agent-facing surface

| Method | Step | Params | Result (each under 1 KB) |
|---|---|---|---|
| `world.step` | — | unchanged | physics steps inside it |
| `physics.events` | 1 | `since?` (seq), `max?` (1..64, default 32) | `{events: [{seq, step, kind, a, b, an, bn, point, normal, speed}], next, dropped?}` |
| `physics.info` | 1 | unchanged | adds `steps`, `awake`; `unstepped` is removed |
| `physics.contacts` | 5 | `entity?`, `max?` | the pairs in contact now |
| `player.info` | 5 | `entity?` | `{entity, pos, yawDeg, pitchDeg, velocity, ground, surface, steps}` |
| `player.teleport` | 5 | `pos`, `yawDeg?` | moves the player (an agent's spawn) |

Driving the player is `input.inject` (actions and axes) and `world.step`; looking is
`render.capture`. Errors reuse the catalogue: `E_STATE` without physics (or without a player),
`E_NOT_FOUND` for a named entity that is not one, `E_RANGE` for numbers out of their limits.
`tools/list` stays under 24 KB.

## Snapshots and replay (step 5)

Velocities and sleep state are physics state the saved components do not hold. Step 5 adds
`BodyMotion{linear, angular, asleep}` (runtime, snapshotted, not saved): written for awake
bodies after each step, restored into the bodies on restore. Until then a restore puts dynamic
bodies at their snapshot poses at rest, and a replay is exact only from `scene.load`.

## Performance budgets

Measured by `physics_bench` (new in step 1; `tests/bench/physics_budgets.json`), median of
200 steps after 60 warm-up steps, CPU:
- `blockout_step_ms` (the blockout, crates dropped, while they fall): ≤ 0.25 ms.
- `blockout_rest_step_ms` (the blockout settled, everything asleep): ≤ 0.02 ms, and zero
  `Transform` writes.
- `boxes1k_step_ms` (1,000 dynamic boxes falling into a pit, awake): ≤ 4 ms (step 5 gates it).
- `events_1k_ms` (1,000 begins in one step collapsed, sorted and logged): ≤ 0.5 ms.
- `player_step_ms` (the controller walking the blockout): ≤ 0.1 ms (step 2 gates it).

Each budget is tagged `gatedFrom` its step, as `render_bench` does.

## Determinism

The same scene, seed and inputs give the same poses, events and `world.hash` in lockstep,
in realtime (the same steps), in a window and headless, run to run. Tests compare two runs'
event lists and hashes byte for byte.

## Test plan

Step 1 (`level_physics_tests`, CPU, label `quick`; `api_tests`; serve goldens):
- `t10_crates_fall_and_rest`: the blockout's crates lifted by `setTransform` (CrateA 1 m,
  CrateB 2 m, so they start apart), 180 world steps (`simulate` + `sync`): both rest at their
  authored heights (2 cm), upright (1°), their `Transform`s say so, both asleep.
- `t10_contact_begin_events`: the same drop: `contact_begin` CrateA–Floor at step 26–29 with
  speed 4.0–4.9 m/s (a 1 m fall: 0.45 s, 4.43 m/s), then CrateA–CrateB at step 36–40 (2 m:
  0.64 s, 6.26 m/s); no `contact_end` while they rest.
- `t10_rest_writes_nothing`: after settling, 60 more steps patch no `Transform` and build no
  body.
- `t10_edit_teleports_dynamic`: `setTransform` on a resting crate moves its body there, at rest.
- `t10_two_steps_one_frame_motion`: two steps between two syncs: the crate's `prevWorld` is
  its pose before both.
- `t10_destroy_ends_contacts`: destroying a resting crate gives `contact_end` with the floor.
- `t10_trigger_enter_exit`: a box trigger and a falling sphere: `trigger_enter` (trigger,
  sphere) when it enters, `trigger_exit` after it is teleported out; the sphere passes through.
- `t10_substeps`: a step of 0.05 s runs 3 updates; the drop lands where three 1/60 steps land.
- `t10_runs_are_identical`: two worlds, the same drop: identical events and poses, bit for bit.
- `api_tests`: `sp6_t11_physics_events_after_drop`, `_events_cursor_and_dropped`,
  `_events_params_refused`, `_physics_info_counts_steps`, `E_STATE` without physics.
- serve: a `physics.jsonl` session (load the blockout, lift a crate, step, events, info);
  the CLI: `ohao run` of the blockout with a lift script and `--steps` (positions in the reply).

Steps 2–5 add (each in its step's plan task): walking tests (forward N steps moves N·dt·speed,
walls stop, the step and the ramp climb, crates are pushed, jump height 1.22 m ± 5 cm), camera
tests (pitch clamp, interpolation does not move the hash), footstep cadence and surfaces, the
`player.*` methods, a `check_live.py` leg walking the blockout in a hidden window, e2e_mcp.

## Done criteria

- You can walk the level: `ohao_game` on `blockout_play.scene.json` spawns at the player
  spawn; WASD walks, Shift sprints, Space jumps, the mouse looks; walls and crates block, the
  step and the ramp climb, crates can be pushed, footsteps sound.
- An agent can do the same over MCP and see it (`check_live.py` and `e2e_mcp.py` legs).
- Every gate green; physics budgets met (or noisy, pending a quiet rerun, per Frank's ruling
  on benches).

## Build order

1. **The step, the sync, events.** `physics` simulate system (propagate, build, substeps,
   write back, events); `propagateTransforms`; teleport detection; contact pair collapse;
   triggers (`Collider.trigger`, hint `trig`, sensor bodies); `physicsEvents(world)`, the log;
   `physics.events`, `physics.info` steps/awake; `physics_bench` (blockout and events budgets);
   tests above; look shots of crates falling in a real `ohao run`.
2. **The player.** `PlayerController`, the character, spawn, input, pushing, the inner body,
   kinematic bodies by `MoveKinematic`; walking tests; `player_step_ms`.
3. **The camera.** Head child and camera, look, render interpolation, `blockout_play.scene.json`,
   `ohao_game` play mode (mouse capture), look shots from the player's eyes.
4. **Footsteps.** `Collider.surface`, hint `surf`, footstep and land events, the audio
   system in the window host, sounds.
5. **Agent surface and replay.** `player.info`, `player.teleport`, `physics.contacts`,
   `BodyMotion` in snapshots, `check_live.py` and `e2e_mcp.py` walking legs, `boxes1k_step_ms`
   gated, docs, the done demo.

Each step ends with the engine running, every gate green, a change record
(`docs/changes/2026-10-26-player-physics-stepN.md`) and look shots in
`C:\Users\djmax\ohao_refs\look_shots\sp6_player\`.

## Risks

- **Jolt contact callbacks per sub-shape**: a compound body's begin/end counts must balance;
  a missed removal would leave a pair in contact forever. Removal on body destruction ends
  pairs explicitly; a test destroys a resting crate.
- **Teleport detection by comparison**: a gameplay write that happens to equal the last
  written pose is ignored (it is a no-op anyway).
- **Character and sensors**: `CharacterVirtual` is not a body; triggers see the player only
  through the inner body (step 2 tests it).
- **Write-back cost** on many awake bodies (1,000 boxes: 1,000 patches a step); budgeted.

## Review focus

- A dynamic body whose entity has a parent that moves: the body is the authority; the parent's
  move teleports it (compare by world pose), which is what a reasonable author expects of a
  crate on a moving level root.
- A level reloaded (`level.reload`) while crates fly: bodies rebuilt at the file's poses, their
  pairs ended (events), no stale handles.
- `world.step` with `dt` 0.1: substeps, not tunnelling.
- A trigger that is also a `RigidBody` dynamic: Jolt sensors may be dynamic; it falls and
  detects; tested in step 1 only as static.
- Events past the log's 1,024: `dropped` is right and the cursor never repeats an event.

## Decisions record

- Physics steps inside `World::simulate`, not in a host loop: one clock for every host,
  record/replay and lockstep inherited.
- No second accumulator; substeps only split a long `dt`.
- Events are per body pair (sub-shapes collapsed), sorted per step, logged with a sequence for
  agents; persist is not an event.
- The transform system is split, not called as is inside steps (motion vectors stay per
  frame).
- `physics.info.unstepped` is removed (forward only): nothing is unstepped any more.

## Frank actions

- Accept the spec (or rule on the open decisions).
- Footstep sounds: generated/CC0 ones are planned; say if Somnabulate's should be staged in
  `assets/local/` for look-and-listen sessions only.

## Open decisions

- Player tuning (walk 4.0 m/s, sprint 6.5, jump 1.22 m, eye 1.62 m, step 0.35 m): defaults
  chosen for a CoD-like feel; Frank may retune.
- Whether `physics.events` should also stream as MCP notifications during a long `world.step`
  (now: polled after).
