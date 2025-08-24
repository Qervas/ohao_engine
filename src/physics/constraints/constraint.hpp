#pragma once

#include "physics/utils/physics_math.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <vector>
#include <memory>

namespace ohao {
namespace physics {
namespace constraints {

// Forward declarations
class Constraint;
class ConstraintSolver;

// Jacobian matrix entry for a constraint
struct JacobianEntry {
    glm::vec3 linearA{0.0f};     // Linear part for body A
    glm::vec3 angularA{0.0f};    // Angular part for body A
    glm::vec3 linearB{0.0f};     // Linear part for body B
    glm::vec3 angularB{0.0f};    // Angular part for body B
    
    JacobianEntry() = default;
    JacobianEntry(const glm::vec3& linA, const glm::vec3& angA,
                  const glm::vec3& linB, const glm::vec3& angB)
        : linearA(linA), angularA(angA), linearB(linB), angularB(angB) {}
    
    // Compute J * v (Jacobian times velocity vector)
    float computeJV(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB) const {
        float result = 0.0f;
        
        if (bodyA) {
            result += glm::dot(linearA, bodyA->getLinearVelocity());
            result += glm::dot(angularA, bodyA->getAngularVelocity());
        }
        
        if (bodyB) {
            result += glm::dot(linearB, bodyB->getLinearVelocity());
            result += glm::dot(angularB, bodyB->getAngularVelocity());
        }
        
        return result;
    }
    
    // Apply impulse using this Jacobian entry
    void applyImpulse(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, float lambda) const {
        if (bodyA && !bodyA->isStatic()) {
            bodyA->setLinearVelocity(bodyA->getLinearVelocity() + linearA * lambda * bodyA->getInverseMass());
            
            glm::mat3 invInertiaA = bodyA->getWorldInverseInertiaTensor();
            bodyA->setAngularVelocity(bodyA->getAngularVelocity() + invInertiaA * angularA * lambda);
        }
        
        if (bodyB && !bodyB->isStatic()) {
            bodyB->setLinearVelocity(bodyB->getLinearVelocity() + linearB * lambda * bodyB->getInverseMass());
            
            glm::mat3 invInertiaB = bodyB->getWorldInverseInertiaTensor();
            bodyB->setAngularVelocity(bodyB->getAngularVelocity() + invInertiaB * angularB * lambda);
        }
    }
};

// Base class for all constraints
class Constraint {
public:
    enum class Type {
        CONTACT,
        DISTANCE,
        HINGE,
        BALL_SOCKET,
        SLIDER,
        FIXED,
        SPRING,
        MOTOR
    };
    
    Constraint(Type type, dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB = nullptr)
        : m_type(type), m_bodyA(bodyA), m_bodyB(bodyB) {}
    
    virtual ~Constraint() = default;
    
    // Constraint interface
    virtual void updateJacobians(float deltaTime) = 0;
    virtual void warmStart() = 0;
    virtual void solveVelocityConstraints(float deltaTime) = 0;
    virtual void solvePositionConstraints(float deltaTime) = 0;
    virtual bool isActive() const { return m_enabled && m_bodyA; }
    
    // Accessors
    Type getType() const { return m_type; }
    dynamics::RigidBody* getBodyA() const { return m_bodyA; }
    dynamics::RigidBody* getBodyB() const { return m_bodyB; }
    
    // Configuration
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    
    void setBreakingThreshold(float threshold) { m_breakingThreshold = threshold; }
    float getBreakingThreshold() const { return m_breakingThreshold; }
    
    // Impulse tracking
    float getAppliedImpulse() const { return m_appliedImpulse; }
    
protected:
    Type m_type;
    dynamics::RigidBody* m_bodyA{nullptr};
    dynamics::RigidBody* m_bodyB{nullptr};
    
    bool m_enabled{true};
    float m_breakingThreshold{std::numeric_limits<float>::max()};
    float m_appliedImpulse{0.0f};
    
    // Solver data
    std::vector<JacobianEntry> m_jacobians;
    std::vector<float> m_lambdas;           // Accumulated impulses
    std::vector<float> m_lowerLimits;       // Lower bounds for impulses
    std::vector<float> m_upperLimits;       // Upper bounds for impulses
    std::vector<float> m_bias;              // Bias terms (for position correction)
    std::vector<float> m_effectiveMass;     // Effective mass matrix diagonal
    
    // Helper functions
    void allocateConstraintData(size_t numRows);
    float computeEffectiveMass(const JacobianEntry& jacobian) const;
    void clampAccumulatedImpulse(size_t index, float deltaLambda);
};

// Distance constraint - maintains fixed distance between two points
class DistanceConstraint : public Constraint {
public:
    DistanceConstraint(dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
                      dynamics::RigidBody* bodyB, const glm::vec3& anchorB,
                      float distance = -1.0f); // -1 means use current distance
    
    void updateJacobians(float deltaTime) override;
    void warmStart() override;
    void solveVelocityConstraints(float deltaTime) override;
    void solvePositionConstraints(float deltaTime) override;
    
    // Configuration
    void setDistance(float distance) { m_targetDistance = distance; }
    float getDistance() const { return m_targetDistance; }
    
    void setAnchorA(const glm::vec3& anchor) { m_localAnchorA = anchor; }
    void setAnchorB(const glm::vec3& anchor) { m_localAnchorB = anchor; }
    
    glm::vec3 getAnchorA() const { return m_localAnchorA; }
    glm::vec3 getAnchorB() const { return m_localAnchorB; }

private:
    glm::vec3 m_localAnchorA{0.0f};
    glm::vec3 m_localAnchorB{0.0f};
    float m_targetDistance{0.0f};
    float m_frequency{0.0f};        // Frequency for soft constraint (0 = hard)
    float m_dampingRatio{0.7f};     // Damping ratio for soft constraint
};

// Ball-socket constraint - connects two points with no rotation constraint
class BallSocketConstraint : public Constraint {
public:
    BallSocketConstraint(dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
                        dynamics::RigidBody* bodyB, const glm::vec3& anchorB);
    
    void updateJacobians(float deltaTime) override;
    void warmStart() override;
    void solveVelocityConstraints(float deltaTime) override;
    void solvePositionConstraints(float deltaTime) override;
    
    // Configuration
    void setAnchorA(const glm::vec3& anchor) { m_localAnchorA = anchor; }
    void setAnchorB(const glm::vec3& anchor) { m_localAnchorB = anchor; }
    
    glm::vec3 getAnchorA() const { return m_localAnchorA; }
    glm::vec3 getAnchorB() const { return m_localAnchorB; }

private:
    glm::vec3 m_localAnchorA{0.0f};
    glm::vec3 m_localAnchorB{0.0f};
};

// Hinge constraint - allows rotation around one axis
class HingeConstraint : public Constraint {
public:
    HingeConstraint(dynamics::RigidBody* bodyA, const glm::vec3& anchorA, const glm::vec3& axisA,
                   dynamics::RigidBody* bodyB, const glm::vec3& anchorB, const glm::vec3& axisB);
    
    void updateJacobians(float deltaTime) override;
    void warmStart() override;
    void solveVelocityConstraints(float deltaTime) override;
    void solvePositionConstraints(float deltaTime) override;
    
    // Limit configuration
    void setAngleLimits(float lowerLimit, float upperLimit);
    void enableAngleLimits(bool enable) { m_hasAngleLimits = enable; }
    bool hasAngleLimits() const { return m_hasAngleLimits; }
    
    float getCurrentAngle() const;
    float getAngularVelocity() const;
    
    // Motor configuration
    void enableMotor(bool enable) { m_hasMotor = enable; }
    void setMotorSpeed(float speed) { m_motorSpeed = speed; }
    void setMaxMotorTorque(float torque) { m_maxMotorTorque = torque; }

private:
    glm::vec3 m_localAnchorA{0.0f};
    glm::vec3 m_localAnchorB{0.0f};
    glm::vec3 m_localAxisA{0.0f, 0.0f, 1.0f};
    glm::vec3 m_localAxisB{0.0f, 0.0f, 1.0f};
    
    // Angle limits
    bool m_hasAngleLimits{false};
    float m_lowerAngleLimit{0.0f};
    float m_upperAngleLimit{0.0f};
    float m_referenceAngle{0.0f};
    
    // Motor
    bool m_hasMotor{false};
    float m_motorSpeed{0.0f};
    float m_maxMotorTorque{0.0f};
    
    // Cached data
    glm::vec3 m_worldAxisA{0.0f};
    glm::vec3 m_worldAxisB{0.0f};
};

// Spring constraint - applies spring forces between two bodies
class SpringConstraint : public Constraint {
public:
    SpringConstraint(dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
                    dynamics::RigidBody* bodyB, const glm::vec3& anchorB,
                    float restLength, float stiffness, float damping);
    
    void updateJacobians(float deltaTime) override;
    void warmStart() override;
    void solveVelocityConstraints(float deltaTime) override;
    void solvePositionConstraints(float deltaTime) override;
    
    // Configuration
    void setRestLength(float length) { m_restLength = length; }
    void setStiffness(float stiffness) { m_stiffness = stiffness; }
    void setDamping(float damping) { m_damping = damping; }
    
    float getRestLength() const { return m_restLength; }
    float getStiffness() const { return m_stiffness; }
    float getDamping() const { return m_damping; }
    float getCurrentLength() const;

private:
    glm::vec3 m_localAnchorA{0.0f};
    glm::vec3 m_localAnchorB{0.0f};
    float m_restLength{1.0f};
    float m_stiffness{1000.0f};
    float m_damping{50.0f};
};

// Contact constraint - represents collision contact with friction
class ContactConstraint : public Constraint {
public:
    struct ContactData {
        glm::vec3 localPointA{0.0f};
        glm::vec3 localPointB{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float penetration{0.0f};
        float restitution{0.3f};
        float friction{0.6f};
        glm::vec3 tangent1{0.0f};
        glm::vec3 tangent2{0.0f};
    };
    
    ContactConstraint(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                     const ContactData& contactData);
    
    void updateJacobians(float deltaTime) override;
    void warmStart() override;
    void solveVelocityConstraints(float deltaTime) override;
    void solvePositionConstraints(float deltaTime) override;
    
    // Update contact data
    void updateContactData(const ContactData& data) { m_contactData = data; }
    const ContactData& getContactData() const { return m_contactData; }

private:
    ContactData m_contactData;
    
    // Solver indices for normal and friction constraints
    size_t m_normalIndex{0};
    size_t m_friction1Index{1};
    size_t m_friction2Index{2};
};

} // namespace constraints
} // namespace physics
} // namespace ohao