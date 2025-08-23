#pragma once

#include "physics/utils/physics_math.hpp"
#include <memory>

namespace ohao {
namespace physics {
namespace collision {

// Forward declarations
class CollisionShape;

struct ContactInfo {
    glm::vec3 contactPoint{0.0f};
    glm::vec3 contactNormal{0.0f};
    float penetrationDepth{0.0f};
    bool hasContact{false};
    
    // Additional contact properties
    float restitution{0.0f};
    float friction{0.5f};
    
    ContactInfo() = default;
    
    ContactInfo(const glm::vec3& point, const glm::vec3& normal, float depth)
        : contactPoint(point), contactNormal(normal), penetrationDepth(depth), hasContact(true) {}
        
    bool isValid() const {
        return hasContact && penetrationDepth > 0.0f && !math::isNearZero(contactNormal);
    }
    
    void flip() {
        contactNormal = -contactNormal;
    }
};

} // namespace collision
} // namespace physics
} // namespace ohao