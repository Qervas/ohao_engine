#pragma once
// The physics scene (sub-project 5, docs/specs/2026-10-26-level-pipeline-design.md, "Collision
// generation"): a Jolt world (physics::backend) holding one body per entity with a Collider
// (and the body entities of ColliderParts), static unless the entity has a RigidBody.
//
//   - The `physics` sync system, right after `transform`, keeps it in step through dirty
//     state, so a still world costs nothing: Collider, RigidBody, ColliderPart, CollisionMesh
//     and MeshSource changes (and destroyed entities) rebuild their body, in creation order;
//     PhysicsMoved (tagged by the transform system) moves a body to its entity.
//   - Shapes are fitted to the entity's mesh in its own frame and baked with its world scale
//     (fitCollider); a body's position and rotation are its entity's world transform without
//     the scale. A mirrored mesh has its winding flipped, so it still faces out.
//   - A dynamic Mesh collider is built Convex (Jolt does not simulate triangle meshes), with
//     W_LEVEL_DYNAMIC_MESH. ColliderParts (a level's UCX_ hulls) join their body entity's own
//     collider in one compound shape.
//   - Dynamic bodies are built but not stepped: nothing steps the backend until sub-project 6
//     (tests step it through backend()).
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
    std::string code;   // W_LEVEL_DYNAMIC_MESH, W_COLLIDER_NO_MESH, W_COLLIDER_SCALE
    entt::entity entity{entt::null};
    std::string message;
};

struct PhysicsInfo {
    struct BodyCounts {
        uint32_t statics{0}, dynamics{0}, kinematics{0};
    } bodies;
    struct ShapeCounts {
        uint32_t box{0}, sphere{0}, capsule{0}, convex{0}, mesh{0}, compound{0};
    } shapes;
    double buildMs{0.0};    // total time spent building bodies
    uint32_t unstepped{0};  // dynamic bodies, which nothing steps until sub-project 6
};

class PhysicsScene {
public:
    PhysicsScene(entt::registry& reg, AssetResolver& assets);
    ~PhysicsScene();
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    // Builds, moves and removes bodies from the dirty state; no work when nothing changed.
    void sync(entt::registry& reg);
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

// Adds the "physics" sync system right after "transform" and returns the scene (once per
// World; a second call returns the first). The resolver must outlive the World.
PhysicsScene& installPhysics(World& world, AssetResolver& assets);
// The World's physics scene, or null without installPhysics.
[[nodiscard]] PhysicsScene* physicsScene(World& world);
[[nodiscard]] const PhysicsScene* physicsScene(const World& world);

} // namespace ohao::ecs
