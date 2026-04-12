#include "spring_force.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/utils/physics_math.hpp"
#include <algorithm>

namespace ohao {
namespace physics {
namespace forces {

// SpringForce implementation
void SpringForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!m_bodyA || !m_bodyB) return;
    
    // Calculate world positions of attachment points
    glm::vec3 posA = m_bodyA->getPosition() + m_bodyA->getRotation() * m_attachmentPointA;
    glm::vec3 posB = m_bodyB->getPosition() + m_bodyB->getRotation() * m_attachmentPointB;
    
    // Spring vector (from A to B)
    glm::vec3 springVector = posB - posA;
    float currentLength = glm::length(springVector);
    
    if (currentLength < 1e-6f) return; // Avoid division by zero
    
    glm::vec3 springDirection = springVector / currentLength;
    
    // Spring force magnitude: F = k * (current_length - rest_length)
    float extension = currentLength - m_restLength;
    float springForceMagnitude = m_springConstant * extension;
    
    // Damping force - based on relative velocity along spring direction
    glm::vec3 velA = m_bodyA->getLinearVelocity();
    glm::vec3 velB = m_bodyB->getLinearVelocity();
    
    // Add rotational velocity contributions at attachment points
    glm::vec3 relPosA = posA - m_bodyA->getPosition();
    glm::vec3 relPosB = posB - m_bodyB->getPosition();
    velA += glm::cross(m_bodyA->getAngularVelocity(), relPosA);
    velB += glm::cross(m_bodyB->getAngularVelocity(), relPosB);
    
    glm::vec3 relativeVelocity = velB - velA;
    float dampingForceMagnitude = m_damping * glm::dot(relativeVelocity, springDirection);
    
    // Total force magnitude
    float totalForceMagnitude = springForceMagnitude + dampingForceMagnitude;
    glm::vec3 force = totalForceMagnitude * springDirection;
    
    // Apply forces (Newton's third law - equal and opposite)
    if (body == m_bodyA) {
        ForceUtils::applyForceAtWorldPosition(m_bodyA, force, posA);
    } else if (body == m_bodyB) {
        ForceUtils::applyForceAtWorldPosition(m_bodyB, -force, posB);
    }
}

float SpringForce::getCurrentLength() const {
    if (!m_bodyA || !m_bodyB) return 0.0f;
    
    glm::vec3 posA = m_bodyA->getPosition() + m_bodyA->getRotation() * m_attachmentPointA;
    glm::vec3 posB = m_bodyB->getPosition() + m_bodyB->getRotation() * m_attachmentPointB;
    
    return glm::length(posB - posA);
}

glm::vec3 SpringForce::getSpringDirection() const {
    if (!m_bodyA || !m_bodyB) return glm::vec3(0.0f, 1.0f, 0.0f);
    
    glm::vec3 posA = m_bodyA->getPosition() + m_bodyA->getRotation() * m_attachmentPointA;
    glm::vec3 posB = m_bodyB->getPosition() + m_bodyB->getRotation() * m_attachmentPointB;
    
    glm::vec3 direction = posB - posA;
    float length = glm::length(direction);
    
    return (length > 1e-6f) ? (direction / length) : glm::vec3(0.0f, 1.0f, 0.0f);
}

// AnchorSpringForce implementation
void AnchorSpringForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body) return;
    
    // Calculate world position of attachment point
    glm::vec3 attachmentWorldPos = body->getPosition() + body->getRotation() * m_attachmentPoint;
    
    // Spring vector (from attachment to anchor)
    glm::vec3 springVector = m_anchorPosition - attachmentWorldPos;
    float currentLength = glm::length(springVector);
    
    if (currentLength < 1e-6f) return;
    
    glm::vec3 springDirection = springVector / currentLength;
    
    // Spring force
    float extension = currentLength - m_restLength;
    float springForceMagnitude = m_springConstant * extension;
    
    // Damping force
    glm::vec3 velocity = body->getLinearVelocity();
    glm::vec3 relPos = attachmentWorldPos - body->getPosition();
    velocity += glm::cross(body->getAngularVelocity(), relPos);
    
    float dampingForceMagnitude = m_damping * glm::dot(velocity, springDirection);
    
    // Total force
    float totalForceMagnitude = springForceMagnitude - dampingForceMagnitude;
    glm::vec3 force = totalForceMagnitude * springDirection;
    
    ForceUtils::applyForceAtWorldPosition(body, force, attachmentWorldPos);
}

float AnchorSpringForce::getCurrentLength() const {
    if (!m_targetBody) return 0.0f;
    
    glm::vec3 attachmentWorldPos = m_targetBody->getPosition() + m_targetBody->getRotation() * m_attachmentPoint;
    return glm::length(m_anchorPosition - attachmentWorldPos);
}

// BungeeSpringForce implementation
void BungeeSpringForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!m_bodyA || !m_bodyB) return;
    
    // Calculate distance between bodies
    glm::vec3 springVector = m_bodyB->getPosition() - m_bodyA->getPosition();
    float currentLength = glm::length(springVector);
    
    // Only apply force if stretched beyond rest length
    if (currentLength <= m_restLength) return;
    
    glm::vec3 springDirection = springVector / currentLength;
    float extension = currentLength - m_restLength;
    float forceMagnitude = m_springConstant * extension;
    
    glm::vec3 force = forceMagnitude * springDirection;
    
    // Apply forces
    if (body == m_bodyA) {
        body->applyForce(force);
    } else if (body == m_bodyB) {
        body->applyForce(-force);
    }
}

// AngularSpringForce implementation
AngularSpringForce::AngularSpringForce(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                       float springConstant, float damping)
    : PairForceGenerator(bodyA, bodyB), m_springConstant(springConstant), m_damping(damping) {
    // Store initial relative orientation as rest state
    if (bodyA && bodyB) {
        m_restOrientation = glm::conjugate(bodyA->getRotation()) * bodyB->getRotation();
    }
}

void AngularSpringForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!m_bodyA || !m_bodyB) return;
    
    // Calculate current relative orientation
    glm::quat currentRelative = glm::conjugate(m_bodyA->getRotation()) * m_bodyB->getRotation();
    
    // Calculate orientation error (difference from rest orientation)
    glm::quat errorQuat = currentRelative * glm::conjugate(m_restOrientation);
    
    // Convert quaternion to axis-angle for spring torque calculation
    float angle = 2.0f * std::acos(std::abs(errorQuat.w));
    if (angle > glm::pi<float>()) {
        angle = 2.0f * glm::pi<float>() - angle;
        errorQuat = -errorQuat; // Use shorter rotation
    }
    
    if (angle < 1e-6f) return; // No significant rotation error
    
    // Extract rotation axis
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float sinHalfAngle = std::sqrt(1.0f - errorQuat.w * errorQuat.w);
    if (sinHalfAngle > 1e-6f) {
        axis = glm::vec3(errorQuat.x, errorQuat.y, errorQuat.z) / sinHalfAngle;
    }
    
    // Spring torque magnitude
    float springTorqueMagnitude = m_springConstant * angle;
    
    // Damping torque based on relative angular velocity
    glm::vec3 relativeAngularVel = m_bodyB->getAngularVelocity() - m_bodyA->getAngularVelocity();
    float dampingTorqueMagnitude = m_damping * glm::dot(relativeAngularVel, axis);
    
    // Total torque
    float totalTorqueMagnitude = springTorqueMagnitude + dampingTorqueMagnitude;
    glm::vec3 torque = totalTorqueMagnitude * axis;
    
    // Apply torques (equal and opposite)
    if (body == m_bodyA) {
        body->applyTorque(-torque);
    } else if (body == m_bodyB) {
        body->applyTorque(torque);
    }
}

} // namespace forces
} // namespace physics
} // namespace ohao