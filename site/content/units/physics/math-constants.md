---
module: physics
id: math-constants
title: Physics math and constants
standard: v2
---

## Two headers that both define `constants`

`physics_constants.hpp` and `physics_math.hpp` each open a namespace named
`constants`, and each declares the same twelve scalars — PI, TWO_PI, HALF_PI,
EPSILON, LARGE_NUMBER, the three planetary gravities, the velocity caps and the
mass bounds — with identical values. They are not two views of one file. They are
two independent declarations sitting at different namespace depths,
`ohao::physics::constants` and `ohao::physics::math::constants`:

{{cite ohao/physics/common/physics_constants.hpp "constexpr float EPSILON = 1e-6f;"}}
{{cite ohao/physics/utils/physics_math.hpp "constexpr float EPSILON = 1e-6f;"}}

Which copy a call site gets is decided silently by unqualified lookup. Code
inside `namespace dynamics` writes `constants::MAX_LINEAR_VELOCITY`, finds
nothing in `dynamics`, and lands on the outer copy:

{{cite ohao/physics/dynamics/rigid_body.cpp "m_linearVelocity = math::clampLength(velocity, constants::MAX_LINEAR_VELOCITY);"}}

The inline helpers in `physics_math.hpp` that read a constant at all — both
`safeNormalize` overloads, both `isNearZero` overloads, the quaternion
integrator — write the same token from inside `namespace math` and land on the
inner one, as do the collision-shape headers, which qualify it as
`math::constants::` explicitly. The values agree today, so nothing misbehaves.
The hazard is edit locality. `EPSILON` guards eight sites in the shape headers —
the plane's on-plane test, its ray-parallel denominator and its equation
normaliser, the capsule's collapsed-segment case, the cylinder's on-axis case,
and three in the triangle-mesh Möller–Trumbore routine — plus every
`safeNormalize` those headers call; retuning it in `physics_math.hpp` moves all
of them at once. Retuning it in `physics_constants.hpp` moves nothing but the
diagonal test and reciprocal guard inside `physics_constants.cpp`'s
`calculateInverse`. Neither edit touches the other set, and nothing in the build
reports the asymmetry.

## The inertia tensors are compiled twice

Both `.cpp` files define `ohao::physics::inertia::calculateBoxTensor`,
`calculateSphereTensor`, `calculateCylinderTensor`, `calculateCapsuleTensor`,
`transformToWorldSpace` and `calculateInverse` — six external-linkage functions,
same namespace, two bodies each. The module globs every `.cpp` beneath
`ohao/physics/` into one static library, so both translation units land in the
same archive:

{{cite ohao/physics/CMakeLists.txt "file(GLOB_RECURSE PHYSICS_SOURCES"}}

This is a one-definition-rule violation the toolchain is not obliged to
diagnose. An archive member is extracted only if, at the moment the scan reaches
it, one of its symbols is *already* undefined; a member that would merely
duplicate a definition is passed over, so no duplicate-symbol error is ever
raised. What picks the winner is therefore where the *reference* sits, not where
the definitions do. `ar t build/libohao_physics.a` lists
`physics_constants.cpp.o` third, `rigid_body.cpp.o` — the only file in the tree
that calls `inertia::*` — sixth, and `physics_math.cpp.o` seventeenth. Member 3
is examined while those six symbols are still unreferenced, so it is skipped;
member 6 makes them undefined; member 17 is the next definition the scan meets.
No later pass rescues member 3 — by then the symbols are defined and it has
nothing left to contribute.

The binaries agree. `inertia::combine`, defined only in `physics_constants.cpp`
and called from nowhere in the repo, is absent from the linked `renderer_test`,
`cornell_box` and `interactive`, so its parallel-axis-theorem code has never
shipped — and the `calculateCapsuleTensor` that did link sums a cylinder tensor
and a sphere tensor, which is `physics_math.cpp`'s body; the
`physics_constants.cpp` one calls the cylinder alone with `height + 2 * radius`.

{{cite ohao/physics/common/physics_constants.cpp "glm::mat3 combine(const glm::mat3& tensorA"}}

## The two copies disagree about the cylinder's axis

For a solid cylinder of mass $m$, radius $r$ and length $h$, the principal
moments are

$$I_{\parallel} = \tfrac{1}{2} m r^2, \qquad I_{\perp} = \tfrac{1}{12} m\,(3r^2 + h^2)$$

where $I_{\parallel}$ is about the symmetry axis and $I_{\perp}$ about either
transverse axis. Both files implement exactly these two expressions, and put
$I_{\parallel}$ on different diagonal slots. `physics_math.cpp` puts it in the
middle — a Y-aligned cylinder:

{{cite ohao/physics/utils/physics_math.cpp "float iyy = mass * r2 / 2.0f;"}}

`physics_constants.cpp` puts it last — a Z-aligned cylinder:

{{cite ohao/physics/common/physics_constants.cpp "float izz = 0.5f * mass * r2;"}}

The shapes settle which is right. `CylinderShape` and `CapsuleShape` both take
their axis to be local +Y, in their bounds, endpoint and containment code:

{{cite ohao/physics/collision/shapes/cylinder_shape.hpp "In local space, cylinder is aligned with Y-axis"}}

So the copy that currently wins the link is the one whose convention matches the
shapes. The loser would swap $I_{\parallel}$ and $I_{\perp}$ for every cylinder
and capsule — for a long thin capsule that is roughly the difference between
spinning about its length and tipping end over end. Which one survives is decided
by where `rigid_body.cpp.o` happens to sit between the two definitions in the
archive, not by anything a reader of the source can see.

:::why
None of this reaches the simulation. `PhysicsWorld` prefers the Jolt backend and
drops to `NullPhysicsBackend` if Jolt fails to initialise; on the Jolt path a
*dynamic* body is created with OHAO's mass but Jolt's own rotational inertia,
derived from the Jolt shape, so the tensor that is integrated never comes from
`inertia::*`:

{{cite ohao/physics/backend/jolt/jolt_backend.cpp "EOverrideMassProperties::CalculateInertia"}}

Static and kinematic bodies set no override at all and take Jolt's defaults. The
alternative was Jolt's `MassAndInertiaProvided` override — push OHAO's tensor
into the body creation settings and keep one source of truth. It was not taken.
The price is visible above: the engine carries two disagreeing inertia
implementations, and nothing the simulation does can tell them apart.

`inertia::*` is not idle, though. Every `PhysicsComponent::setMass` — scene
import and `ComponentFactory` both call it on load — reaches
`RigidBody::calculateInertiaFromShape`, which builds a shape tensor and inverts
it through `inertia::calculateInverse` for the mirror `RigidBody`. Nothing then
reads the result. `getAngularMomentum` has no callers; `getKineticEnergy`'s only
caller is `RigidBody::updateSleepState`, which has none of its own; and
`getWorldInverseInertiaTensor` is reached only from `RigidBody::integrate`, which
has no callers, and `RigidBody::applyImpulse`, whose only route in is
`PhysicsComponent::applyImpulse` — an unused public entry point. So the tensor is
computed for every body on every load and consumed by nobody.
:::

## The helper that lost to its own inlined copy

`physics_math.hpp` ships `integrateAngularVelocity`, which has no callers — but
the same construction (axis from $\hat\omega$, angle $|\omega|\Delta t$,
`angleAxis`, compose, renormalise) is inlined in `RigidBody::integrate`. It is
not a copy. The header skips the update when $|\omega|^2 <$ `EPSILON`$^2$, i.e.
below $10^{-6}$ rad/s, and renormalises through `math::safeNormalize`; the
integrator skips when $|\omega| \le 0.001$ and renormalises with
`glm::normalize` — a dead band a thousand times wider, unremarked in either
file. The composition order differs too. The header composes on the right:

{{cite ohao/physics/utils/physics_math.hpp "return safeNormalize(rotation * deltaRotation);"}}

The rigid body composes on the left:

{{cite ohao/physics/dynamics/rigid_body.cpp "m_rotation = glm::normalize(deltaRotation * m_rotation);"}}

That is not a style difference. For orientation $q$ and incremental rotation
$\Delta q$ built from angular velocity $\omega$,

$$q_{t+\Delta t} = \Delta q\, q_t \;\;(\omega \text{ in world frame}), \qquad q_{t+\Delta t} = q_t\, \Delta q \;\;(\omega \text{ in body frame})$$

`RigidBody::integrate` obtains $\omega$ by applying the *world*-space inverse
inertia tensor to accumulated torque, so its left-multiply is the consistent one;
the header helper is written for a body-frame $\omega$ that no call site in the
tree produces. Deleting the helper as unused is safe. Substituting it into the
integrator to "remove duplication" would silently flip the frame — in code that
is itself dormant, since `PhysicsWorld::stepFixed` steps Jolt and reads the
transforms back rather than calling `RigidBody::integrate`.

## Constants describing a solver that does not exist

`CONTACT_PENETRATION_SLOP`, `CONTACT_BAUMGARTE_FACTOR`, `RESTITUTION_THRESHOLD`,
`MIN_TIMESTEP`, `MAX_TIMESTEP`, `TWO_PI`, `HALF_PI`, `LARGE_NUMBER` and
`GRAVITY_MARS` each appear exactly at their own definitions and nowhere else.
They describe a hand-written contact solver — Baumgarte position stabilisation,
penetration slop, a restitution cutoff — that OHAO does not have, because Jolt
owns all three internally. They are a design sketch left in a header, not tuning
knobs.

`PhysicsConfig`, the struct that consumes `GRAVITY_EARTH`, is likewise referenced
outside its own header only by `tests/engine/engine_tests.cpp`. The world runs on
a different struct, `PhysicsWorldConfig`, which re-hardcodes the same number:

{{cite ohao/physics/common/physics_constants.hpp "glm::vec3 gravity{0.0f, -constants::GRAVITY_EARTH, 0.0f};"}}
{{cite ohao/physics/world/physics_world.hpp "glm::vec3 gravity{0.0f, -9.81f, 0.0f};"}}

One constant is written into a comparison that has a units error.
`SLEEP_LINEAR_THRESHOLD` is declared under "Sleep system constants" as 0.1
alongside an angular-velocity twin, i.e. as a speed; the sleep test compares it
against a kinetic energy in joules:

{{cite ohao/physics/dynamics/rigid_body.cpp "if (kineticEnergy < constants::SLEEP_LINEAR_THRESHOLD) {"}}

Since $\tfrac{1}{2}mv^2 < 0.1$ solves to $v < \sqrt{0.2/m}$, the speed threshold
it encodes is mass-dependent: a 10 kg body must slow to 0.14 m/s, a 0.1 kg body
can drift at 1.4 m/s and still be put to sleep. That comparison has never run —
its enclosing `RigidBody::updateSleepState` has no callers, Jolt owns sleeping —
and the constant's only other readers are `PhysicsConfig::sleepLinearThreshold`
and a `RigidBody` member `m_motionThreshold` that nothing ever reads. It is a
units mismatch preserved in amber, and it would bite on the first frame if the
mirror bodies were ever stepped locally.

:::key
Treat these four files as a specification of a physics core OHAO chose not to
write. The load-bearing part is a thin slice of `physics_math.hpp`: `math::AABB`,
the return type of `CollisionShape::getAABB` and therefore compiled into all six
shapes; `math::transformPoint`, which the box, cylinder and triangle-mesh headers
use to move points between local and world space; `math::safeNormalize`, which
builds the plane's normal and the triangle's face normal; and
`math::constants::EPSILON` and `PI`, the degeneracy guards and the sphere's
volume. `clampLength` belongs to `RigidBody`
alone, and `isNearZero` reaches the shapes at exactly one site, the axis-aligned
fast path in `BoxShape::getAABB`. Everything with the word *inertia*, *contact* or
*sleep* in it is either duplicated, unreferenced, or superseded by Jolt.
:::

## Contracts

- The two `constants` namespaces are independent. Any change to a shared scalar must be made in both `physics_constants.hpp` and `physics_math.hpp`, or callers silently split across two values.
- `physics_constants.cpp` and `physics_math.cpp` define the same six `inertia::` symbols. Scanning repeats until no new member is extracted, and every target additionally links `ohao_physics` inside `-Wl,--start-group … -Wl,--end-group`, so adding a caller for `inertia::combine` extracts `physics_constants.cpp.o` *in addition to* `physics_math.cpp.o` — six duplicated strong definitions. That is a `multiple definition` link failure on the targets that do not pass `-Wl,--allow-multiple-definition` (`tests/engine`, `tests/physics`, `tests/force_generators`) and a silent first-definition-wins pick on the ones that do (the examples, `tests/renderer`). Deleting one of the two files is the fix; picking either one at random is not.
- `calculateCapsuleTensor` in the linked copy treats its `height` argument as the cylindrical section only, while `CapsuleShape::getHeight()` returns the total height including both caps — so the tensor is computed for an over-long capsule. Correct only if the caller subtracts `2 * radius` first, which `RigidBody::calculateInertiaFromShape` does not.
- Jolt derives its own rotational inertia for dynamic bodies (`EOverrideMassProperties::CalculateInertia`), and static/kinematic bodies get no override at all. Nothing in `inertia::*` influences simulated motion — and nothing reads its output either, so a wrong tensor here is invisible until someone wires up the `RigidBody` reporting or integration path.
