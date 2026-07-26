---
module: physics
id: debug
title: Force debugger
standard: v2
---

## An instrument wired to nothing

`ForceDebugger` is a per-frame recorder for physics forces: it collects labelled
force and torque vectors, aggregates them per rigid body, and logs or writes a
text report with net force, net torque and application counts. What is missing
is a caller. `PhysicsWorld` owns one, built at construction and rebuilt lazily if
`enableForceDebugging(true)` finds none:

{{cite ohao/physics/world/physics_world.cpp "if (enable && !m_forceDebugger) {"}}

but the fixed-timestep tick never touches it. `stepFixed` applies the force
registry, pushes each body's accumulated force and torque into whichever
`IPhysicsBackend` is live — Jolt when it is compiled in and initialises, the null
backend otherwise — steps, and syncs results back:

{{cite ohao/physics/world/physics_world.cpp "m_forceRegistry.applyForces(bodyPtrs, fixedDt);"}}

No `startFrame`, no `recordForceApplication`, no `analyzeForceRegistry` appears in
that path, or anywhere else in the tree: those three names occur only in
`force_debugger.hpp` and `force_debugger.cpp`, where they are declared, defined,
and called by each other. Outside `PhysicsWorld`, which only owns the object, the
sole file naming the debugger is `physics/examples/force_system_example.hpp`, and
it uses only the configuration and query surface — the visualization setters,
`getForceVectors`, `getFrameStats`, `generateForceReport` — while asserting in a
comment that collection takes care of itself:

{{cite ohao/physics/examples/force_system_example.hpp "The debugger will automatically collect data during simulation steps"}}

No translation unit includes that header, and as written it would not compile.
The hook whose name promises to drive this has an empty body:

{{cite ohao/physics/world/physics_world.cpp "// Update debug visualization data"}}

The code still ships: the physics CMake target globs its own directory tree, so
being unreferenced does not keep it out of the static library.

## The automatic feed reads a field nobody writes

There are two intended ways in. The manual one, `recordForceApplication`, wants
the caller to name each force as it is applied. The automatic one,
`analyzeForceRegistry`, walks the body list and synthesises one `net_force`
record per body out of `RigidBody::ForceStats`:

{{cite ohao/physics/debug/force_debugger.cpp "recordForceApplication(body, forceStats.totalForceApplied,"}}

That field is incremented in exactly one place in the engine, inside
`RigidBody::applyForceTracked`:

{{cite ohao/physics/dynamics/rigid_body.cpp "m_forceStats.totalForceApplied += force;"}}

and `applyForceTracked` / `applyTorqueTracked` have zero callers. Every shipping
generator reaches for the untracked pair instead. Gravity, drag, buoyancy, wind,
magnetic, spring, explosion, vortex and turbulence reach the plain `applyForce` —
directly, or through `applyForceAtWorldPoint` — which accumulates into
`m_accumulatedForce` and records no statistics:

{{cite ohao/physics/forces/environmental_force.cpp "body->applyForce(buoyantForce);"}}

and the two purely rotational members of that family, `AngularDragForce` and
`AngularSpringForce`, call `applyTorque`, which does the same to
`m_accumulatedTorque`:

{{cite ohao/physics/forces/drag_force.cpp "body->applyTorque(angularDragTorque);"}}

So `getForceStats()` returns an all-zero struct, every magnitude falls under the
0.1 default threshold, and the automatic feed records nothing even when called. A
second break hides behind the first: `resetForceStats()` has no callers either.

{{cite ohao/physics/dynamics/rigid_body.hpp "void resetForceStats() { m_forceStats = ForceStats{}; }"}}

The moment someone flips the generators to the tracked variants,
`totalForceApplied` becomes a lifetime running total rather than a per-frame net
force. Nothing zeroes it: `clearForces()` resets only the accumulators the solver
consumes.

{{cite ohao/physics/dynamics/rigid_body.cpp "m_accumulatedForce = glm::vec3(0.0f);"}}

It is a `glm::vec3` sum, not a sum of magnitudes, so it does not grow
monotonically — opposed forces cancel inside it and it random-walks — but it is
stale by construction, and the debugger will label whatever it holds `net_force`.
Wiring the tracked call without a per-step reset trades no data for wrong data.

## Net force and total force are different numbers

Per-body aggregation keeps two quantities whose names suggest they are the same:

$$\mathrm{netForce} = \sum_i \mathbf{F}_i, \qquad \mathrm{totalForceApplied} = \sum_i \lVert \mathbf{F}_i \rVert$$

where $\mathbf{F}_i$ is the *i*-th force recorded on that body this frame. The
vector sum is what accelerates the body. The scalar sum of magnitudes is what
tells you whether a body sitting still is genuinely unloaded or is being pulled
apart by two large opposed forces that cancel — the distinction that makes a
force debugger worth having.

{{cite ohao/physics/debug/force_debugger.cpp "bodyIt->totalForceApplied += magnitude;"}}

The name is reused across two structs with different types and opposite meanings:
`ForceDebugger::BodyForceStats::totalForceApplied` is the scalar
$\sum\lVert\mathbf{F}\rVert$ above, while `RigidBody::ForceStats::totalForceApplied`
is a `glm::vec3` holding $\sum\mathbf{F}$. The automatic feed passes the latter
into the slot the report prints as net force, which is correct — but only because
the two identically-named fields do not mean the same thing.

## Frame discipline, and where it is asymmetric

`startFrame` clears the vectors and the stats block; `endFrame` computes
statistics, then applies mode filtering. It looks like it also stamps a
collection time, but that branch sits behind `m_profilingEnabled`, which defaults
to `false` — the only `setProfilingEnabled(true)` in the tree is in the example
header that nothing compiles — so by default the timing fields stay zero.
`startFrame` is idempotent by early return, so a nested `DEBUG_FORCE_FRAME` will
not wipe the outer frame's data on entry:

{{cite ohao/physics/debug/force_debugger.cpp "if (m_frameActive) return;"}}

The matching destructor is a different story. `endFrame` does not count nesting,
so an inner scope closing finalises the outer frame, and every record made
afterwards is silently swallowed by the `!m_frameActive` guard. The RAII helper
is single-scope-only.

Ordering inside `endFrame` matters too — statistics run before filtering:

{{cite ohao/physics/debug/force_debugger.cpp "updateFrameStatistics();"}}

so in `NET_FORCES_ONLY` the reported `totalForcesApplied` counts vectors that are
erased a line later. Statistics describe what was collected; `getForceVectors()`
describes what survived. `generateForceReport` prints both halves of that split
in one document: its summary block and per-body breakdown read the pre-filter
counts, while its FORCE VECTORS listing walks the post-filter vector, so
`NET_FORCES_ONLY` deletes rows from the report and `BY_TYPE` reorders them.

{{cite ohao/physics/debug/force_debugger.cpp "report << force.sourceId"}}

{{cite ohao/physics/debug/force_debugger.cpp "case VisualizationMode::ABOVE_THRESHOLD:"}}

:::why
Two of the settings that read as view controls are enforced at record time, not
at draw time, and that is the asymmetry to hold on to. The magnitude threshold
discards a force before anything counts it — which is why the `ABOVE_THRESHOLD`
filter case is empty, the work is already done — and `setShowTorques(false)`
discards torques the same way. Neither is reversible after the fact: lowering the
threshold reveals nothing already dropped, and raising it removes forces from the
statistics, not just from the display. Only the visualization mode is applied
post-hoc, and even it is not inert.
:::

## Sharp edges before you wire it up

- The threshold doubles as the divide-by-zero guard: direction is stored
  normalized, and only the `magnitude < m_minMagnitude` early-out keeps a
  zero-length force away from `glm::normalize`. Setting the threshold to `0` —
  the natural "show me everything" move — lets a zero force through and yields a
  NaN direction. {{cite ohao/physics/debug/force_debugger.cpp "forceVec.direction = glm::normalize(force);"}}
- `setShowTorques(false)` gates *collection*, not drawing, so turning off torque
  rendering also zeroes `maxTorqueMagnitude` and the report's net-torque column.
  {{cite ohao/physics/debug/force_debugger.cpp "if (!m_frameActive || !body || !m_showTorques) return;"}}
- A torque merges into a per-body row only if a force already created that row;
  it never creates one. A body with large torque and no recorded force gets a
  torque vector in the visualization list and no line in the body breakdown.
  {{cite ohao/physics/debug/force_debugger.cpp "if (bodyIt != m_bodyStats.end()) {"}}
- The per-body row is keyed on the `RigidBody*` address, but the label printed
  beside it is a *different* pointer: `getBodyName` returns the
  `PhysicsComponent*` in decimal whenever the body has one, and every body the
  world owns does — `createRigidBody` returns `nullptr` for a null component, so
  the raw-body fallback below is unreachable for any body in the world. The code's
  own comment concedes the name should be the entity's.
  {{cite ohao/physics/debug/force_debugger.cpp "std::to_string(reinterpret_cast<size_t>(component));"}}
  So one body is identified by two unrelated addresses, neither stable across
  runs, while `RigidBody` already carries a monotonic `m_uniqueId` — which
  `SimulationProfile` uses for exactly this purpose and the debugger never asks
  for. {{cite ohao/physics/dynamics/rigid_body.hpp "uint32_t getUniqueId() const"}}
  The lookup is also a linear `std::find_if` over the per-body vector, so
  recording costs O(forces x bodies).

:::key
This is an unwired instrument that is also not yet correct. Wiring it takes four
edits, and the order matters: move the force generators to `applyForceTracked`
*and* the two rotational ones to `applyTorqueTracked` — skip the second and the
torque those two apply never reaches `analyzeForceRegistry`'s net-torque branch,
which reads only `totalTorqueApplied` — call `resetForceStats()` on every body
once per fixed step, and bracket `stepFixed`
with `startFrame` / `analyzeForceRegistry` / `endFrame`. Doing only the first
yields plausible, never-reset, wrong numbers. Even fully wired, the defects above
remain: the zero-force `normalize`, the nesting-blind destructor, and the torque
rows that vanish when no force created them.
:::
