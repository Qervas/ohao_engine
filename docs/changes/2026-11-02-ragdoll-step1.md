# Sub-project 13, step 1: joints as scene data

**Spec:** `docs/specs/2026-11-02-ragdoll-design.md` ("The data model", "Test plan": test 36; "Build order" 1); plan
`docs/plans/2026-11-02-ragdoll.md`, task 1.
**Status:** built. A `Joint` component makes one constraint between two bodies (or a body and the world); a break
disables it; snapshots replay a swinging chain and a break bit for bit. Ragdolls (step 2) are next.

## 1. Summary

The physics backend had seven constraint types and nothing used them. Step 1 makes them scene data: `Joint` names the
other entity, an anchor and an axis in the entity's own frame, limits, a spring and breaking thresholds; the `physics`
sync builds its constraint after the bodies, in creation order, and rebuilds or removes it with them. A break disables
the constraint instead of removing it, so Jolt's constraint list, which its solver and its saved state are ordered by,
never changes mid-round. `assets/scenes/joints.scene.json` holds a hinged door, a 10-link chain, a lamp on a rope and a
plank that a falling crate breaks.

## 2. What was built

- **Backend** (`physics/backend`):
  - `ConstraintSettings.space`: `Local` (the default) or `World`. Local anchors and axes are placed by the body's
    origin and rotation and handed to Jolt in world space; Jolt's own local space is relative to the centre of mass,
    which a compound body moves.
  - `SWING_TWIST` (Jolt's `SwingTwistConstraint`, the ragdoll joint) replaces `CONE_TWIST`, which had no twist limit.
  - `setConstraintLimits` covers swing-twist's twist; other types without limits log once.
  - Breaking disables the constraint and reports it until `getAndClearBrokenConstraints` (in handle order, across
    substeps); `isConstraintEnabled`.
  - `collideConnected` (default false): two bodies joined by a constraint do not collide, as in PhysX and Unity
    (`OnContactValidate` rejects the pair; a count per pair for two joints on one pair).
  - `velocitySteps`/`positionSteps`: Jolt's per-constraint solver iteration overrides (unused by joints; section 5).
  - `saveState` writes the constraint list in Jolt's order (our handles); `restoreState` refuses a different list.
- **ECS:** `Joint` and `JointState` (saved; `JointType` Fixed, Point, Hinge, Slider, SwingTwist, Distance). A joint's
  body is its entity's own, a `ColliderPart`'s target, else its nearest ancestor's, so several joints on one body are
  child entities. The other side is the same world point and axis at build, except a Distance joint's, which is the
  other entity's origin (a rope from a hook). Limits are this entity's turn or travel relative to the other
  (right-handed about the axis; Jolt measures the other against this one, so they go in negated). Warnings
  `W_JOINT_NO_BODY`, `W_JOINT_SELF`, `W_JOINT_STATIC`, `W_JOINT_FAILED`.
- **The physics scene:** joints built after the bodies (`syncJoints`, creation order); a body destroyed or rebuilt
  destroys its joints first and rebuilds them after; a `JointState` written by anything but the step (a restore, an
  edit) only enables or disables the constraint, never rebuilds it; `joint_break` events (`PhysicsEvent::Kind::
  JointBreak`, after footsteps in a step's order); `SavedSimulation` carries the joints' identity (handle, entity id).
- **Agents:** `physics.info` `joints`, `brokenJoints`; `physics.events` `joint_break`.
- **Reflection:** units `N s` and `N m s`.
- **Bench:** `physics_bench` `joints_step_ms` (the joints scene's whole physics step over 180 steps; the chain must
  swing and both plank joints break), budget 0.10 ms pending until `ragdollStep` 2.

## 3. Bugs found and fixed

- **Constraint anchors were world space** although `ConstraintSettings` said local: no setting passed `mSpace`.
- **A broken constraint was removed** from Jolt and the backend's map. Jolt fills a removed constraint's slot with the
  last one, and the solver and `SaveState` order by that slot, so a break changed the order every later constraint
  solves in, and a snapshot from before it could not be restored.
- **Broken constraints from all but the last substep were lost**: `step()` cleared the list each call, and the scene
  steps the backend up to 8 times a step.

## 4. Spec corrections (to be folded into the spec's "As built")

- `JointState` is `{broken}` alone. The step it broke in would be the physics scene's step counter, which snapshots do
  not restore (combat's and animation's counters are not restored either; their logs are runtime): a saved step
  would differ after a replay. The step is the `joint_break` event's.
- `Joint` has no `otherAnchor`/`otherAxis`: the other side is the built pose's (or the other entity's origin for a
  Distance joint).
- The joints scene is its own open-air scene (a floor, a gantry), not in the blockout room.
- The chain bench is `joints_step_ms`, the joints scene's whole step (10 links, a rope, a door, a breaking plank),
  not a 20-box chain.

## 5. Results

- `physics_backend_tests` 65/65 (new: swing-twist cone, local anchors on a turned door, a break disabling, state
  restored exactly across a disable and refused across a removal, a 20-link chain on the bare backend).
- `level_physics_tests` 71/71, test 36 (11 cases):

| Case | Measured |
|---|---|
| a door turned 90 degrees, hinged at its local edge, swung 30 steps | the edge moved at most 0.03 mm |
| hinge limits 0 to 110 degrees, spun both ways | [0.00, 110.37] and [-0.00, 0.00] |
| a 1 m pendulum at 5 degrees | period 2.0092 s against 2.0061 (0.15%) |
| swing-twist, 20 degree cone, 10 degree twist, kicked | worst swing 21.02, twist 10.92 |
| 20-link chain, settled 4 s | gaps sum 5.17 cm (2.6 mm a joint) |
| the same, swinging from a 1 m/s kick | worst single joint 4.65 cm |
| a 200 kg box on a 10 N s joint | broke in step 1; the 100 N s one held |
| restore mid-swing, restore before a break | bit for bit (poses and `world.hash`) |

- **The chain's stretch is Jolt's solver at its defaults** (10 velocity, 2 position iterations): the bare backend
  measures the same 5.17 cm, so the ECS builds what it should. More velocity iterations through the per-constraint
  override steadied the hanging chain (20: 3.9 cm, 40: 1.7, 100: 0.5) but made the swinging one worse (15: 39 cm, 20:
  36, 40: 30); position iterations changed little. Not understood yet; joints keep Jolt's defaults, and the ragdoll's
  limits test (step 2) measures it again on short chains. Neighbouring links never collided (the pair filter changed
  no number).
- A swing-twist joint overshoots its cone with the kick: 0.9 degrees at 2 m/s, 1.1 at 3, 1.9 at 4 (backend test, 1 m
  arm). The tests' 2 degree tolerance is the spec's.
- `physics_bench` `joints_step_ms`: median 0.023 ms, p95 0.031 (noisy machine; pending).
- Gate and goldens: section 7.

## 6. Known gaps

- More velocity iterations make a swinging chain stretch more (section 5).
- A joint built again (its body rebuilt, the `Joint` edited) re-anchors at the bodies' current poses.
- The door in the joints scene stays shut: nothing in the scene pushes it (no player).

## 7. Gates

- **First `tools/check.py`: 50 of 58.** The new saved types had no scene-file writers (`scene_tests` t21) or member
  checks (t01), and moved the schema hash: the reflection catalogue goldens (`scene_tests`, `reflect_tests`), every
  CLI and serve expectation that records a hash, and three examples in `docs/agents/ohao-serve.md`. Each was regenerated
  after reading its diff: only the hash, the two new units, and the `Joint`, `JointState` and `JointType` blocks
  changed. `check_live` (a headless capture after the fight wrote no image) and `deferred_determinism_gpu_test`
  (`cornell_rt_offline` across processes, past the BVH floor) failed once and passed alone (76/76, 84/84).
- **Second run: 57 of 58**; `check_docs` failed only on the three `ohao-serve.md` hashes, which match a fresh run of
  each command after the update.
- Goldens not run (no renderer change).

## 8. How to run

```bash
build/Release/level_physics_tests.exe
build/Release/physics_backend_tests.exe
build/Release/ohao.exe run assets/scenes/joints.scene.json --steps 120 --save build/joints.scene.json
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
```
