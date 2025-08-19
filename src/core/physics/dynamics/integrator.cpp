#include "integrator.hpp"
#include "rigid_body.hpp"
#include "../utils/physics_math.hpp"

namespace ohao {
namespace physics {
namespace dynamics {

void Integrator::integrateVelocity(RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    // Get accumulated forces
    glm::vec3 totalForce = body->getAccumulatedForce();
    glm::vec3 totalTorque = body->getAccumulatedTorque();
    
    // Calculate acceleration from forces: a = F / m
    float invMass = body->getInverseMass();
    if (invMass > 0.0f) {
        glm::vec3 acceleration = totalForce * invMass;
        
        // Update linear velocity: v = v + a * dt
        glm::vec3 newVelocity = body->getLinearVelocity() + acceleration * deltaTime;
        body->setLinearVelocity(newVelocity);
        
        // Update angular velocity: ω = ω + (τ * invI) * dt
        // Simplified: assume uniform sphere inertia for now
        glm::vec3 angularAcceleration = totalTorque * invMass; // Simplified
        glm::vec3 newAngularVelocity = body->getAngularVelocity() + angularAcceleration * deltaTime;
        body->setAngularVelocity(newAngularVelocity);
    }
}

void Integrator::integratePosition(RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    // Update position: p = p + v * dt
    glm::vec3 newPosition = body->getPosition() + body->getLinearVelocity() * deltaTime;
    body->setPosition(newPosition);
    
    // Update rotation: q = q + 0.5 * ω * q * dt
    glm::vec3 angularVelocity = body->getAngularVelocity();
    if (!math::isNearZero(angularVelocity)) {
        glm::quat newRotation = math::integrateAngularVelocity(
            body->getRotation(), 
            angularVelocity, 
            deltaTime
        );
        body->setRotation(newRotation);
    }
}

void Integrator::integratePhysics(RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    // Semi-implicit Euler: update velocity first, then position
    integrateVelocity(body, deltaTime);
    integratePosition(body, deltaTime);
    
    // Apply damping
    applyDamping(body, deltaTime);
    
    // Clamp velocities to prevent instability
    clampVelocities(body);
    
    // Clear forces for next frame
    body->clearForces();
}

void Integrator::applyDamping(RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    // Apply linear damping: v = v * (1 - damping)^dt
    float linearDamping = body->getLinearDamping();
    if (linearDamping > 0.0f) {
        float dampingFactor = glm::pow(1.0f - linearDamping, deltaTime);
        body->setLinearVelocity(body->getLinearVelocity() * dampingFactor);
    }
    
    // Apply angular damping: ω = ω * (1 - damping)^dt  
    float angularDamping = body->getAngularDamping();
    if (angularDamping > 0.0f) {
        float dampingFactor = glm::pow(1.0f - angularDamping, deltaTime);
        body->setAngularVelocity(body->getAngularVelocity() * dampingFactor);
    }
}

void Integrator::clampVelocities(RigidBody* body, float maxLinearVel, float maxAngularVel) {
    if (!body) return;
    
    // Clamp linear velocity
    glm::vec3 linearVel = body->getLinearVelocity();
    if (glm::length(linearVel) > maxLinearVel) {
        body->setLinearVelocity(math::safeNormalize(linearVel) * maxLinearVel);
    }
    
    // Clamp angular velocity
    glm::vec3 angularVel = body->getAngularVelocity();
    if (glm::length(angularVel) > maxAngularVel) {
        body->setAngularVelocity(math::safeNormalize(angularVel) * maxAngularVel);
    }
}

} // namespace dynamics
} // namespace physics
} // namespace ohao