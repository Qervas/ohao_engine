---
module: physics
id: forces
title: Force generators
standard: v2
---

## A stage, not a solver

Everything under `ohao/physics/forces/` runs inside one narrow window of the fixed
step: after the engine gathers its body list, before Jolt integrates anything.
`PhysicsWorld::stepFixed` invokes the registry as step 2 of five.

{{cite ohao/physics/world/physics_world.cpp "// 2. Apply custom forces from force registry"}}

No generator ever touches Jolt — there is not a single Jolt symbol in the directory.
They write into a per-body accumulator on the engine-side `RigidBody` mirror, and
step 3 flushes it to the backend as one force at the centre of mass:

{{cite ohao/physics/world/physics_world.cpp "m_backend->applyForce(h, force, glm::vec3(0.0f));"}}

The zero offset does not discard the lever arm. No generator calls
`RigidBody::applyForceAtWorldPoint` directly; the one route out of this directory is
the `ForceUtils` wrapper, taken three times in `spring_force.cpp` and nowhere else:

{{cite ohao/physics/forces/force_generator.cpp "body->applyForceAtWorldPoint(force, worldPos);"}}

What it forwards to subtracts the body centre and files the remainder in a separate
torque accumulator, which leaves through its own backend call:

{{cite ohao/physics/dynamics/rigid_body.cpp "void RigidBody::applyForceAtWorldPoint(const glm::vec3& force, const glm::vec3& worldPoint) {"}}

Two consequences follow. Generators read body state pulled out of Jolt at the end of
the *previous* step, and their output enters Jolt as an opaque external force — the
solver never learns that a spring is a spring, so stiff springs fail exactly where
explicit integration says they should. And the flush has a deadband: the accumulated
force is dropped unless its squared magnitude exceeds 1e-4, and the torque channel
carries the identical test.

{{cite ohao/physics/world/physics_world.cpp "if (glm::length2(force) > 0.0001f) {"}}

{{cite ohao/physics/world/physics_world.cpp "if (glm::length2(torque) > 0.0001f) {"}}

That 0.01 N floor gates the per-body *sum*, not any one generator: `getAccumulatedForce`
is read once, after every registration has contributed, so a weak force disappears only
when everything the registry put on that body totals under the threshold. And it is a
low floor. `SurfaceTensionForce` defaults to a coefficient of 0.072 over a 1 m influence
radius, and even acting alone its linear taper $0.072\,(1 - d/R)$ stays above 0.01 out
to $d/R = 0.861$ — only the last 14% of its range, where the force has nearly vanished
anyway, lands in the deadband. That coefficient is annotated N/m in the header,
a tension per unit length, and then used as newtons: no contact length appears anywhere
in the expression. Accumulators are zeroed during read-back, which is what makes them
per-step rather than persistent.

{{cite ohao/physics/world/physics_world.cpp "// Clear local force accumulators"}}

:::why
The cheaper design lets each generator call `m_backend->applyForce` directly: one less
copy of the body list, one less step of staleness. OHAO accumulates on the mirror so
that force code compiles against `RigidBody` and nothing else, and so the
lever-arm-to-torque arithmetic is written once in engine code rather than re-derived
behind every implementation of `IPhysicsBackend`.
:::

## Priority is a sort with no keys

Each registration carries a generator, a name, and a target set. Application walks a
lazily-rebuilt vector sorted by descending generator priority.

{{cite ohao/physics/forces/force_registry.cpp "return a->generator->getPriority() > b->generator->getPriority();"}}

The key is constant. `m_priority` initialises to 0 on the base class and no engine code
ever calls `setPriority` — the sole call site in the repository is a unit test.

{{cite ohao/physics/forces/force_generator.hpp "int m_priority = 0; // Default priority"}}

With every key equal the comparator never returns true, and `std::sort` is not stable,
so the sequence it emits is whatever the `unordered_map` walk and the sort
implementation happen to produce. That costs less than it sounds: every generator adds
into the same accumulator and no generator reads it back, so ordering cannot change
*what* any one force computes. It can still move the low bits of the sum, because float
addition is not associative — and the registry gives you no lever to pin that order
down, which is the opposite of the scheduling control the name suggests.

Targeting has an inversion that catches people: an empty target set means "affect every
body", not "affect none".

{{cite ohao/physics/forces/force_registry.cpp "// If no specific targets, affect all bodies"}}

That is what makes `PhysicsWorld::setWind` global — it registers with no body list.

## The pair-force half-step

Dispatch is a `dynamic_cast` chain: global force, then single-body, then pair, then a
generic fallback. The pair branch is where the abstraction gives way.

{{cite ohao/physics/forces/force_registry.cpp "// For pair forces, apply to bodyA"}}

The comment assumes the generator applies both halves internally. `SpringForce` does
not — it branches on *which* body it was handed and pushes exactly one:

{{cite ohao/physics/forces/spring_force.cpp "ForceUtils::applyForceAtWorldPosition(m_bodyA, force, posA);"}}

Since the registry only ever hands it `bodyA`, the `bodyB` branch is unreachable
through the shipping path. Body B never receives the equal-and-opposite half, so a
registry-driven spring is a one-sided tether: A is drawn toward B, B feels nothing, and
linear momentum appears out of nothing every step. `BungeeSpringForce`,
`AngularSpringForce` and `MagneticForce` have the same shape. The unit tests miss it
because they call `applyForce` with each body by hand, reaching a `bodyB` branch the
registry never reaches.

:::key
The registry hands a generator exactly one body per call. A force that must move two
bodies has to push both inside that single call; branching on which body was passed is
only correct if the caller iterates both, and the pair branch does not.
:::

## Two damping signs, both correct

`SpringForce` and `AnchorSpringForce` build the same Hooke-plus-damper magnitude and
combine the terms with *opposite* signs. Both are right; the difference is a change of
reference, not a bug.

Write $\hat u$ for the unit vector from the body receiving the force toward the far end,
$L$ for the distance between attachment points, $L_0$ for the rest length
(`m_restLength`), $k$ for stiffness (`m_springConstant`), $c$ for damping (`m_damping`),
and $\mathbf v_{\text{far}}$, $\mathbf v_{\text{near}}$ for the world velocities of the
two attachment points, each already including its
$\boldsymbol\omega \times \mathbf r$ term:

$$f = k\,(L - L_0) \;+\; c\,\big[(\mathbf v_{\text{far}} - \mathbf v_{\text{near}})\cdot\hat u\big], \qquad \mathbf F = f\,\hat u$$

The bracket is $\dot L$, the rate at which the spring lengthens; adding $c\dot L$ to the
pull makes the force oppose stretching, which is what a damper does. `SpringForce` has
two moving ends, forms $\mathbf v_B - \mathbf v_A$ directly, and adds:

{{cite ohao/physics/forces/spring_force.cpp "float totalForceMagnitude = springForceMagnitude + dampingForceMagnitude;"}}

`AnchorSpringForce` has a fixed far end, so $\mathbf v_{\text{far}} = \mathbf 0$ and the
bracket collapses to $-\,\mathbf v_{\text{near}}\cdot\hat u$. The code forms the
positive dot product against the body's own velocity and therefore subtracts:

{{cite ohao/physics/forces/spring_force.cpp "float totalForceMagnitude = springForceMagnitude - dampingForceMagnitude;"}}

"Fixing" that minus to match its sibling converts the damper into an energy source.

## Buoyancy invents a volume, and it is the wrong one

`BuoyancyForce` needs a displaced volume. It holds the body, and
`RigidBody::getCollisionShape()` is public, but it never asks — the comment directly
above the line concedes that a real implementation would use the actual collision
shape. Instead it fabricates a sphere from mass alone, taking the radius as the cube
root of the mass.

{{cite ohao/physics/forces/environmental_force.cpp "float approximateRadius = std::pow(body->getMass(), 1.0f / 3.0f);"}}

That line pins the body's density, because volume and mass are now locked together.
With $m$ the mass in kg, $r = m^{1/3}$ the fabricated radius, $\rho_f$ the fluid density
and $g$ gravity, Archimedes' $F_b = \rho_f V g$ against the weight $W = mg$ gives

$$\rho_{\text{body}} = \frac{m}{\tfrac{4}{3}\pi m} = \frac{3}{4\pi} \approx 0.24\ \mathrm{kg\,m^{-3}}, \qquad \frac{F_b}{W} = \frac{4\pi}{3}\,\rho_f \approx 4.19\,\rho_f$$

Every body is lighter than air, whatever its real size. The parameter is usable only as
an arbitrary gain: neutral buoyancy sits at $\rho_f \approx 0.24$, not at the body's
actual density.

That ratio is the fully-displaced case, and the ramp reaches it slowly. The submerged
fraction is the *centre's* depth over one diameter, so a sphere floating exactly half
in the water displaces nothing, a sphere whose centre is one radius down counts as half
displaced, and the full $4.19\,\rho_f$ arrives only a whole diameter below the surface.

{{cite ohao/physics/forces/environmental_force.cpp "float submersionFraction = std::min(1.0f, submersionDepth / (2.0f * approximateRadius));"}}

At the default `m_fluidDensity` of 1000 — the value the header labels as water and
`PhysicsWorld::setWater` forwards verbatim — that is roughly 2100 times body weight for
a body sunk one radius and roughly 4200 for one sunk a full diameter.

## Noise whose clock runs on the population

`WindForce` and `TurbulenceForce` each advance an internal clock at the top of
`applyForce`.

{{cite ohao/physics/forces/environmental_force.cpp "m_time += deltaTime;"}}

`applyForce` runs once per affected body, so with $N$ eligible bodies the gust clock
advances $N\,\Delta t$ per step: spawn ten more crates and the wind gusts ten times
faster. `TurbulenceForce` carries the identical line.

{{cite ohao/physics/forces/field_force.cpp "m_time += deltaTime;"}}

The two generators do not share a noise function; each carries its own, and they differ.
`TurbulenceForce::noise3D` hashes integer lattice coordinates and folds the result into
$[-1, 1]$ — no gradient, no interpolation, whatever the "Perlin-like" label on the
function above it suggests. The comment inside the body, which calls it hash-based, is
the accurate one.

{{cite ohao/physics/forces/field_force.cpp "return (float)(hash % 2000) / 1000.0f - 1.0f;"}}

It takes the cell index by truncation rather than `floor`, so the cell straddling the
origin is two units wide while every other cell is one:

{{cite ohao/physics/forces/field_force.cpp "int xi = (int)x;"}}

Either way the field is piecewise constant: inside a cell the "turbulence" is exactly
constant, and crossing a boundary steps the force discontinuously. It is sampled at
world position offset by time, so those cells are one world unit across.

`WindForce::noise` is a near-copy — the same three multiply-and-xor constants, minus the
seed term — with two differences that change the field. It floors instead of truncating,
and the position it receives has already been scaled by 0.1, which makes its cells ten
world units wide: a crate must travel 10 m to meet a different gust. It also reads only
x and y, never z, so the turbulence vector is constant along the world Z axis.

{{cite ohao/physics/forces/environmental_force.cpp "float turbulenceX = noise(bodyPos.x * 0.1f, bodyPos.y * 0.1f, m_time * m_turbulenceFrequency);"}}

## The preset that doubles gravity

`ForcePresets::setupEarthEnvironment` registers a −9.81 m/s² `GravityForce` across the
whole body list, plus an air-drag generator.

{{cite ohao/physics/forces/force_presets.cpp "void ForcePresets::setupEarthEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies) {"}}

Jolt already has the same vector: `PhysicsWorldConfig` defaults gravity to (0, −9.81, 0)
and the backend installs it at initialisation.

{{cite ohao/physics/backend/jolt/jolt_backend.cpp "m_physicsSystem->SetGravity(toJolt(config.gravity));"}}

`PhysicsWorld::setupEarthEnvironment` clears the registry first but never zeroes the
backend's gravity, so every body it reaches falls at 2 g. Which bodies those are follows
from the targeting rule above: the preset registers against a snapshot of the body list
taken at the call, so anything created afterwards keeps 1 g — unless the world was empty
when it ran, in which case the empty target set makes the registration global and
present and future bodies alike get the double. Its only callers are in
`physics/examples/force_system_example.hpp`, which no translation unit includes, which
is why the doubling has gone unnoticed; the fix is `setGravity(vec3(0))` before
installing a registry gravity, not a weaker generator.

The drag half carries a smaller trap. `ForceFactory::createAirDrag` reads like it takes
a drag coefficient; the argument lands in `FluidDragForce`'s *third* slot, the
cross-section area, while the coefficient stays hard-wired at a sphere's 0.47.

{{cite ohao/physics/forces/forces.hpp "return std::make_unique<FluidDragForce>(1.2f, 0.47f, coefficient); // Air properties"}}

## What the shipping path actually uses

`PhysicsWorld` fronts a handful of these with named convenience calls: `setWind`
(`WindForce`), `setWater` (`BuoyancyForce`), `createSpring` / `createAnchorSpring`, and
`createForceVolumeBox` / `Sphere`. A force volume applies its vector without scaling by
mass, so unlike gravity it accelerates a heavy crate less than a light one — the point
of a jump pad, a surprise in a wind tunnel.

{{cite ohao/physics/forces/force_volume.cpp "body->applyForce(m_force);"}}

Those wrappers are a convenience, not the boundary. `registerForce` and
`getForceRegistry` are both public, so any generator here can be installed without one;
and the four `setup*` presets route into `force_presets.cpp`, a compiled translation
unit that makes eight `ForceFactory` calls of its own.

{{cite ohao/physics/forces/force_presets.cpp "auto waterDrag = ForceFactory::createWaterDrag(2.0f);"}}

What is genuinely unreached is narrower than "the rest". `AngularSpringForce` appears
nowhere outside its own two files. `ExplosionForce` is constructed only by
`ForceFactory::createExplosion`, whose sole caller is the example header that no
translation unit includes — and the same header is the only caller of the four `setup*`
entry points. `PhysicsWorld::applyRadialImpulse` is the nearest thing to a second
explosion path, with its own linear / quadratic / constant falloff and impulses pushed
straight at the backend, bypassing the registry; but its one caller in the tree is a
backend test, so by the standard applied above it is exposed rather than shipping.

{{cite ohao/physics/world/physics_world.cpp "float factor = (falloff == 1) ? t * t : (falloff == 2) ? 1.0f : t;"}}

## Contracts

- `applyForces` must run before the accumulator flush inside the same `stepFixed`; read-back zeroes the accumulators, so a generator invoked after the flush fails silently.
- An empty target list means *every* body, not none — a "targeted" registration whose body vector happens to be empty becomes global.
- Pair forces receive only `bodyA`. A two-body force must apply both halves inside that one call or it manufactures momentum.
- Registry gravity is additive on top of the backend's world gravity. Zero one of the two.
- The flush gates the per-body *sum*, not the individual generator: a body whose whole registry force totals 0.01 N or less (squared magnitude ≤ 1e-4) sends nothing to the backend that step, and the torque accumulator carries the same test.
- Priority sorts on a key nothing ever sets, and the sort is not stable. Do not rely on the order generators run in.
- `BuoyancyForce`'s fluid density is a gain, not a physical density; the mass-derived volume fixes body density at ≈ 0.24 kg/m³.
