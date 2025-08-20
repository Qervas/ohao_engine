#pragma once

#include "integrator.hpp"
#include "../collision/shapes/collision_shape.hpp"
#include "../utils/physics_math.hpp"
#include <memory>

namespace ohao {

// Forward declarations
class PhysicsComponent;

namespace physics {
namespace dynamics {

class RigidBody {
public:
    RigidBody(PhysicsComponent* component);
    ~RigidBody();
    
    // === TYPE & STATE ===
    void setType(RigidBodyType type) { 
        m_type = type; 
        // Update mass calculations when type changes
        setMass(m_mass);
    }
    RigidBodyType getType() const { return m_type; }
    
    bool isStatic() const { return m_type == RigidBodyType::STATIC; }
    bool isDynamic() const { return m_type == RigidBodyType::DYNAMIC; }
    bool isKinematic() const { return m_type == RigidBodyType::KINEMATIC; }
    
    // === TRANSFORM ===
    void setPosition(const glm::vec3& position) { m_position = position; }
    glm::vec3 getPosition() const { return m_position; }
    
    void setRotation(const glm::quat& rotation) { m_rotation = math::safeNormalize(rotation); }
    glm::quat getRotation() const { return m_rotation; }
    
    void setTransform(const glm::vec3& position, const glm::quat& rotation) {
        setPosition(position);
        setRotation(rotation);
    }
    
    glm::mat4 getTransformMatrix() const {
        return math::createTransformMatrix(m_position, m_rotation);
    }
    
    // === PHYSICS PROPERTIES ===
    void setMass(float mass) { 
        if (isStatic()) {
            m_mass = 0.0f;
            m_invMass = 0.0f;
        } else {
            m_mass = glm::max(mass, math::constants::EPSILON);
            m_invMass = (m_mass > 0.0f) ? 1.0f / m_mass : 0.0f;
        }
    }
    float getMass() const { return m_mass; }
    float getInverseMass() const { return m_invMass; }
    
    void setRestitution(float restitution) { m_restitution = math::clamp(restitution, 0.0f, 1.0f); }
    float getRestitution() const { return m_restitution; }
    
    void setFriction(float friction) { m_friction = glm::max(friction, 0.0f); }
    float getFriction() const { return m_friction; }
    
    void setLinearDamping(float damping) { m_linearDamping = math::clamp(damping, 0.0f, 1.0f); }
    float getLinearDamping() const { return m_linearDamping; }
    
    void setAngularDamping(float damping) { m_angularDamping = math::clamp(damping, 0.0f, 1.0f); }
    float getAngularDamping() const { return m_angularDamping; }
    
    // === VELOCITY ===
    void setLinearVelocity(const glm::vec3& velocity) { 
        if (!isStatic()) m_linearVelocity = velocity; 
    }
    glm::vec3 getLinearVelocity() const { return m_linearVelocity; }
    
    void setAngularVelocity(const glm::vec3& velocity) { 
        if (!isStatic()) m_angularVelocity = velocity; 
    }
    glm::vec3 getAngularVelocity() const { return m_angularVelocity; }
    
    // === FORCES ===
    void applyForce(const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f)) {
        if (isStatic()) return;
        m_accumulatedForce += force;
        if (!math::isNearZero(relativePos)) {
            m_accumulatedTorque += glm::cross(relativePos, force);
        }
    }
    
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f)) {
        if (isStatic()) return;
        m_linearVelocity += impulse * m_invMass;
        if (!math::isNearZero(relativePos)) {
            m_angularVelocity += glm::cross(relativePos, impulse) * m_invMass; // Simplified
        }
    }
    
    void applyTorque(const glm::vec3& torque) {
        if (!isStatic()) m_accumulatedTorque += torque;
    }
    
    void clearForces() {
        m_accumulatedForce = glm::vec3(0.0f);
        m_accumulatedTorque = glm::vec3(0.0f);
    }
    
    glm::vec3 getAccumulatedForce() const { return m_accumulatedForce; }
    glm::vec3 getAccumulatedTorque() const { return m_accumulatedTorque; }
    
    // === COLLISION SHAPE ===
    void setCollisionShape(std::shared_ptr<collision::CollisionShape> shape) { m_collisionShape = shape; }
    std::shared_ptr<collision::CollisionShape> getCollisionShape() const { return m_collisionShape; }
    
    math::AABB getAABB() const {
        if (m_collisionShape) {
            return m_collisionShape->getAABB(m_position, m_rotation);
        }
        return math::AABB(m_position, glm::vec3(0.5f)); // Default 1x1x1 box
    }
    
    // === INTEGRATION ===
    void integrate(float deltaTime) {
        if (isStatic()) return;
        Integrator::integratePhysics(this, deltaTime);
    }
    
    // === COMPONENT SYNC ===
    PhysicsComponent* getComponent() const { return m_component; }
    void updateTransformComponent();
    
    // === SLEEP/WAKE SYSTEM ===
    void setAwake(bool awake) { m_isAwake = awake; }
    bool isAwake() const { return m_isAwake; }
    
    void updateSleepState(float deltaTime, float sleepThreshold = 0.1f, float sleepTimeout = 2.0f) {
        if (isStatic()) return;
        
        float kineticEnergy = 0.5f * m_mass * math::lengthSquared(m_linearVelocity) +
                             0.5f * math::lengthSquared(m_angularVelocity); // Simplified inertia
                             
        if (kineticEnergy < sleepThreshold) {
            m_sleepTimer += deltaTime;
            if (m_sleepTimer > sleepTimeout) {
                setAwake(false);
                m_linearVelocity = glm::vec3(0.0f);
                m_angularVelocity = glm::vec3(0.0f);
            }
        } else {
            m_sleepTimer = 0.0f;
            setAwake(true);
        }
    }
    
private:
    // Component reference
    PhysicsComponent* m_component;
    
    // Physics properties  
    RigidBodyType m_type{RigidBodyType::DYNAMIC};
    float m_mass{1.0f};
    float m_invMass{1.0f};
    float m_restitution{0.0f};
    float m_friction{0.5f};
    float m_linearDamping{0.01f};
    float m_angularDamping{0.05f};
    
    // Transform
    glm::vec3 m_position{0.0f};
    glm::quat m_rotation{1, 0, 0, 0};
    
    // Velocities
    glm::vec3 m_linearVelocity{0.0f};
    glm::vec3 m_angularVelocity{0.0f};
    
    // Forces
    glm::vec3 m_accumulatedForce{0.0f};
    glm::vec3 m_accumulatedTorque{0.0f};
    
    // Collision
    std::shared_ptr<collision::CollisionShape> m_collisionShape;
    
    // Sleep system
    bool m_isAwake{true};
    float m_sleepTimer{0.0f};
};

} // namespace dynamics
} // namespace physics
} // namespace ohao