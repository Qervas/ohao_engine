#include "physics_component.hpp"
#include "../actor/actor.hpp"
#include "../scene/scene.hpp"
#include "../physics/collision_shape.hpp"
#include <nlohmann/json.hpp>

namespace ohao {

PhysicsComponent::PhysicsComponent(Actor* owner)
    : Component(owner)
    , mass(1.0f)
    , friction(0.5f)
    , restitution(0.2f)
    , bodyType(BodyType::STATIC)
    , linearDamping(0.0f)
    , angularDamping(0.0f)
    , gravityEnabled(true)
    , collisionEnabled(true)
{
}

PhysicsComponent::~PhysicsComponent() {
    // Clean up physics resources
    collisionShape.reset();
    rigidBody.reset();
}

void PhysicsComponent::setStatic(bool isStatic) {
    beginModification();
    bodyType = isStatic ? BodyType::STATIC : BodyType::DYNAMIC;
    updateRigidBody();
    endModification();
}

void PhysicsComponent::setDynamic(bool isDynamic) {
    beginModification();
    bodyType = isDynamic ? BodyType::DYNAMIC : BodyType::STATIC;
    updateRigidBody();
    endModification();
}

void PhysicsComponent::setKinematic(bool isKinematic) {
    beginModification();
    bodyType = isKinematic ? BodyType::KINEMATIC : BodyType::STATIC;
    updateRigidBody();
    endModification();
}

void PhysicsComponent::setBodyType(BodyType type) {
    if (bodyType == type) return;
    
    beginModification();
    bodyType = type;
    updateRigidBody();
    endModification();
}

PhysicsComponent::BodyType PhysicsComponent::getBodyType() const {
    return bodyType;
}

void PhysicsComponent::setMass(float newMass) {
    if (mass == newMass) return;
    
    beginModification();
    mass = newMass > 0.0f ? newMass : 0.0001f;
    updateRigidBody();
    endModification();
}

float PhysicsComponent::getMass() const {
    return mass;
}

void PhysicsComponent::setFriction(float newFriction) {
    if (friction == newFriction) return;
    
    beginModification();
    friction = newFriction;
    
    // Update rigidbody if it exists
    if (rigidBody) {
        // TODO: Update friction on actual physics body
    }
    
    endModification();
}

float PhysicsComponent::getFriction() const {
    return friction;
}

void PhysicsComponent::setRestitution(float newRestitution) {
    if (restitution == newRestitution) return;
    
    beginModification();
    restitution = newRestitution;
    
    // Update rigidbody if it exists
    if (rigidBody) {
        // TODO: Update restitution on actual physics body
    }
    
    endModification();
}

float PhysicsComponent::getRestitution() const {
    return restitution;
}

void PhysicsComponent::setLinearDamping(float damping) {
    if (linearDamping == damping) return;
    
    beginModification();
    linearDamping = damping;
    
    // Update rigidbody if it exists
    if (rigidBody) {
        // TODO: Update linear damping on actual physics body
    }
    
    endModification();
}

float PhysicsComponent::getLinearDamping() const {
    return linearDamping;
}

void PhysicsComponent::setAngularDamping(float damping) {
    if (angularDamping == damping) return;
    
    beginModification();
    angularDamping = damping;
    
    // Update rigidbody if it exists
    if (rigidBody) {
        // TODO: Update angular damping on actual physics body
    }
    
    endModification();
}

float PhysicsComponent::getAngularDamping() const {
    return angularDamping;
}

void PhysicsComponent::createBoxShape(const glm::vec3& size) {
    beginModification();
    
    // Create box collision shape
    collisionShape = std::make_unique<CollisionShape>();
    collisionShape->createBox(size);
    
    // Update rigidbody
    updateRigidBody();
    
    endModification();
}

void PhysicsComponent::createSphereShape(float radius) {
    beginModification();
    
    // Create sphere collision shape
    collisionShape = std::make_unique<CollisionShape>();
    collisionShape->createSphere(radius);
    
    // Update rigidbody
    updateRigidBody();
    
    endModification();
}

void PhysicsComponent::createCapsuleShape(float radius, float height) {
    beginModification();
    
    // Create capsule collision shape
    collisionShape = std::make_unique<CollisionShape>();
    collisionShape->createCapsule(radius, height);
    
    // Update rigidbody
    updateRigidBody();
    
    endModification();
}

void PhysicsComponent::createConvexHullShape(const std::vector<glm::vec3>& vertices) {
    beginModification();
    
    // Create convex hull collision shape
    collisionShape = std::make_unique<CollisionShape>();
    collisionShape->createConvexHull(vertices);
    
    // Update rigidbody
    updateRigidBody();
    
    endModification();
}

void PhysicsComponent::createMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<unsigned int>& indices) {
    beginModification();
    
    // Create mesh collision shape
    collisionShape = std::make_unique<CollisionShape>();
    collisionShape->createTriangleMesh(vertices, indices);
    
    // Update rigidbody
    updateRigidBody();
    
    endModification();
}

CollisionShape* PhysicsComponent::getCollisionShape() const {
    return collisionShape.get();
}

void PhysicsComponent::setLinearVelocity(const glm::vec3& velocity) {
    // Only works on dynamic/kinematic bodies
    if (bodyType == BodyType::STATIC) return;
    
    // Update the physics representation
    if (rigidBody) {
        // TODO: Update velocity on actual physics body
    }
}

glm::vec3 PhysicsComponent::getLinearVelocity() const {
    if (rigidBody) {
        // TODO: Get velocity from actual physics body
        return glm::vec3(0.0f);
    }
    return glm::vec3(0.0f);
}

void PhysicsComponent::setAngularVelocity(const glm::vec3& velocity) {
    // Only works on dynamic/kinematic bodies
    if (bodyType == BodyType::STATIC) return;
    
    // Update the physics representation
    if (rigidBody) {
        // TODO: Update angular velocity on actual physics body
    }
}

glm::vec3 PhysicsComponent::getAngularVelocity() const {
    if (rigidBody) {
        // TODO: Get angular velocity from actual physics body
        return glm::vec3(0.0f);
        }
    return glm::vec3(0.0f);
}

void PhysicsComponent::applyForce(const glm::vec3& force, const glm::vec3& relativePosition) {
    // Only works on dynamic bodies
    if (bodyType != BodyType::DYNAMIC || !rigidBody) return;
    
    // TODO: Apply force to the actual physics body
}

void PhysicsComponent::applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePosition) {
    // Only works on dynamic bodies
    if (bodyType != BodyType::DYNAMIC || !rigidBody) return;
    
    // TODO: Apply impulse to the actual physics body
}

void PhysicsComponent::applyTorque(const glm::vec3& torque) {
    // Only works on dynamic bodies
    if (bodyType != BodyType::DYNAMIC || !rigidBody) return;
    
    // TODO: Apply torque to the actual physics body
}

void PhysicsComponent::applyTorqueImpulse(const glm::vec3& torque) {
    // Only works on dynamic bodies
    if (bodyType != BodyType::DYNAMIC || !rigidBody) return;
    
    // TODO: Apply torque impulse to the actual physics body
}

void PhysicsComponent::setGravityEnabled(bool enabled) {
    if (gravityEnabled == enabled) return;
    
    beginModification();
    gravityEnabled = enabled;
    
    // Update rigidbody if it exists
    if (rigidBody) {
        // TODO: Update gravity flag on actual physics body
    }
    
    endModification();
}

bool PhysicsComponent::isGravityEnabled() const {
    return gravityEnabled;
}

void PhysicsComponent::setCollisionEnabled(bool enabled) {
    if (collisionEnabled == enabled) return;
    
    beginModification();
    collisionEnabled = enabled;
    
    // Update rigidbody if it exists
    if (rigidBody) {
        // TODO: Update collision flag on actual physics body
    }
    
    endModification();
}

bool PhysicsComponent::isCollisionEnabled() const {
    return collisionEnabled;
}

void PhysicsComponent::initialize() {
    // Register with the physics world
    if (auto scene = getScene()) {
        scene->onPhysicsComponentAdded(this);
    }
    
    // Create the rigid body
    updateRigidBody();
}

void PhysicsComponent::update(float deltaTime) {
    // Synchronize transform from physics if dynamic/kinematic
    if (bodyType != BodyType::STATIC && rigidBody) {
        syncPhysicsToTransform();
    }
}

void PhysicsComponent::destroy() {
    // Unregister from the physics world
    if (auto scene = getScene()) {
            scene->onPhysicsComponentRemoved(this);
    }
    
    // Clean up resources
    rigidBody.reset();
    collisionShape.reset();
}

const char* PhysicsComponent::getTypeName() const {
    return "PhysicsComponent";
}

void PhysicsComponent::updateRigidBody() {
    // Clean up old rigid body
    rigidBody.reset();
    
    // Create new rigid body if we have a shape
    if (collisionShape) {
        // TODO: Create actual physics rigid body
        rigidBody = std::make_unique<RigidBody>(); // Placeholder
    }
    
    // Update properties
    if (rigidBody) {
        // TODO: Apply properties to actual physics body
    }
}

void PhysicsComponent::syncTransformToPhysics() {
    // Update physics body transform from actor transform
    if (!rigidBody) return;
    
    auto actor = getOwner();
    if (!actor) return;
    
    auto transform = actor->getTransform();
    if (!transform) return;
    
    // TODO: Update the physics body position/rotation
}

void PhysicsComponent::syncPhysicsToTransform() {
    // Update actor transform from physics body
    if (!rigidBody) return;
    
    auto actor = getOwner();
    if (!actor) return;
    
    auto transform = actor->getTransform();
    if (!transform) return;
    
    // TODO: Get position/rotation from physics body and update transform
}

nlohmann::json PhysicsComponent::serialize() const {
    nlohmann::json data;
    
    // Save physics properties
    data["bodyType"] = static_cast<int>(bodyType);
    data["mass"] = mass;
    data["friction"] = friction;
    data["restitution"] = restitution;
    data["linearDamping"] = linearDamping;
    data["angularDamping"] = angularDamping;
    data["gravityEnabled"] = gravityEnabled;
    data["collisionEnabled"] = collisionEnabled;
    
    // Save collision shape type
    if (collisionShape) {
        // TODO: Properly serialize collision shape
        data["collisionShape"] = {}; // Placeholder
    }
    
    return data;
}

void PhysicsComponent::deserialize(const nlohmann::json& data) {
    beginModification();
    
    if (data.contains("bodyType")) {
        bodyType = static_cast<BodyType>(data["bodyType"].get<int>());
    }
    
    if (data.contains("mass")) {
        mass = data["mass"];
    }
    
    if (data.contains("friction")) {
        friction = data["friction"];
    }
    
    if (data.contains("restitution")) {
        restitution = data["restitution"];
    }
    
    if (data.contains("linearDamping")) {
        linearDamping = data["linearDamping"];
    }
    
    if (data.contains("angularDamping")) {
        angularDamping = data["angularDamping"];
    }
    
    if (data.contains("gravityEnabled")) {
        gravityEnabled = data["gravityEnabled"];
        }
    
    if (data.contains("collisionEnabled")) {
        collisionEnabled = data["collisionEnabled"];
    }
    
    // TODO: Deserialize collision shape
    
    // Recreate the rigid body with the new properties
    updateRigidBody();
    
    endModification();
}

} // namespace ohao 