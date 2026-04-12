#include "gravity_force.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>

namespace ohao {
namespace physics {
namespace forces {

// GravityForce implementation
void GravityForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || (!m_affectStatic && body->isStatic())) {
        return;
    }
    
    // F = m * g (scaled by mass scale factor)
    float mass = body->getMass() * m_massScale;
    glm::vec3 force = m_gravity * mass;
    
    body->applyForce(force);
}

bool GravityForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body) return false;
    if (!m_affectStatic && body->isStatic()) return false;
    return body->isGravityEnabled();
}

// DirectionalGravityForce implementation
void DirectionalGravityForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || (!m_affectStatic && body->isStatic())) {
        return;
    }
    
    float mass = body->getMass();
    glm::vec3 force = m_direction * (m_strength * mass);
    
    body->applyForce(force);
}

bool DirectionalGravityForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body) return false;
    if (!m_affectStatic && body->isStatic()) return false;
    return body->isGravityEnabled();
}

// PointGravityForce implementation
void PointGravityForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || (!m_affectStatic && body->isStatic())) {
        return;
    }
    
    glm::vec3 bodyPos = body->getPosition();
    glm::vec3 direction = m_center - bodyPos;
    float distance = glm::length(direction);
    
    // Check distance bounds
    if (distance < m_minDistance || distance > m_maxDistance) {
        return;
    }
    
    // Normalize direction
    direction /= distance;
    
    // Calculate gravitational force: F = G * m1 * m2 / r^2
    // Simplified: F = strength * mass / distance^2
    float forceMagnitude = (m_strength * body->getMass()) / (distance * distance);
    glm::vec3 force = direction * forceMagnitude;
    
    body->applyForce(force);
}

bool PointGravityForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body) return false;
    if (!m_affectStatic && body->isStatic()) return false;
    if (!body->isGravityEnabled()) return false;
    
    // Check if body is within range
    float distance = glm::length(body->getPosition() - m_center);
    return distance >= m_minDistance && distance <= m_maxDistance;
}

} // namespace forces
} // namespace physics
} // namespace ohao