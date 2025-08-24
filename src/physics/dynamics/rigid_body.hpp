#pragma once

#include "integrator.hpp"
#include "physics/collision/shapes/collision_shape.hpp"
#include "physics/material/physics_material.hpp"
#include "physics/common/physics_constants.hpp"
#include "physics/utils/physics_math.hpp"
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
    void setType(RigidBodyType type);
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
    
    // === MASS PROPERTIES ===
    void setMass(float mass);
    float getMass() const { return m_mass; }
    float getInverseMass() const { return m_invMass; }
    
    // Inertia tensor (in local space)
    void setInertiaTensor(const glm::mat3& tensor);
    glm::mat3 getInertiaTensor() const { return m_inertiaTensor; }
    glm::mat3 getInverseInertiaTensor() const { return m_invInertiaTensor; }
    
    // World space inertia tensor (rotated by current orientation)
    glm::mat3 getWorldInverseInertiaTensor() const;
    
    // Automatic inertia calculation from collision shape
    void calculateInertiaFromShape();
    
    // === MATERIAL PROPERTIES ===
    void setPhysicsMaterial(std::shared_ptr<PhysicsMaterial> material) { m_material = material; }
    std::shared_ptr<PhysicsMaterial> getPhysicsMaterial() const { return m_material; }
    
    // Convenient material property accessors (uses material if available)
    float getRestitution() const;
    float getStaticFriction() const;
    float getDynamicFriction() const;
    float getDensity() const;
    
    // === VELOCITY ===
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const { return m_linearVelocity; }
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const { return m_angularVelocity; }
    
    // === FORCES ===
    void applyForce(const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyTorque(const glm::vec3& torque);
    void clearForces();
    
    glm::vec3 getAccumulatedForce() const { return m_accumulatedForce; }
    glm::vec3 getAccumulatedTorque() const { return m_accumulatedTorque; }
    
    // Advanced force application
    void applyForceAtWorldPoint(const glm::vec3& force, const glm::vec3& worldPoint);
    void applyImpulseAtWorldPoint(const glm::vec3& impulse, const glm::vec3& worldPoint);
    
    // === COLLISION SHAPE ===
    void setCollisionShape(std::shared_ptr<collision::CollisionShape> shape);
    std::shared_ptr<collision::CollisionShape> getCollisionShape() const { return m_collisionShape; }
    
    math::AABB getAABB() const;
    
    // === INTEGRATION ===
    void integrate(float deltaTime);
    
    // === COMPONENT SYNC ===
    PhysicsComponent* getComponent() const { return m_component; }
    void updateTransformComponent();
    
    // === SLEEP/WAKE SYSTEM ===
    void setAwake(bool awake);
    bool isAwake() const { return m_isAwake; }
    
    void updateSleepState(float deltaTime);
    
    // === ENERGY & MOMENTUM ===
    float getKineticEnergy() const;
    glm::vec3 getLinearMomentum() const { return m_mass * m_linearVelocity; }
    glm::vec3 getAngularMomentum() const;
    
    // === DAMPING ===
    void setLinearDamping(float damping) { m_linearDamping = math::clamp(damping, 0.0f, 1.0f); }
    float getLinearDamping() const { return m_linearDamping; }
    
    void setAngularDamping(float damping) { m_angularDamping = math::clamp(damping, 0.0f, 1.0f); }
    float getAngularDamping() const { return m_angularDamping; }
    
    // === CONSTRAINTS ===
    void setGravityEnabled(bool enabled) { m_gravityEnabled = enabled; }
    bool isGravityEnabled() const { return m_gravityEnabled; }
    
    void setKinematicTarget(const glm::vec3& position, const glm::quat& rotation) {
        m_kinematicTargetPos = position;
        m_kinematicTargetRot = rotation;
    }
    
private:
    // Component reference
    PhysicsComponent* m_component;
    
    // Physics type
    RigidBodyType m_type{RigidBodyType::DYNAMIC};
    
    // Mass properties
    float m_mass{1.0f};
    float m_invMass{1.0f};
    glm::mat3 m_inertiaTensor{1.0f};        // Local space inertia tensor
    glm::mat3 m_invInertiaTensor{1.0f};     // Inverse of local inertia tensor
    
    // Material
    std::shared_ptr<PhysicsMaterial> m_material;
    
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
    float m_motionThreshold{constants::SLEEP_LINEAR_THRESHOLD};
    
    // Damping
    float m_linearDamping{0.01f};
    float m_angularDamping{0.05f};
    
    // Constraints
    bool m_gravityEnabled{true};
    
    // Kinematic targets (for kinematic bodies)
    glm::vec3 m_kinematicTargetPos{0.0f};
    glm::quat m_kinematicTargetRot{1, 0, 0, 0};
    
    // Helper methods
    void updateMassProperties();
    void validateState();
};

} // namespace dynamics
} // namespace physics
} // namespace ohao