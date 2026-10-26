#pragma once
// The physics scene (sub-project 5, docs/specs/2026-10-26-level-pipeline-design.md, "Collision
// generation"; stepped since sub-project 6, docs/specs/2026-10-26-player-physics-design.md): a
// Jolt world (physics::backend) holding one body per entity with a Collider (and the body
// entities of ColliderParts), static unless the entity has a RigidBody.
//
//   - The `physics` sync system, right after `transform`, keeps it in step through dirty
//     state, so a still world costs nothing: Collider, RigidBody, ColliderPart, CollisionMesh
//     and MeshSource changes (and destroyed entities) rebuild their body, in creation order;
//     PhysicsMoved (tagged by the transform system) moves a body to its entity.
//   - The `physics` simulate system steps it once per fixed step (step()): propagate dirty
//     transforms, the same sync, Jolt in substeps of at most 1/60 s, every awake dynamic
//     body's pose written back to its entity's Transform, then the step's events.
//   - A dynamic body's entity moved by anything but that write-back (an edit, a restore, a
//     moved parent) teleports the body there, at rest.
//   - Events: contact begin/end per body pair (sub-shape contacts collapsed) and trigger
//     enter/exit (a Collider with trigger is a sensor), drained from the backend's queue on
//     the simulating thread after the step, sorted, and published as the step's span and in
//     a sequenced log of the last kEventLog. Removing a body ends its pairs at once.
//   - Shapes are fitted to the entity's mesh in its own frame and baked with its world scale
//     (fitCollider); a body's position and rotation are its entity's world transform without
//     the scale. A mirrored mesh has its winding flipped, so it still faces out.
//   - A dynamic Mesh collider is built Convex (Jolt does not simulate triangle meshes), with
//     W_LEVEL_DYNAMIC_MESH. ColliderParts (a level's UCX_ hulls) join their body entity's own
//     collider in one compound shape.
//   - The backend is created at the first body, so a world without colliders pays nothing.
//
// No reflection: only components and the backend.
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/world.hpp"
#include "physics/backend/physics_backend.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ohao::ecs {

// A mesh's positions and triangles, in its own frame.
struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
};

// A collider fitted to a mesh (spec "Fitting from the node's mesh"), in the entity's frame with
// its scale baked in.
struct FittedCollider {
    ColliderShape shape{ColliderShape::Box};
    glm::vec3 halfExtents{0.0f};        // Box
    float radius{0.0f};                 // Sphere, Capsule
    float halfHeight{0.0f};             // Capsule: half the length of its straight part
    glm::quat rotation{1, 0, 0, 0};     // Capsule: from +Y to its axis
    glm::vec3 offset{0.0f};             // the shape's centre
    std::vector<glm::vec3> vertices;    // Convex, Mesh
    std::vector<uint32_t> indices;      // Mesh, wound to face out
    bool roundedScale{false};           // Sphere, Capsule: the scale's axes differ by > 1%; the largest is used
};

// box: the AABB, its centre the offset; sphere: the AABB centre to the farthest vertex;
// capsule: along the AABB's longest axis, the radius the larger other half extent; convex
// and mesh: the vertices (and triangles) scaled. Non-uniform scale is baked into vertices and
// box extents; a sphere or capsule takes the largest axis.
[[nodiscard]] FittedCollider fitCollider(const MeshData& mesh, ColliderShape shape, glm::vec3 scale);

struct RayHit {
    entt::entity entity{entt::null};
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    float dist{0.0f};
};

struct PhysicsWarning {
    std::string code;   // W_LEVEL_DYNAMIC_MESH, W_COLLIDER_NO_MESH, W_COLLIDER_SCALE, W_PHYSICS_PARENT_SCALE
    entt::entity entity{entt::null};
    std::string message;
};

// Something physics decided in one step (spec "Events").
struct PhysicsEvent {
    enum class Kind : uint8_t { ContactBegin, ContactEnd, TriggerEnter, TriggerExit };
    Kind kind{Kind::ContactBegin};
    // Contacts: a has the lower entity id. Triggers: a is the trigger, b the body. The entities
    // may be gone by the time the event is read; the ids are kept.
    entt::entity a{entt::null}, b{entt::null};
    uint64_t aId{0}, bId{0};
    glm::vec3 point{0.0f};    // world; an end repeats its begin's
    glm::vec3 normal{0.0f};   // world, from a toward b
    float speed{0.0f};        // the relative normal speed at a begin or enter, m/s; 0 for an end or exit
    uint64_t step{0};         // the scene's step it was published in (1 is the first); a body removed between steps ends its pairs in the next
    uint64_t seq{0};          // its place in the log, from 1
};
[[nodiscard]] const char* physicsEventKindName(PhysicsEvent::Kind k) noexcept;   // contact_begin, ...

struct PhysicsInfo {
    struct BodyCounts {
        uint32_t statics{0}, dynamics{0}, kinematics{0};
    } bodies;
    struct ShapeCounts {
        uint32_t box{0}, sphere{0}, capsule{0}, convex{0}, mesh{0}, compound{0};
    } shapes;
    double buildMs{0.0};    // total time spent building bodies
    uint64_t steps{0};      // physics steps run
    uint32_t awake{0};      // dynamic bodies awake
    uint32_t triggers{0};   // sensor bodies
    uint32_t contacts{0};   // body pairs in contact now (triggers' overlaps included)
};

class PhysicsScene {
public:
    PhysicsScene(entt::registry& reg, AssetResolver& assets);
    ~PhysicsScene();
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    static constexpr std::size_t kEventLog = 1024;   // events kept for eventsSince
    static constexpr int kMaxSubsteps = 8;

    // Builds, moves and removes bodies from the dirty state; no work when nothing changed.
    void sync(entt::registry& reg);
    // One fixed step of dt (the `physics` simulate system): propagate transforms, sync, step in
    // ceil(dt * 60) substeps (at most kMaxSubsteps), write back, publish events. A world with
    // no collider does nothing (not even the propagation).
    void step(entt::registry& reg, float dt);
    // The latest step's events, sorted (removals since the step before it first).
    [[nodiscard]] std::span<const PhysicsEvent> stepEvents() const noexcept;
    // Up to max logged events with seq > since, oldest first; dropped is how many of those the
    // log no longer holds.
    [[nodiscard]] std::vector<PhysicsEvent> eventsSince(uint64_t since, std::size_t max, uint64_t& dropped) const;
    // The seq the next event will get (events so far + 1).
    [[nodiscard]] uint64_t nextEventSeq() const noexcept;
    [[nodiscard]] uint64_t steps() const noexcept;
    // Jolt updates the last step ran (its substeps).
    [[nodiscard]] int lastSubsteps() const noexcept;
    // Where the last step's time went (physics_bench reads it).
    struct StepTiming {
        double totalMs{0.0};    // the whole step
        double joltMs{0.0};     // the substeps
        double eventsMs{0.0};   // draining, collapsing, sorting and logging the events
        double writeMs{0.0};    // the write-back
        uint32_t writes{0};     // Transforms written
    };
    [[nodiscard]] StepTiming lastStepTiming() const noexcept;
    // The closest body along the ray within max (dir need not be normalized).
    [[nodiscard]] std::optional<RayHit> raycast(glm::vec3 from, glm::vec3 dir, float max) const;
    [[nodiscard]] PhysicsInfo info() const;
    // The backend, created on first use; tests step it.
    [[nodiscard]] physics::backend::IPhysicsBackend& backend();
    // The entity's body, or INVALID_BODY.
    [[nodiscard]] physics::backend::BodyHandle bodyOf(entt::entity e) const;
    [[nodiscard]] const std::vector<PhysicsWarning>& warnings() const noexcept;
    // Bodies built so far (each rebuild counts): a still world adds none.
    [[nodiscard]] std::size_t bodyBuilds() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Adds the "physics" sync system right after "transform" and the "physics" simulate system
// after those added so far, and returns the scene (once per World; a second call returns the
// first). The resolver must outlive the World.
PhysicsScene& installPhysics(World& world, AssetResolver& assets);
// The World's physics scene, or null without installPhysics.
[[nodiscard]] PhysicsScene* physicsScene(World& world);
[[nodiscard]] const PhysicsScene* physicsScene(const World& world);
// The latest physics step's events (empty without installPhysics): what a simulate system
// added after installPhysics reads in the same step.
[[nodiscard]] std::span<const PhysicsEvent> physicsEvents(const World& world);

} // namespace ohao::ecs
