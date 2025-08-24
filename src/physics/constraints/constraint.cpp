#include "constraint.hpp"
#include "physics/utils/physics_math.hpp"
#include <algorithm>

namespace ohao {
namespace physics {
namespace constraints {

// === BASE CONSTRAINT IMPLEMENTATION ===

void Constraint::allocateConstraintData(size_t numRows) {
    m_jacobians.resize(numRows);
    m_lambdas.resize(numRows, 0.0f);
    m_lowerLimits.resize(numRows, -std::numeric_limits<float>::max());
    m_upperLimits.resize(numRows, std::numeric_limits<float>::max());
    m_bias.resize(numRows, 0.0f);
    m_effectiveMass.resize(numRows, 0.0f);
}

float Constraint::computeEffectiveMass(const JacobianEntry& jacobian) const {
    float effectiveMass = 0.0f;
    
    if (m_bodyA && !m_bodyA->isStatic()) {
        // Linear part: m_A^-1
        effectiveMass += m_bodyA->getInverseMass() * glm::dot(jacobian.linearA, jacobian.linearA);
        
        // Angular part: I_A^-1
        glm::mat3 invInertiaA = m_bodyA->getWorldInverseInertiaTensor();
        glm::vec3 angularContrib = invInertiaA * jacobian.angularA;
        effectiveMass += glm::dot(jacobian.angularA, angularContrib);
    }
    
    if (m_bodyB && !m_bodyB->isStatic()) {
        // Linear part: m_B^-1
        effectiveMass += m_bodyB->getInverseMass() * glm::dot(jacobian.linearB, jacobian.linearB);
        
        // Angular part: I_B^-1
        glm::mat3 invInertiaB = m_bodyB->getWorldInverseInertiaTensor();
        glm::vec3 angularContrib = invInertiaB * jacobian.angularB;
        effectiveMass += glm::dot(jacobian.angularB, angularContrib);
    }
    
    return effectiveMass > math::constants::EPSILON ? 1.0f / effectiveMass : 0.0f;
}

void Constraint::clampAccumulatedImpulse(size_t index, float deltaLambda) {
    float oldLambda = m_lambdas[index];
    m_lambdas[index] = math::clamp(oldLambda + deltaLambda, m_lowerLimits[index], m_upperLimits[index]);
    float actualDelta = m_lambdas[index] - oldLambda;
    
    // Apply the clamped impulse
    m_jacobians[index].applyImpulse(m_bodyA, m_bodyB, actualDelta);
    m_appliedImpulse += glm::abs(actualDelta);
}

// === DISTANCE CONSTRAINT IMPLEMENTATION ===

DistanceConstraint::DistanceConstraint(dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
                                      dynamics::RigidBody* bodyB, const glm::vec3& anchorB,
                                      float distance)
    : Constraint(Type::DISTANCE, bodyA, bodyB)
    , m_localAnchorA(anchorA)
    , m_localAnchorB(anchorB)
{
    // If distance is negative, calculate current distance
    if (distance < 0.0f) {
        glm::vec3 worldAnchorA = bodyA->getPosition() + bodyA->getRotation() * anchorA;
        glm::vec3 worldAnchorB = bodyB ? bodyB->getPosition() + bodyB->getRotation() * anchorB : anchorB;
        m_targetDistance = glm::length(worldAnchorB - worldAnchorA);
    } else {
        m_targetDistance = distance;
    }
    
    allocateConstraintData(1); // One constraint equation
}

void DistanceConstraint::updateJacobians(float deltaTime) {
    if (!m_bodyA) return;
    
    // Calculate world positions
    glm::vec3 worldAnchorA = m_bodyA->getPosition() + m_bodyA->getRotation() * m_localAnchorA;
    glm::vec3 worldAnchorB = m_bodyB ? 
        m_bodyB->getPosition() + m_bodyB->getRotation() * m_localAnchorB : 
        m_localAnchorB;
    
    // Distance vector
    glm::vec3 d = worldAnchorB - worldAnchorA;
    float currentDistance = glm::length(d);
    
    if (currentDistance < math::constants::EPSILON) {
        // Bodies are coincident - use a default direction
        d = glm::vec3(1, 0, 0);
        currentDistance = math::constants::EPSILON;
    } else {
        d /= currentDistance; // Normalize
    }
    
    // Constraint violation (C = |distance| - targetDistance)
    float C = currentDistance - m_targetDistance;
    
    // Jacobian: J = [d, r_A x d, -d, -r_B x d]
    glm::vec3 rA = worldAnchorA - m_bodyA->getPosition();
    glm::vec3 rB = m_bodyB ? worldAnchorB - m_bodyB->getPosition() : glm::vec3(0.0f);
    
    m_jacobians[0] = JacobianEntry(
        d,                    // Linear A
        glm::cross(rA, d),   // Angular A
        -d,                  // Linear B
        -glm::cross(rB, d)   // Angular B
    );
    
    // Calculate effective mass
    m_effectiveMass[0] = computeEffectiveMass(m_jacobians[0]);
    
    // Bias for position correction (Baumgarte stabilization)
    const float baumgarte = 0.2f;
    m_bias[0] = -(baumgarte / deltaTime) * C;
}

void DistanceConstraint::warmStart() {
    if (m_jacobians.empty()) return;
    
    // Apply cached impulse from previous frame
    m_jacobians[0].applyImpulse(m_bodyA, m_bodyB, m_lambdas[0]);
}

void DistanceConstraint::solveVelocityConstraints(float deltaTime) {
    if (m_jacobians.empty() || m_effectiveMass[0] <= 0.0f) return;
    
    // Calculate constraint velocity: JV
    float jv = m_jacobians[0].computeJV(m_bodyA, m_bodyB);
    
    // Compute impulse: lambda = -effectiveMass * (JV + bias)
    float deltaLambda = -m_effectiveMass[0] * (jv + m_bias[0]);
    
    // Apply impulse (no clamping for distance constraint)
    m_jacobians[0].applyImpulse(m_bodyA, m_bodyB, deltaLambda);
    m_lambdas[0] += deltaLambda;
    m_appliedImpulse += glm::abs(deltaLambda);
}

void DistanceConstraint::solvePositionConstraints(float deltaTime) {
    // Distance constraint is typically stable with velocity-level solving
    // Position correction is handled through bias term
}

// === BALL SOCKET CONSTRAINT IMPLEMENTATION ===

BallSocketConstraint::BallSocketConstraint(dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
                                          dynamics::RigidBody* bodyB, const glm::vec3& anchorB)
    : Constraint(Type::BALL_SOCKET, bodyA, bodyB)
    , m_localAnchorA(anchorA)
    , m_localAnchorB(anchorB)
{
    allocateConstraintData(3); // Three constraint equations (x, y, z)
}

void BallSocketConstraint::updateJacobians(float deltaTime) {
    if (!m_bodyA) return;
    
    // Calculate world positions
    glm::vec3 worldAnchorA = m_bodyA->getPosition() + m_bodyA->getRotation() * m_localAnchorA;
    glm::vec3 worldAnchorB = m_bodyB ? 
        m_bodyB->getPosition() + m_bodyB->getRotation() * m_localAnchorB : 
        m_localAnchorB;
    
    // Constraint violation vector
    glm::vec3 C = worldAnchorB - worldAnchorA;
    
    // Relative positions
    glm::vec3 rA = worldAnchorA - m_bodyA->getPosition();
    glm::vec3 rB = m_bodyB ? worldAnchorB - m_bodyB->getPosition() : glm::vec3(0.0f);
    
    // Create Jacobians for each axis
    for (int i = 0; i < 3; ++i) {
        glm::vec3 axis(0.0f);
        axis[i] = 1.0f;
        
        m_jacobians[i] = JacobianEntry(
            axis,                    // Linear A
            glm::cross(rA, axis),   // Angular A
            -axis,                  // Linear B
            -glm::cross(rB, axis)   // Angular B
        );
        
        // Calculate effective mass
        m_effectiveMass[i] = computeEffectiveMass(m_jacobians[i]);
        
        // Bias for position correction
        const float baumgarte = 0.2f;
        m_bias[i] = -(baumgarte / deltaTime) * C[i];
    }
}

void BallSocketConstraint::warmStart() {
    for (size_t i = 0; i < m_jacobians.size(); ++i) {
        m_jacobians[i].applyImpulse(m_bodyA, m_bodyB, m_lambdas[i]);
    }
}

void BallSocketConstraint::solveVelocityConstraints(float deltaTime) {
    for (size_t i = 0; i < m_jacobians.size(); ++i) {
        if (m_effectiveMass[i] <= 0.0f) continue;
        
        // Calculate constraint velocity
        float jv = m_jacobians[i].computeJV(m_bodyA, m_bodyB);
        
        // Compute impulse
        float deltaLambda = -m_effectiveMass[i] * (jv + m_bias[i]);
        
        // Apply impulse
        m_jacobians[i].applyImpulse(m_bodyA, m_bodyB, deltaLambda);
        m_lambdas[i] += deltaLambda;
        m_appliedImpulse += glm::abs(deltaLambda);
    }
}

void BallSocketConstraint::solvePositionConstraints(float deltaTime) {
    // Position correction handled through bias
}

// === CONTACT CONSTRAINT IMPLEMENTATION ===

ContactConstraint::ContactConstraint(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                     const ContactData& contactData)
    : Constraint(Type::CONTACT, bodyA, bodyB)
    , m_contactData(contactData)
{
    allocateConstraintData(3); // Normal + 2 friction directions
    
    // Set limits: normal impulse >= 0, friction limited by normal force
    m_lowerLimits[m_normalIndex] = 0.0f;
    m_upperLimits[m_normalIndex] = std::numeric_limits<float>::max();
    
    // Friction limits will be set dynamically based on normal impulse
}

void ContactConstraint::updateJacobians(float deltaTime) {
    if (!m_bodyA || !m_bodyB) return;
    
    // Calculate world contact points
    glm::vec3 worldPointA = m_bodyA->getPosition() + m_bodyA->getRotation() * m_contactData.localPointA;
    glm::vec3 worldPointB = m_bodyB->getPosition() + m_bodyB->getRotation() * m_contactData.localPointB;
    
    // Relative positions
    glm::vec3 rA = worldPointA - m_bodyA->getPosition();
    glm::vec3 rB = worldPointB - m_bodyB->getPosition();
    
    // Normal constraint Jacobian
    m_jacobians[m_normalIndex] = JacobianEntry(
        m_contactData.normal,
        glm::cross(rA, m_contactData.normal),
        -m_contactData.normal,
        -glm::cross(rB, m_contactData.normal)
    );
    
    // Friction constraint Jacobians
    m_jacobians[m_friction1Index] = JacobianEntry(
        m_contactData.tangent1,
        glm::cross(rA, m_contactData.tangent1),
        -m_contactData.tangent1,
        -glm::cross(rB, m_contactData.tangent1)
    );
    
    m_jacobians[m_friction2Index] = JacobianEntry(
        m_contactData.tangent2,
        glm::cross(rA, m_contactData.tangent2),
        -m_contactData.tangent2,
        -glm::cross(rB, m_contactData.tangent2)
    );
    
    // Calculate effective masses
    for (size_t i = 0; i < m_jacobians.size(); ++i) {
        m_effectiveMass[i] = computeEffectiveMass(m_jacobians[i]);
    }
    
    // Bias for position correction (penetration resolution)
    const float baumgarte = 0.2f;
    const float slop = 0.01f; // Allow small penetration
    float penetrationError = std::max(0.0f, m_contactData.penetration - slop);
    m_bias[m_normalIndex] = -(baumgarte / deltaTime) * penetrationError;
    
    // Add restitution bias for bouncing
    float relativeVelocity = m_jacobians[m_normalIndex].computeJV(m_bodyA, m_bodyB);
    if (relativeVelocity < -1.0f) { // Only apply restitution for significant approach velocity
        m_bias[m_normalIndex] += m_contactData.restitution * relativeVelocity;
    }
}

void ContactConstraint::warmStart() {
    for (size_t i = 0; i < m_jacobians.size(); ++i) {
        m_jacobians[i].applyImpulse(m_bodyA, m_bodyB, m_lambdas[i]);
    }
}

void ContactConstraint::solveVelocityConstraints(float deltaTime) {
    // Solve normal constraint
    if (m_effectiveMass[m_normalIndex] > 0.0f) {
        float jv = m_jacobians[m_normalIndex].computeJV(m_bodyA, m_bodyB);
        float deltaLambda = -m_effectiveMass[m_normalIndex] * (jv + m_bias[m_normalIndex]);
        clampAccumulatedImpulse(m_normalIndex, deltaLambda);
    }
    
    // Update friction limits based on normal impulse
    float frictionLimit = m_contactData.friction * m_lambdas[m_normalIndex];
    m_lowerLimits[m_friction1Index] = -frictionLimit;
    m_upperLimits[m_friction1Index] = frictionLimit;
    m_lowerLimits[m_friction2Index] = -frictionLimit;
    m_upperLimits[m_friction2Index] = frictionLimit;
    
    // Solve friction constraints
    for (size_t i = m_friction1Index; i <= m_friction2Index; ++i) {
        if (m_effectiveMass[i] > 0.0f) {
            float jv = m_jacobians[i].computeJV(m_bodyA, m_bodyB);
            float deltaLambda = -m_effectiveMass[i] * jv;
            clampAccumulatedImpulse(i, deltaLambda);
        }
    }
}

void ContactConstraint::solvePositionConstraints(float deltaTime) {
    // Position correction handled through bias in velocity constraints
}

} // namespace constraints
} // namespace physics
} // namespace ohao