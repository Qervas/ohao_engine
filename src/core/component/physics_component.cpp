#include "physics_component.hpp"
#include "transform_component.hpp"
#include "../physics/physics_world.hpp"
#include "../physics/rigid_body.hpp"
#include "../physics/collision_shape.hpp"
#include "../actor/actor.hpp"
#include "../../ui/components/console_widget.hpp"
#include "../../ui/panels/viewport/viewport_toolbar.hpp"

namespace ohao {

PhysicsComponent::PhysicsComponent() 
    : m_mass(1.0f)
    , m_rigidBodyType(RigidBodyType::DYNAMIC)
    , m_friction(0.5f)
    , m_restitution(0.0f)
    , m_linearDamping(0.0f)
    , m_angularDamping(0.0f)
    , m_gravityEnabled(true) {
    // Initialize with default values
}

PhysicsComponent::~PhysicsComponent() {
    destroy();
}

void PhysicsComponent::setMass(float mass) {
    m_mass = mass;
    if (m_rigidBody) {
        m_rigidBody->setMass(mass);
    }
}

float PhysicsComponent::getMass() const {
    if (m_rigidBody) {
        return m_rigidBody->getMass();
    }
    return m_mass;
}

void PhysicsComponent::setRigidBodyType(RigidBodyType type) {
    m_rigidBodyType = type;
    if (m_rigidBody) {
        m_rigidBody->setType(type);
        
        // For static bodies, clear velocities and forces
        if (type == RigidBodyType::STATIC) {
            m_rigidBody->setLinearVelocity(glm::vec3(0.0f));
            m_rigidBody->setAngularVelocity(glm::vec3(0.0f));
            m_rigidBody->clearForces();
        }
        
        OHAO_LOG("Changed rigid body type to: " + std::to_string(static_cast<int>(type)));
    }
}

RigidBodyType PhysicsComponent::getRigidBodyType() const {
    if (m_rigidBody) {
        return m_rigidBody->getType();
    }
    return m_rigidBodyType;
}

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

void PhysicsComponent::setGravityEnabled(bool enabled) {
    m_gravityEnabled = enabled;
    // TODO: Update physics world settings for this body
}

bool PhysicsComponent::isGravityEnabled() const {
    return m_gravityEnabled;
}

void PhysicsComponent::setFriction(float friction) {
    m_friction = friction;
    if (m_rigidBody) {
        m_rigidBody->setFriction(friction);
    }
}

float PhysicsComponent::getFriction() const {
    if (m_rigidBody) {
        return m_rigidBody->getFriction();
    }
    return m_friction;
}

void PhysicsComponent::setRestitution(float restitution) {
    m_restitution = restitution;
    if (m_rigidBody) {
        m_rigidBody->setRestitution(restitution);
    }
}

float PhysicsComponent::getRestitution() const {
    if (m_rigidBody) {
        return m_rigidBody->getRestitution();
    }
    return m_restitution;
}

void PhysicsComponent::setLinearDamping(float damping) {
    m_linearDamping = damping;
    if (m_rigidBody) {
        m_rigidBody->setLinearDamping(damping);
    }
}

float PhysicsComponent::getLinearDamping() const {
    if (m_rigidBody) {
        return m_rigidBody->getLinearDamping();
    }
    return m_linearDamping;
}

void PhysicsComponent::setAngularDamping(float damping) {
    m_angularDamping = damping;
    if (m_rigidBody) {
        m_rigidBody->setAngularDamping(damping);
    }
}

float PhysicsComponent::getAngularDamping() const {
    if (m_rigidBody) {
        return m_rigidBody->getAngularDamping();
    }
    return m_angularDamping;
}

void PhysicsComponent::setCollisionShape(std::shared_ptr<CollisionShape> shape) {
    m_collisionShape = shape;
    if (m_rigidBody) {
        m_rigidBody->setCollisionShape(shape);
    }
}

std::shared_ptr<CollisionShape> PhysicsComponent::getCollisionShape() const {
    return m_collisionShape;
}

void PhysicsComponent::createBoxShape(const glm::vec3& halfExtents) {
    auto shape = std::make_shared<BoxShape>(halfExtents);
    setCollisionShape(shape);
    OHAO_LOG("Created box collision shape with half extents: (" + 
             std::to_string(halfExtents.x) + ", " + 
             std::to_string(halfExtents.y) + ", " + 
             std::to_string(halfExtents.z) + ")");
}

void PhysicsComponent::createSphereShape(float radius) {
    auto shape = std::make_shared<SphereShape>(radius);
    setCollisionShape(shape);
    OHAO_LOG("Created sphere collision shape with radius: " + std::to_string(radius));
}

void PhysicsComponent::createCapsuleShape(float radius, float height) {
    auto shape = std::make_shared<CapsuleShape>(radius, height);
    setCollisionShape(shape);
    OHAO_LOG("Created capsule collision shape with radius: " + std::to_string(radius) + 
             ", height: " + std::to_string(height));
}

void PhysicsComponent::createMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices) {
    // For mesh shape, we'll create a convex hull from the vertices
    auto shape = std::make_shared<ConvexHullShape>(vertices);
    setCollisionShape(shape);
    OHAO_LOG("Created mesh collision shape with " + std::to_string(vertices.size()) + " vertices");
}

void PhysicsComponent::setTransformComponent(TransformComponent* transform) {
    m_transformComponent = transform;
}

TransformComponent* PhysicsComponent::getTransformComponent() const {
    return m_transformComponent;
}

void PhysicsComponent::setPhysicsWorld(PhysicsWorld* world) {
    m_physicsWorld = world;
    
    // Recreate rigid body in new world
    if (m_rigidBody && world) {
        createRigidBody();
    }
}

PhysicsWorld* PhysicsComponent::getPhysicsWorld() const {
    return m_physicsWorld;
}

std::shared_ptr<RigidBody> PhysicsComponent::getRigidBody() const {
    return m_rigidBody;
}

void PhysicsComponent::initialize() {
    OHAO_LOG("=== PhysicsComponent::initialize() called ===");
    
    // Get transform component from actor
    auto actor = getOwner();
    if (actor) {
        m_transformComponent = actor->getComponent<TransformComponent>().get();
        OHAO_LOG("Found transform component: " + std::to_string((long long)m_transformComponent));
    } else {
        OHAO_LOG_WARNING("No actor owner found for physics component");
    }
    
    // Create rigid body if we have all necessary components
    if (m_physicsWorld && m_collisionShape) {
        OHAO_LOG("Creating rigid body - Physics world: " + std::to_string((long long)m_physicsWorld) + 
                 ", Collision shape: " + std::to_string((long long)m_collisionShape.get()));
        createRigidBody();
    } else {
        OHAO_LOG_WARNING("Cannot create rigid body - Physics world: " + std::to_string((long long)m_physicsWorld) + 
                         ", Collision shape: " + std::to_string((long long)m_collisionShape.get()));
    }
}

void PhysicsComponent::update(float deltaTime) {
    // Bidirectional sync between transform and rigid body
    if (m_rigidBody && m_transformComponent) {
        if (m_transformComponent->isDirty()) {
            // Transform was manually changed - check if we should allow this
            bool allowManualChange = false;
            
            // Always allow manual changes for static and kinematic bodies
            if (m_rigidBodyType == RigidBodyType::STATIC || m_rigidBodyType == RigidBodyType::KINEMATIC) {
                allowManualChange = true;
            }
            // For dynamic bodies, only allow manual changes when simulation is not playing
            else if (m_rigidBodyType == RigidBodyType::DYNAMIC) {
                if (m_physicsWorld) {
                    PhysicsSimulationState simState = m_physicsWorld->getSimulationState();
                    allowManualChange = (simState != PhysicsSimulationState::PLAYING);
                } else {
                    // No physics world - allow changes
                    allowManualChange = true;
                }
            }
            
            if (allowManualChange) {
                // Update rigid body from transform
                updateRigidBodyFromTransform();
                
                // Clear the dirty flag so we don't process this change again
                m_transformComponent->clearDirty();
                
                // Clear velocities for static/kinematic bodies when manually moved
                if (m_rigidBodyType == RigidBodyType::STATIC || m_rigidBodyType == RigidBodyType::KINEMATIC) {
                    m_rigidBody->setLinearVelocity(glm::vec3(0.0f));
                    m_rigidBody->setAngularVelocity(glm::vec3(0.0f));
                }
                
                OHAO_LOG_DEBUG("Updated rigid body from manually changed transform");
            } else {
                // Manual change not allowed - revert transform to physics body position
                updateTransformFromRigidBody();
                m_transformComponent->clearDirty();
                OHAO_LOG_DEBUG("Reverted manual transform change - simulation is playing");
            }
        } else {
            // Transform not manually changed - update transform from physics simulation
            // Only for dynamic bodies that might be affected by physics
            if (m_rigidBodyType == RigidBodyType::DYNAMIC && m_physicsWorld) {
                PhysicsSimulationState simState = m_physicsWorld->getSimulationState();
                if (simState == PhysicsSimulationState::PLAYING) {
                    updateTransformFromRigidBody();
                }
            }
        }
    }
}

void PhysicsComponent::destroy() {
    if (m_rigidBody && m_physicsWorld) {
        m_physicsWorld->removeRigidBody(m_rigidBody);
        m_rigidBody.reset();
    }
}

const char* PhysicsComponent::getTypeName() const {
    return "PhysicsComponent";
}

void PhysicsComponent::onCollisionBegin(PhysicsComponent* other) {
    // TODO: Implement collision callbacks
    OHAO_LOG("Collision began with: " + std::string(other->getTypeName()));
}

void PhysicsComponent::onCollisionEnd(PhysicsComponent* other) {
    // TODO: Implement collision callbacks
    OHAO_LOG("Collision ended with: " + std::string(other->getTypeName()));
}

void PhysicsComponent::serialize(class Serializer& serializer) const {
    // TODO: Implement serialization
    // serializer.serialize("mass", getMass());
    // serializer.serialize("friction", getFriction());
    // serializer.serialize("restitution", getRestitution());
    // serializer.serialize("gravityEnabled", m_gravityEnabled);
}

void PhysicsComponent::deserialize(class Deserializer& deserializer) {
    // TODO: Implement deserialization
    // float mass, friction, restitution;
    // bool gravityEnabled;
    // deserializer.deserialize("mass", mass);
    // deserializer.deserialize("friction", friction);
    // deserializer.deserialize("restitution", restitution);
    // deserializer.deserialize("gravityEnabled", gravityEnabled);
    // 
    // setMass(mass);
    // setFriction(friction);
    // setRestitution(restitution);
    // setGravityEnabled(gravityEnabled);
}

void PhysicsComponent::createRigidBody() {
    if (!m_physicsWorld) {
        OHAO_LOG_WARNING("Cannot create rigid body: no physics world");
        return;
    }
    
    // Remove existing rigid body
    if (m_rigidBody) {
        m_physicsWorld->removeRigidBody(m_rigidBody);
    }
    
    // Create new rigid body
    m_rigidBody = m_physicsWorld->createRigidBody(this);
    
    if (m_rigidBody) {
        // Set collision shape
        if (m_collisionShape) {
            m_rigidBody->setCollisionShape(m_collisionShape);
        }
        
        // Apply all cached properties to the new rigid body
        m_rigidBody->setType(m_rigidBodyType);
        m_rigidBody->setMass(m_mass);
        m_rigidBody->setFriction(m_friction);
        m_rigidBody->setRestitution(m_restitution);
        m_rigidBody->setLinearDamping(m_linearDamping);
        m_rigidBody->setAngularDamping(m_angularDamping);
        
        // Sync transform from component
        updateRigidBodyFromTransform();
        
        OHAO_LOG("Created rigid body for PhysicsComponent with type: " + std::to_string(static_cast<int>(m_rigidBodyType)));
    } else {
        OHAO_LOG_ERROR("Failed to create rigid body");
    }
}

void PhysicsComponent::updateRigidBodyFromTransform() {
    if (!m_rigidBody || !m_transformComponent) {
        return;
    }
    
    // Get transform data from the transform component
    glm::vec3 position = m_transformComponent->getPosition();
    glm::quat rotation = m_transformComponent->getRotation();
    
    // Update rigid body transform
    m_rigidBody->setPosition(position);
    m_rigidBody->setRotation(rotation);
    
    OHAO_LOG_DEBUG("Updated rigid body from transform: pos(" + 
             std::to_string(position.x) + ", " + 
             std::to_string(position.y) + ", " + 
             std::to_string(position.z) + ")");
}

void PhysicsComponent::updateTransformFromRigidBody() {
    if (!m_rigidBody || !m_transformComponent) {
        return;
    }
    
    // Get transform data from the rigid body
    glm::vec3 position = m_rigidBody->getPosition();
    glm::quat rotation = m_rigidBody->getRotation();
    
    // Update transform component (this moves the visual object)
    m_transformComponent->setPosition(position);
    m_transformComponent->setRotation(rotation);
}

} // namespace ohao