#include "physics_component.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "engine/actor/actor.hpp"
#include "physics/world/physics_world.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/collision/shapes/collision_shape.hpp"
#include "physics/collision/shapes/shape_factory.hpp"
#include "physics/material/physics_material.hpp"
#include "engine/asset/model.hpp"
#include "ui/components/console_widget.hpp"
#include <map>

namespace ohao {

PhysicsComponent::PhysicsComponent() {
    // Initialize with defaults
}

PhysicsComponent::~PhysicsComponent() {
    destroy();
}

// === RIGID BODY TYPE ===
void PhysicsComponent::setRigidBodyType(physics::dynamics::RigidBodyType type) {
    if (m_rigidBody) {
        m_rigidBody->setType(type);
    }
}

physics::dynamics::RigidBodyType PhysicsComponent::getRigidBodyType() const {
    if (m_rigidBody) {
        return m_rigidBody->getType();
    }
    return physics::dynamics::RigidBodyType::DYNAMIC;
}

// === PHYSICS PROPERTIES ===
void PhysicsComponent::setMass(float mass) {
    if (m_rigidBody) {
        m_rigidBody->setMass(mass);
    }
}

float PhysicsComponent::getMass() const {
    if (m_rigidBody) {
        return m_rigidBody->getMass();
    }
    return 1.0f;
}

void PhysicsComponent::setRestitution(float restitution) {
    // Create or modify material with new restitution
    if (m_rigidBody) {
        auto material = m_rigidBody->getPhysicsMaterial();
        if (!material) {
            material = physics::MaterialLibrary::getInstance().getDefault();
        }
        // Create a copy with modified restitution
        auto newMaterial = std::make_shared<physics::PhysicsMaterial>(*material);
        newMaterial->setRestitution(restitution);
        m_rigidBody->setPhysicsMaterial(newMaterial);
    }
}

float PhysicsComponent::getRestitution() const {
    if (m_rigidBody) {
        return m_rigidBody->getRestitution();
    }
    return 0.0f;
}

void PhysicsComponent::setFriction(float friction) {
    // Create or modify material with new friction
    if (m_rigidBody) {
        auto material = m_rigidBody->getPhysicsMaterial();
        if (!material) {
            material = physics::MaterialLibrary::getInstance().getDefault();
        }
        // Create a copy with modified friction
        auto newMaterial = std::make_shared<physics::PhysicsMaterial>(*material);
        newMaterial->setStaticFriction(friction);
        newMaterial->setDynamicFriction(friction);
        m_rigidBody->setPhysicsMaterial(newMaterial);
    }
}

float PhysicsComponent::getFriction() const {
    if (m_rigidBody) {
        return m_rigidBody->getStaticFriction();
    }
    return 0.5f;
}

void PhysicsComponent::setLinearDamping(float damping) {
    if (m_rigidBody) {
        m_rigidBody->setLinearDamping(damping);
    }
}

float PhysicsComponent::getLinearDamping() const {
    if (m_rigidBody) {
        return m_rigidBody->getLinearDamping();
    }
    return 0.01f;
}

void PhysicsComponent::setAngularDamping(float damping) {
    if (m_rigidBody) {
        m_rigidBody->setAngularDamping(damping);
    }
}

float PhysicsComponent::getAngularDamping() const {
    if (m_rigidBody) {
        return m_rigidBody->getAngularDamping();
    }
    return 0.05f;
}

// === MOVEMENT ===
void PhysicsComponent::setLinearVelocity(const glm::vec3& velocity) {
    if (m_rigidBody) {
        m_rigidBody->setLinearVelocity(velocity);
    }
}

glm::vec3 PhysicsComponent::getLinearVelocity() const {
    if (m_rigidBody) {
        return m_rigidBody->getLinearVelocity();
    }
    return glm::vec3(0.0f);
}

void PhysicsComponent::setAngularVelocity(const glm::vec3& velocity) {
    if (m_rigidBody) {
        m_rigidBody->setAngularVelocity(velocity);
    }
}

glm::vec3 PhysicsComponent::getAngularVelocity() const {
    if (m_rigidBody) {
        return m_rigidBody->getAngularVelocity();
    }
    return glm::vec3(0.0f);
}

// === FORCES ===
void PhysicsComponent::applyForce(const glm::vec3& force, const glm::vec3& relativePos) {
    if (m_rigidBody) {
        m_rigidBody->applyForce(force, relativePos);
    }
}

void PhysicsComponent::applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos) {
    if (m_rigidBody) {
        m_rigidBody->applyImpulse(impulse, relativePos);
    }
}

void PhysicsComponent::applyTorque(const glm::vec3& torque) {
    if (m_rigidBody) {
        m_rigidBody->applyTorque(torque);
    }
}

void PhysicsComponent::clearForces() {
    if (m_rigidBody) {
        m_rigidBody->clearForces();
    }
}

// === COLLISION SHAPES ===
void PhysicsComponent::setCollisionShape(std::shared_ptr<physics::collision::CollisionShape> shape) {
    m_collisionShape = shape;
    if (m_rigidBody) {
        m_rigidBody->setCollisionShape(shape);
    }
}

std::shared_ptr<physics::collision::CollisionShape> PhysicsComponent::getCollisionShape() const {
    return m_collisionShape;
}

void PhysicsComponent::createBoxShape(const glm::vec3& halfExtents) {
    auto shape = physics::collision::ShapeFactory::createBox(halfExtents);
    setCollisionShape(shape);
}

void PhysicsComponent::createBoxShape(float width, float height, float depth) {
    auto shape = physics::collision::ShapeFactory::createBox(width, height, depth);
    setCollisionShape(shape);
}

void PhysicsComponent::createSphereShape(float radius) {
    auto shape = physics::collision::ShapeFactory::createSphere(radius);
    setCollisionShape(shape);
}

void PhysicsComponent::createCubeShape(float size) {
    auto shape = physics::collision::ShapeFactory::createCube(size);
    setCollisionShape(shape);
}

void PhysicsComponent::createCapsuleShape(float radius, float height) {
    auto shape = physics::collision::ShapeFactory::createCapsule(radius, height);
    setCollisionShape(shape);
}

void PhysicsComponent::createCylinderShape(float radius, float height) {
    auto shape = physics::collision::ShapeFactory::createCylinder(radius, height);
    setCollisionShape(shape);
}

void PhysicsComponent::createPlaneShape(const glm::vec3& normal, float distance) {
    auto shape = physics::collision::ShapeFactory::createPlane(normal, distance);
    setCollisionShape(shape);
}

void PhysicsComponent::createTriangleMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices) {
    auto shape = physics::collision::ShapeFactory::createTriangleMesh(vertices, indices);
    setCollisionShape(shape);
}

void PhysicsComponent::createMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices) {
    // Alias for createTriangleMeshShape for backward compatibility
    createTriangleMeshShape(vertices, indices);
}

void PhysicsComponent::createCollisionShapeFromModel(const Model& model) {
    if (model.vertices.empty() || model.indices.empty()) {
        OHAO_LOG_WARNING("Cannot create collision shape: model has no vertices or indices");
        return;
    }
    
    // Extract positions from vertex data
    std::vector<glm::vec3> positions;
    positions.reserve(model.vertices.size());
    
    for (const auto& vertex : model.vertices) {
        positions.push_back(vertex.position);
    }
    
    // Create triangle mesh collision shape
    createTriangleMeshShape(positions, model.indices);
    
    OHAO_LOG("Created triangle mesh collision shape with " + 
             std::to_string(model.vertices.size()) + " vertices and " + 
             std::to_string(model.indices.size() / 3) + " triangles");
}

// === PHYSICS WORLD INTEGRATION ===
void PhysicsComponent::setPhysicsWorld(physics::PhysicsWorld* world) {
    if (m_physicsWorld != world) {
        if (m_physicsWorld && m_rigidBody) {
            destroyRigidBody();
        }
        
        m_physicsWorld = world;
        
        if (m_physicsWorld && m_initialized) {
            createRigidBody();
        }
    }
}

physics::PhysicsWorld* PhysicsComponent::getPhysicsWorld() const {
    return m_physicsWorld;
}

// === TRANSFORM SYNC ===
void PhysicsComponent::setTransformComponent(TransformComponent* transform) {
    m_transformComponent = transform;
}

TransformComponent* PhysicsComponent::getTransformComponent() const {
    return m_transformComponent;
}

// === COMPONENT INTERFACE ===
void PhysicsComponent::initialize() {
    OHAO_LOG("Initializing PhysicsComponent");
    
    m_initialized = true;
    
    if (m_physicsWorld) {
        createRigidBody();
    }
}

void PhysicsComponent::update(float deltaTime) {
    if (m_rigidBody && m_transformComponent && m_physicsWorld) {
        // Only update transform from physics when physics simulation is running
        // This allows manual editing when physics is paused/stopped
        auto physicsState = m_physicsWorld->getSimulationState();
        if (physicsState == physics::SimulationState::RUNNING) {
            updateTransformFromRigidBody();
        }
    }
}

void PhysicsComponent::destroy() {
    if (m_rigidBody && m_physicsWorld) {
        destroyRigidBody();
    }
    m_initialized = false;
}

// === SERIALIZATION ===
void PhysicsComponent::serialize(class Serializer& serializer) const {
    // TODO: Implement serialization
}

void PhysicsComponent::deserialize(class Deserializer& deserializer) {
    // TODO: Implement deserialization  
}

// === SETTINGS ===
void PhysicsComponent::setAwake(bool awake) {
    if (m_rigidBody) {
        m_rigidBody->setAwake(awake);
    }
}

bool PhysicsComponent::isAwake() const {
    if (m_rigidBody) {
        return m_rigidBody->isAwake();
    }
    return true;
}

// === PRIVATE METHODS ===
void PhysicsComponent::createRigidBody() {
    if (!m_physicsWorld) {
        OHAO_LOG_WARNING("Cannot create rigid body: no physics world");
        return;
    }
    
    if (m_rigidBody) {
        OHAO_LOG_WARNING("Rigid body already exists");
        return;
    }
    
    m_rigidBody = m_physicsWorld->createRigidBody(this);
    
    if (m_rigidBody) {
        if (m_collisionShape) {
            m_rigidBody->setCollisionShape(m_collisionShape);
        }
        
        // Sync physics body position with current transform position
        updateRigidBodyFromTransform();
        
        OHAO_LOG("Created rigid body for PhysicsComponent");
    } else {
        OHAO_LOG_ERROR("Failed to create rigid body");
    }
}

void PhysicsComponent::destroyRigidBody() {
    if (m_rigidBody && m_physicsWorld) {
        m_physicsWorld->removeRigidBody(m_rigidBody);
        m_rigidBody.reset();
        OHAO_LOG("Destroyed rigid body for PhysicsComponent");
    }
}

void PhysicsComponent::updateTransformFromRigidBody() {
    if (!m_rigidBody || !m_transformComponent) {
        return;
    }
    
    // Update visual transform from physics
    glm::vec3 position = m_rigidBody->getPosition();
    glm::quat rotation = m_rigidBody->getRotation();
    
    // DEBUG: Log position changes
    static std::map<void*, glm::vec3> lastPositions;
    auto& lastPos = lastPositions[this];
    if (glm::length(position - lastPos) > 0.001f) { // Only log if moved significantly
        printf("PHYSICS SYNC DEBUG: Body moved from (%.3f,%.3f,%.3f) to (%.3f,%.3f,%.3f)\n",
               lastPos.x, lastPos.y, lastPos.z, position.x, position.y, position.z);
        lastPos = position;
    }
    
    m_transformComponent->setPosition(position);
    m_transformComponent->setRotation(rotation);
}

void PhysicsComponent::updateRigidBodyFromTransform() {
    if (!m_rigidBody || !m_transformComponent) {
        return;
    }
    
    // Update physics body from visual transform
    glm::vec3 position = m_transformComponent->getPosition();
    glm::quat rotation = m_transformComponent->getRotation();
    m_rigidBody->setPosition(position);
    m_rigidBody->setRotation(rotation);
}

} // namespace ohao