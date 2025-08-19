#pragma once

#include "contact_info.hpp"
#include "../dynamics/rigid_body.hpp"
#include "shapes/collision_shape.hpp"
#include <vector>
#include <memory>

namespace ohao {
namespace physics {
namespace collision {

// Collision detection between different shape types
class CollisionDetector {
public:
    // Main collision detection interface
    static ContactInfo detectCollision(
        dynamics::RigidBody* bodyA, 
        dynamics::RigidBody* bodyB
    );
    
    // Broad phase collision detection (AABB overlap test)
    static bool broadPhaseCheck(
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB
    );
    
    // Narrow phase collision detection (exact shape tests)
    static ContactInfo narrowPhaseCheck(
        CollisionShape* shapeA, const glm::vec3& posA, const glm::quat& rotA,
        CollisionShape* shapeB, const glm::vec3& posB, const glm::quat& rotB
    );
    
private:
    // Shape-specific collision tests
    static ContactInfo testBoxVsBox(
        const class BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
        const class BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB
    );
    
    static ContactInfo testSphereVsSphere(
        const class SphereShape* sphereA, const glm::vec3& posA,
        const class SphereShape* sphereB, const glm::vec3& posB
    );
    
    static ContactInfo testBoxVsSphere(
        const class BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
        const class SphereShape* sphere, const glm::vec3& spherePos
    );
    
    // Helper functions
    static glm::vec3 closestPointOnBox(
        const glm::vec3& point, 
        const glm::vec3& boxCenter, 
        const glm::vec3& boxHalfExtents,
        const glm::quat& boxRotation
    );
    
    static ContactInfo createBoxBoxContact(
        const glm::vec3& posA, const glm::vec3& halfExtentsA,
        const glm::vec3& posB, const glm::vec3& halfExtentsB
    );
};

} // namespace collision
} // namespace physics
} // namespace ohao