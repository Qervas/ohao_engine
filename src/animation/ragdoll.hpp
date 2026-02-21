#pragma once

#include "skeleton.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/world/physics_world.hpp"
#include "physics/collision/shapes/shape_factory.hpp"
#include <memory>
#include <vector>

namespace ohao {

// Represents a single ragdoll bone: a rigid body connected to the skeleton
struct RagdollBone {
    int jointIndex{-1};
    std::shared_ptr<physics::dynamics::RigidBody> rigidBody;
    glm::vec3 halfExtents{0.05f};  // Collision shape size
    float mass{1.0f};
};

// Constraint connecting two ragdoll bones (e.g., cone twist, hinge)
struct RagdollConstraint {
    int parentBoneIndex{-1};
    int childBoneIndex{-1};
    glm::vec3 pivotInParent{0.0f};
    glm::vec3 pivotInChild{0.0f};
    float coneAngle{0.5f};     // radians, max swing angle
    float twistAngle{0.3f};    // radians, max twist
};

// Converts a skeleton hierarchy into a ragdoll physics setup.
// When enabled, skeleton joints are driven by physics rigid bodies
// instead of animation data.
class Ragdoll {
public:
    Ragdoll() = default;
    ~Ragdoll();

    // Build ragdoll from skeleton - creates rigid bodies for each joint
    // and constraints between parent-child joints.
    // Call this once during setup (e.g., when loading an animated character).
    bool buildFromSkeleton(const Skeleton& skeleton, physics::PhysicsWorld* world);

    // Activate ragdoll (switch from animation to physics-driven)
    // Sets all rigid bodies to dynamic and applies initial velocity
    void activate(const glm::vec3& impulse = glm::vec3(0.0f));

    // Deactivate ragdoll (switch back to animation-driven)
    void deactivate();

    // Update skeleton joint matrices from ragdoll rigid body positions
    // Call each frame when ragdoll is active to sync physics -> skeleton
    void updateSkeleton(Skeleton& skeleton) const;

    bool isActive() const { return m_active; }
    size_t getBoneCount() const { return m_bones.size(); }

    // Clean up all physics resources
    void cleanup();

private:
    // Estimate bone dimensions from skeleton hierarchy
    glm::vec3 estimateBoneExtents(const Skeleton& skeleton, int jointIndex) const;

    std::vector<RagdollBone> m_bones;
    std::vector<RagdollConstraint> m_constraints;
    physics::PhysicsWorld* m_world{nullptr};
    bool m_active{false};
};

} // namespace ohao
