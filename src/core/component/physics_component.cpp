#include "physics_component.hpp"

namespace ohao {

/* Physics system temporarily disabled
#include "../actor/actor.hpp"
#include "transform_component.hpp"
#include "../scene/scene.hpp"
#include "../physics/collision_shape.hpp"

PhysicsComponent::PhysicsComponent()
    : mass(1.0f)
    , linearVelocity(0.0f)
    , angularVelocity(0.0f)
    , force(0.0f)
    , torque(0.0f)
    , isStaticBody(false)
    , gravityEnabled(true)
    , friction(0.5f)
    , restitution(0.2f)
{
}

PhysicsComponent::~PhysicsComponent() {
    destroy();
    }
/* Physics implementation temporarily disabled

void PhysicsComponent::setMass(float newMass) {
    if (newMass <= 0.0f) {
        // Mass must be positive, so set to a very small value instead
        mass = 0.001f;
    } else {
        mass = newMass;
    }
}

float PhysicsComponent::getMass() const {
    return mass;
}

void PhysicsComponent::setLinearVelocity(const glm::vec3& velocity) {
    if (isStaticBody) return; // Static bodies can't move
    linearVelocity = velocity;
}

glm::vec3 PhysicsComponent::getLinearVelocity() const {
    return linearVelocity;
}

void PhysicsComponent::setAngularVelocity(const glm::vec3& velocity) {
    if (isStaticBody) return; // Static bodies can't rotate
    angularVelocity = velocity;
}

glm::vec3 PhysicsComponent::getAngularVelocity() const {
    return angularVelocity;
}

void PhysicsComponent::applyForce(const glm::vec3& newForce) {
    if (isStaticBody) return; // Static bodies don't respond to forces
    force += newForce;
}

void PhysicsComponent::applyImpulse(const glm::vec3& impulse) {
    if (isStaticBody) return; // Static bodies don't respond to impulses
    
    // Impulse = change in momentum (mass * velocity)
    linearVelocity += impulse / mass;
}

void PhysicsComponent::applyTorque(const glm::vec3& newTorque) {
    if (isStaticBody) return; // Static bodies don't respond to torque
    torque += newTorque;
}

void PhysicsComponent::setStatic(bool staticBody) {
    isStaticBody = staticBody;
    
    if (isStaticBody) {
        // Static bodies don't move
        linearVelocity = glm::vec3(0.0f);
        angularVelocity = glm::vec3(0.0f);
        force = glm::vec3(0.0f);
        torque = glm::vec3(0.0f);
    }
}

bool PhysicsComponent::isStatic() const {
    return isStaticBody;
}

void PhysicsComponent::setGravityEnabled(bool enabled) {
    gravityEnabled = enabled;
}

bool PhysicsComponent::isGravityEnabled() const {
    return gravityEnabled;
}

void PhysicsComponent::setFriction(float newFriction) {
    friction = glm::clamp(newFriction, 0.0f, 1.0f);
}

float PhysicsComponent::getFriction() const {
    return friction;
}

void PhysicsComponent::setRestitution(float newRestitution) {
    restitution = glm::clamp(newRestitution, 0.0f, 1.0f);
}

float PhysicsComponent::getRestitution() const {
    return restitution;
}

void PhysicsComponent::setCollisionShape(std::shared_ptr<CollisionShape> shape) {
    collisionShape = shape;
}

std::shared_ptr<CollisionShape> PhysicsComponent::getCollisionShape() const {
    return collisionShape;
}

void PhysicsComponent::createBoxShape(const glm::vec3& halfExtents) {
    collisionShape = std::make_shared<BoxShape>(halfExtents);
}

void PhysicsComponent::createSphereShape(float radius) {
    collisionShape = std::make_shared<SphereShape>(radius);
}

void PhysicsComponent::createCapsuleShape(float radius, float height) {
    collisionShape = std::make_shared<CapsuleShape>(radius, height);
}

void PhysicsComponent::createConvexHullShape(const std::vector<glm::vec3>& points) {
    collisionShape = std::make_shared<ConvexHullShape>(points);
}

void PhysicsComponent::initialize() {
    // Register with the physics system
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->onPhysicsComponentAdded(this);
        }
    }
}

void PhysicsComponent::update(float deltaTime) {
    if (isStaticBody) return; // Static bodies don't move
    
    // Apply gravity if enabled
    if (gravityEnabled) {
        // TODO: Get gravity from physics system instead of hardcoding
        glm::vec3 gravity(0.0f, -9.81f, 0.0f);
        applyForce(mass * gravity);
    }
    
    // Integrate physics
    integrateForces(deltaTime);
    integrateVelocity(deltaTime);
    
    // Clear forces for next frame
    force = glm::vec3(0.0f);
    torque = glm::vec3(0.0f);
    
    // Update transform component
    updateTransform();
}

void PhysicsComponent::destroy() {
    // Unregister from the physics system
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->onPhysicsComponentRemoved(this);
        }
    }
}

const char* PhysicsComponent::getTypeName() const {
    return "PhysicsComponent";
}

void PhysicsComponent::onCollisionBegin(PhysicsComponent* other) {
    // Called when collision begins
    // TODO: Implement collision response and event system
}

void PhysicsComponent::onCollisionEnd(PhysicsComponent* other) {
    // Called when collision ends
    // TODO: Implement collision response and event system
}

void PhysicsComponent::serialize(class Serializer& serializer) const {
    // TODO: Implement serialization
}

void PhysicsComponent::deserialize(class Deserializer& deserializer) {
    // TODO: Implement deserialization
}

void PhysicsComponent::integrateForces(float deltaTime) {
    // Semi-implicit Euler integration for forces
    // v = v + (F/m) * dt
    linearVelocity += (force / mass) * deltaTime;
    
    // TODO: Replace with proper angular integration
    angularVelocity += torque * deltaTime;
}

void PhysicsComponent::integrateVelocity(float deltaTime) {
    // Semi-implicit Euler integration for velocity
    // x = x + v * dt
    if (auto actor = getOwner()) {
        if (auto transform = actor->getTransform()) {
            glm::vec3 position = transform->getPosition();
            position += linearVelocity * deltaTime;
            transform->setPosition(position);
            
            // Apply angular velocity to rotation
            // Simple Euler integration for now
            glm::vec3 eulerAngles = transform->getRotationEuler();
            eulerAngles += glm::degrees(angularVelocity) * deltaTime;
            transform->setRotationEuler(eulerAngles);
        }
    }
}

void PhysicsComponent::updateTransform() {
    // Update the transform from physics state
    // Already handled by integrateVelocity for now
}
*/ // End of physics implementation

} // namespace ohao