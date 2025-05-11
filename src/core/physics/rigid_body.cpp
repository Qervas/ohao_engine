#include "rigid_body.hpp"

namespace ohao {

void RigidBody::setMass(float newMass) {
    mass = newMass > 0.0f ? newMass : 0.0001f; // Ensure positive mass
}

float RigidBody::getMass() const {
    return mass;
}

void RigidBody::setPosition(const glm::vec3& newPosition) {
    position = newPosition;
}

glm::vec3 RigidBody::getPosition() const {
    return position;
}

void RigidBody::setRotation(const glm::quat& newRotation) {
    rotation = newRotation;
}

glm::quat RigidBody::getRotation() const {
    return rotation;
}

void RigidBody::setLinearVelocity(const glm::vec3& velocity) {
    linearVelocity = velocity;
}

glm::vec3 RigidBody::getLinearVelocity() const {
    return linearVelocity;
}

void RigidBody::setAngularVelocity(const glm::vec3& velocity) {
    angularVelocity = velocity;
}

glm::vec3 RigidBody::getAngularVelocity() const {
    return angularVelocity;
}

void RigidBody::applyForce(const glm::vec3& force, const glm::vec3& relativePos) {
    // For now, just store the force
    // In a real implementation, we would apply the force to the rigidbody
}

void RigidBody::applyTorque(const glm::vec3& torque) {
    // For now, just store the torque
    // In a real implementation, we would apply the torque to the rigidbody
}

void RigidBody::applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos) {
    // Impulse = change in momentum
    linearVelocity += impulse / mass;
}

void RigidBody::applyTorqueImpulse(const glm::vec3& torqueImpulse) {
    // Apply an instantaneous torque impulse
    // In a real implementation, we would calculate the angular velocity change
}

} // namespace ohao 