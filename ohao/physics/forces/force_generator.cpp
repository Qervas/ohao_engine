#include "force_generator.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <cmath>

namespace ohao {
namespace physics {
namespace forces {

namespace ForceUtils {

float distanceSquared(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB) {
    if (!bodyA || !bodyB) return 0.0f;
    
    glm::vec3 diff = bodyB->getPosition() - bodyA->getPosition();
    return glm::dot(diff, diff);
}

float distance(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB) {
    return std::sqrt(distanceSquared(bodyA, bodyB));
}

glm::vec3 direction(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB) {
    if (!bodyA || !bodyB) return glm::vec3(0.0f);
    
    glm::vec3 diff = bodyB->getPosition() - bodyA->getPosition();
    float dist = glm::length(diff);
    
    if (dist < 1e-6f) {
        return glm::vec3(0.0f, 1.0f, 0.0f); // Default up direction for zero distance
    }
    
    return diff / dist;
}

void applyForceAtWorldPosition(dynamics::RigidBody* body, const glm::vec3& force, const glm::vec3& worldPos) {
    if (!body) return;
    
    body->applyForceAtWorldPoint(force, worldPos);
}

glm::vec3 relativeVelocity(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB, const glm::vec3& contactPoint) {
    if (!bodyA || !bodyB) return glm::vec3(0.0f);
    
    // Calculate velocity at contact point for both bodies
    glm::vec3 velA = bodyA->getLinearVelocity();
    glm::vec3 relPosA = contactPoint - bodyA->getPosition();
    velA += glm::cross(bodyA->getAngularVelocity(), relPosA);
    
    glm::vec3 velB = bodyB->getLinearVelocity();
    glm::vec3 relPosB = contactPoint - bodyB->getPosition();
    velB += glm::cross(bodyB->getAngularVelocity(), relPosB);
    
    return velB - velA;
}

} // namespace ForceUtils

} // namespace forces
} // namespace physics
} // namespace ohao