#include "drag_force.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>
#include <cmath>

namespace ohao {
namespace physics {
namespace forces {

// LinearDragForce implementation
void LinearDragForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) {
        return;
    }
    
    glm::vec3 velocity = body->getLinearVelocity();
    glm::vec3 dragForce = -m_dragCoefficient * velocity;
    
    body->applyForce(dragForce);
}

bool LinearDragForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    // Only apply drag if body has significant velocity
    glm::vec3 velocity = body->getLinearVelocity();
    return glm::length(velocity) > 1e-6f;
}

// QuadraticDragForce implementation  
void QuadraticDragForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) {
        return;
    }
    
    glm::vec3 velocity = body->getLinearVelocity();
    float speed = glm::length(velocity);
    
    if (speed < 1e-6f) return; // No drag for stationary objects
    
    // F = -k * |v| * v = -k * speed * velocity
    glm::vec3 dragForce = -m_dragCoefficient * speed * velocity;
    
    body->applyForce(dragForce);
}

bool QuadraticDragForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    glm::vec3 velocity = body->getLinearVelocity();
    return glm::length(velocity) > 1e-6f;
}

// CombinedDragForce implementation
void CombinedDragForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) {
        return;
    }
    
    glm::vec3 velocity = body->getLinearVelocity();
    float speed = glm::length(velocity);
    
    if (speed < 1e-6f) return;
    
    // Combined: F = -k1 * v - k2 * |v| * v
    glm::vec3 linearDrag = -m_linearCoeff * velocity;
    glm::vec3 quadraticDrag = -m_quadraticCoeff * speed * velocity;
    glm::vec3 totalDragForce = linearDrag + quadraticDrag;
    
    body->applyForce(totalDragForce);
}

bool CombinedDragForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    glm::vec3 velocity = body->getLinearVelocity();
    return glm::length(velocity) > 1e-6f;
}

// AngularDragForce implementation
void AngularDragForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) {
        return;
    }
    
    glm::vec3 angularVelocity = body->getAngularVelocity();
    
    // Apply angular drag as torque opposing angular motion
    glm::vec3 angularDragTorque = -m_angularDragCoeff * angularVelocity;
    
    body->applyTorque(angularDragTorque);
}

bool AngularDragForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    glm::vec3 angularVelocity = body->getAngularVelocity();
    return glm::length(angularVelocity) > 1e-6f;
}

// FluidDragForce implementation
void FluidDragForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) {
        return;
    }
    
    glm::vec3 velocity = body->getLinearVelocity();
    float speed = glm::length(velocity);
    
    if (speed < 1e-6f) return;
    
    // Realistic fluid drag: F = 0.5 * ρ * Cd * A * v²
    // Direction opposite to velocity
    glm::vec3 velocityDirection = velocity / speed;
    float dragMagnitude = 0.5f * m_fluidDensity * m_dragCoeff * m_crossSectionArea * (speed * speed);
    glm::vec3 dragForce = -dragMagnitude * velocityDirection;
    
    body->applyForce(dragForce);
}

bool FluidDragForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    glm::vec3 velocity = body->getLinearVelocity();
    return glm::length(velocity) > 1e-6f;
}

} // namespace forces
} // namespace physics
} // namespace ohao