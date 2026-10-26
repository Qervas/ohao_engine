# Player and Physics (Sub-project 6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Physics steps inside the world's fixed step in every host, dynamic bodies drive their entities, contacts and triggers are first-class events (ECS, tests, agents), and a first-person player spawned at the level's player spawn walks the blockout with a camera and footsteps, drivable by an agent over MCP.

**Architecture:**
- `installPhysics` adds a `physics` simulate system to the World: propagate dirty transforms (`propagateTransforms`, the transform system without the per-frame motion bookkeeping), bring bodies up to date (`PhysicsScene::sync`), step Jolt in substeps of at most 1/60 s, write awake dynamic bodies back to `Transform`, publish the step's events.
- Events are collapsed per body pair from the backend's queued Jolt callbacks on the simulating thread, sorted, exposed as `ecs::physicsEvents(world)` (the latest step) and a sequenced log of 1,024 for `physics.events`.
- Later steps add `PlayerController` on a Jolt `CharacterVirtual`, a head camera with render interpolation, footsteps through `AudioSystem`, and `player.*` methods.

**Tech Stack:** C++20, EnTT, Jolt (through `ohao/physics/backend`), miniaudio (`ohao/audio`), the reflection and API layers of sub-projects 3 and 4, Python 3 harnesses.

**Spec:** `docs/specs/2026-10-26-player-physics-design.md`

## Global Constraints

- Build: `C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6`, never more jobs and never two builds at once.
- One plain command per tool call; no `&&`, `;` or loops. Multi-line scripts go through the Write tool into `build/sp6/`.
- Never write under `C:\Users\djmax\Desktop\games` or any Unreal Somnabulate project. Somnabulate art stays in git-ignored `assets/local/`, never committed.
- SP5 step 6 (kit2k) is deferred; nothing here touches it.
- Engine code logs through `OHAO_LOG`; no prints under `ohao/`. ECS system sources include no `reflect/` or `api/` header.
- Budgets are targets and change only by Frank's ruling. Noisy benches are reported as noisy, pending a quiet rerun; no hour-long `wait_quiet`.
- Goldens are looked at before they are updated; every re-baseline is explained.
- Every task ends with `python tools/check.py --all` (functional suites green), a change record `docs/changes/2026-10-26-player-physics-stepN.md`, look shots in `C:\Users\djmax\ohao_refs\look_shots\sp6_player\`, and small local commits. Never push, merge, rebase, reset or checkout.

## Review Focus

1. Motion vectors: a frame of several steps must give each moved entity a `prevWorld` from the frame's start, not the last step's (Task 1, `t10_two_steps_one_frame_motion`).
2. A world at rest does no work: no `Transform` patch, no body build, no event (Task 1, `t10_rest_writes_nothing`).
3. Contact begin/end balance for compound bodies and destroyed bodies; no pair left in contact forever (Task 1, `t10_destroy_ends_contacts`).
4. An agent's edit of a dynamic body's `Transform` teleports it (and stops it); physics' own write-back does not (Task 1, `t10_edit_teleports_dynamic`).
5. Determinism: two runs of the same drop give byte-identical events and poses (Task 1, `t10_runs_are_identical`).

---

## File Structure

| Path | Responsibility |
|---|---|
| `ohao/ecs/transform_system.{hpp,cpp}` | `+ propagateTransforms(reg)`: dirty subtrees without the frame bookkeeping; `prevWorld` captured once per frame (Task 1) |
| `ohao/ecs/physics_scene.{hpp,cpp}` | `+ step(world, dt)`, write-back, teleport detection, event collapse/sort/log, `PhysicsEvent`, `physicsEvents(world)`, sensors (Task 1) |
| `ohao/ecs/components.hpp`, `scene_types.cpp`, `scene_writers.cpp` | `Collider.trigger` (Task 1); `PlayerController` (Task 2); `Collider.surface` (Task 4); `BodyMotion` (Task 5) |
| `ohao/physics/backend/*` | `BodyCreationInfo.isSensor`; later `moveKinematic`, the character's inner body (Tasks 1, 2) |
| `ohao/level/level_hints.cpp` | hint `trig` (Task 1), `surf` (Task 4) |
| `ohao/api/level.cpp`, `params.*`, `session.cpp` | `physics.events`, `physics.info` steps/awake (Task 1); `player.*`, `physics.contacts` (Task 5) |
| `ohao/ecs/player_system.{hpp,cpp}` | the `player` simulate system and spawn (Task 2) |
| `tools/bench/physics_bench.cpp`, `tests/bench/physics_budgets.json` | the physics budgets (Task 1, extended in 2 and 5) |
| `tests/level/level_physics_tests.cpp` | t10 tests (Task 1), walking tests (Task 2) |
| `tests/serve/sessions/physics.jsonl` | the physics session golden (Task 1) |

---

### Task 1: The step, the sync, events (build step 1)

**Files:** transform system, physics scene, components and registration (`Collider.trigger`), backend sensor flag, level hint `trig`, api `physics.events` and `physics.info`, `physics_bench`, tests, docs.

**Interfaces:**
- Produces: `void propagateTransforms(entt::registry&)`; `struct PhysicsEvent { enum class Kind : uint8_t { ContactBegin, ContactEnd, TriggerEnter, TriggerExit }; Kind kind; entt::entity a, b; uint64_t aId, bId; glm::vec3 point, normal; float speed; uint64_t step; uint64_t seq; }`; `std::span<const PhysicsEvent> physicsEvents(const World&)`; `PhysicsScene::step(World&, float dt)`, `stepEvents()`, `eventsSince(uint64_t seq, std::size_t max, uint64_t& dropped)`, `nextEventSeq()`, `steps()`, `awakeBodies()`.
- As built: `PhysicsScene::step(entt::registry&, float dt)`; awake bodies are `info().awake` (with `contacts`, `triggers`); `lastSubsteps()` and `lastStepTiming()` added. See the spec's "As built (step 1)".

- [x] **Step 1: Write failing tests.** `level_physics_tests`: `t10_crates_fall_and_rest`, `t10_contact_begin_events`, `t10_rest_writes_nothing`, `t10_edit_teleports_dynamic`, `t10_two_steps_one_frame_motion`, `t10_destroy_ends_contacts`, `t10_trigger_enter_exit`, `t10_substeps`, `t10_runs_are_identical`. `api_tests`: `sp6_t11_physics_events_after_drop`, `sp6_t11_events_cursor_and_dropped`, `sp6_t11_events_params_refused`, `sp6_t11_physics_info_counts_steps`. Build; run; expected: FAIL (no stepping).
- [x] **Step 2: Implement.** `propagateTransforms`; the `physics` simulate system (propagate, sync, substeps, write-back, events); teleport detection; pair collapse; sensors and `Collider.trigger` (+ hint `trig`); the event log; `physics.events`; `physics.info` (`steps`, `awake`; `unstepped` removed). Run `level_physics_tests`, `ecs_tests`, `api_tests`; expected: PASS.
- [x] **Step 3: Goldens and harnesses.** Schema and catalogue goldens (`Collider.trigger`, the hint), CLI and serve expectations (`physics.info` shape, the new tool), a `physics.jsonl` serve session; look at every diff before `--update`. `tools/list` ≤ 24 KB.
- [x] **Step 4: Bench.** `physics_bench --check tests/bench/physics_budgets.json`: `blockout_step_ms`, `blockout_rest_step_ms` (and zero writes), `events_1k_ms` gated from 1; `boxes1k_step_ms` (5), `player_step_ms` (2) pending.
- [x] **Step 5: Look shots.** Crates lifted and dropped in a real `ohao run` (a script lifts them, `--steps` 0/20/30/45/120, `--render`): `drop_00.png` … and a strip; the blockout from the player spawn.
- [x] **Step 6: Close.** Docs (AGENTS.md physics notes, ARCHITECTURE data flow, the CLI and serve guides' physics methods), `check_docs`, `check.py --all`, change record `2026-10-26-player-physics-step1.md`, commits.

### Task 2: The player (build step 2)

- [ ] **Step 1: Write failing tests.** Walking the blockout from the player spawn with injected input: forward 60 steps moves 4.0 m ± 5 cm; the north wall stops it; the step (0.3 m) and the ramp climb; a crate is pushed; jump apex 1.22 m ± 5 cm; a trigger sees the player; spawn facing the spawn's yaw.
- [ ] **Step 2: Implement.** `PlayerController` (registered with docs/units/limits), the backend character with an inner body, `moveKinematic`, the `player` simulate system, spawn at `SpawnPoint` `player`. `player_step_ms` gated.
- [ ] **Step 3: Close.** Goldens, docs, `check.py --all`, change record, commits.

### Task 3: The camera (build step 3)

- [ ] **Step 1: Write failing tests.** Pitch clamp ±89°, yaw turns the body, the head at `eyeHeight`, render interpolation leaves `world.hash` and saved files unchanged, `blockout_play.scene.json` loads with an active head camera.
- [ ] **Step 2: Implement.** Head child and camera, look, interpolation (render-only matrices at `alpha`), the play scene, `ohao_game` play mode (mouse capture, Esc).
- [ ] **Step 3: Close.** A golden of the play scene from the spawn (looked at), look shots from the player's eyes, docs, gate, change record, commits.

### Task 4: Footsteps (build step 4)

- [ ] **Step 1: Write failing tests.** Footstep cadence (walk 0.75 m, sprint 0.95 m strides), surface from the ground body, `land` after a 0.3 m drop, none while still or airborne.
- [ ] **Step 2: Implement.** `Collider.surface` and hint `surf`, player events in the log, the window host's footsteps system and sounds (generated/CC0, `assets/audio/footsteps/`).
- [ ] **Step 3: Close.** Docs, gate, change record, commits.

### Task 5: Agent surface and replay (build step 5)

- [ ] **Step 1: Write failing tests.** `player.info`, `player.teleport`, `physics.contacts` (api_tests, serve goldens), `BodyMotion` restore exactness (restore mid-fall, replay, same hash), a `check_live.py` leg walking the blockout in a hidden window, an `e2e_mcp.py` walking leg.
- [ ] **Step 2: Implement.** The methods, `BodyMotion`, `boxes1k_step_ms` gated.
- [ ] **Step 3: Close.** The done demo (walk the level in `ohao_game` and over MCP), roadmap row, monograph unit, docs, gate, final change record, commits.
