#include "constraint_solver.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/collision/contact_manifold.hpp"
#include <algorithm>

namespace ohao {
namespace physics {
namespace constraints {

ConstraintSolver::ConstraintSolver(const SolverConfig& config)
    : m_config(config)
    , m_lastVelocityIterations(0)
    , m_lastPositionIterations(0)
{
}

ConstraintSolver::~ConstraintSolver() {
}

void ConstraintSolver::solve(std::vector<collision::ContactManifold*>& manifolds, float deltaTime) {
    if (manifolds.empty()) return;

    // Phase 1: Setup
    setupConstraints(manifolds);

    // Phase 2: Warm start
    if (m_config.warmStartFactor > 0.0f) {
        warmStart(manifolds);
    }

    // Phase 3: Velocity solver (PGS)
    solveVelocityConstraints(manifolds, deltaTime);

    // Phase 4: Position solver (XPBD)
    if (m_config.positionIterations > 0) {
        solvePositionConstraints(manifolds);
    }
}

void ConstraintSolver::setupConstraints(std::vector<collision::ContactManifold*>& manifolds) {
    // Pre-compute constraint data (Jacobians, effective masses, etc.)
    // For now, this is done per-contact in the solve methods
    // Could be optimized by caching here
}

void ConstraintSolver::warmStart(std::vector<collision::ContactManifold*>& manifolds) {
    // Apply cached impulses from previous frame
    for (auto* manifold : manifolds) {
        RigidBody* bodyA = manifold->getBodyA();
        RigidBody* bodyB = manifold->getBodyB();
        if (!bodyA || !bodyB) continue;

        glm::vec3 normal = manifold->getNormal();
        glm::vec3 tangent1 = manifold->getTangent1();
        glm::vec3 tangent2 = manifold->getTangent2();

        for (int i = 0; i < manifold->getContactCount(); ++i) {
            auto& contact = manifold->getContact(i);

            glm::vec3 rA = contact.position - bodyA->getPosition();
            glm::vec3 rB = contact.position - bodyB->getPosition();

            // Apply normal impulse
            glm::vec3 normalImpulse = normal * contact.normalImpulse * m_config.warmStartFactor;
            applyImpulse(bodyA, normalImpulse, rA);
            applyImpulse(bodyB, -normalImpulse, rB);

            // Apply tangent impulses
            glm::vec3 tangentImpulse = tangent1 * contact.tangentImpulse1 + tangent2 * contact.tangentImpulse2;
            tangentImpulse *= m_config.warmStartFactor;
            applyImpulse(bodyA, tangentImpulse, rA);
            applyImpulse(bodyB, -tangentImpulse, rB);
        }
    }
}

void ConstraintSolver::solveVelocityConstraints(std::vector<collision::ContactManifold*>& manifolds, float deltaTime) {
    m_lastVelocityIterations = m_config.velocityIterations;

    for (int iter = 0; iter < m_config.velocityIterations; ++iter) {
        for (auto* manifold : manifolds) {
            for (int i = 0; i < manifold->getContactCount(); ++i) {
                solveContact(manifold, i, deltaTime);
                solveFriction(manifold, i, deltaTime);
            }
        }
    }
}

void ConstraintSolver::solvePositionConstraints(std::vector<collision::ContactManifold*>& manifolds) {
    m_lastPositionIterations = m_config.positionIterations;

    for (int iter = 0; iter < m_config.positionIterations; ++iter) {
        for (auto* manifold : manifolds) {
            for (int i = 0; i < manifold->getContactCount(); ++i) {
                solvePositionContact(manifold, i);
            }
        }
    }
}

void ConstraintSolver::solveContact(collision::ContactManifold* manifold, int contactIndex, float deltaTime) {
    auto& contact = manifold->getContact(contactIndex);
    RigidBody* bodyA = manifold->getBodyA();
    RigidBody* bodyB = manifold->getBodyB();

    if (!bodyA || !bodyB) return;
    if (bodyA->isStatic() && bodyB->isStatic()) return;

    glm::vec3 normal = manifold->getNormal();
    glm::vec3 rA = contact.position - bodyA->getPosition();
    glm::vec3 rB = contact.position - bodyB->getPosition();

    // Compute relative velocity
    glm::vec3 relVel = computeRelativeVelocity(bodyA, bodyB, rA, rB);
    float normalVel = glm::dot(relVel, normal);

    // Compute bias velocity (for restitution)
    float restitution = manifold->getRestitution();
    float biasVelocity = 0.0f;
    if (normalVel < -0.1f) {  // Lower threshold for better collision response
        biasVelocity = -restitution * normalVel;
    }

    // Compute effective mass
    float invMassA = bodyA->isStatic() ? 0.0f : bodyA->getInverseMass();
    float invMassB = bodyB->isStatic() ? 0.0f : bodyB->getInverseMass();

    glm::vec3 rAxN = glm::cross(rA, normal);
    glm::vec3 rBxN = glm::cross(rB, normal);

    glm::mat3 invInertiaA = bodyA->isStatic() ? glm::mat3(0.0f) : bodyA->getWorldInverseInertiaTensor();
    glm::mat3 invInertiaB = bodyB->isStatic() ? glm::mat3(0.0f) : bodyB->getWorldInverseInertiaTensor();

    float kNormal = invMassA + invMassB +
                    glm::dot(rAxN, invInertiaA * rAxN) +
                    glm::dot(rBxN, invInertiaB * rBxN);

    if (kNormal <= 0.0f) return;  // Static/zero mass

    // Compute lambda (impulse magnitude)
    float lambda = -(normalVel - biasVelocity) / kNormal;

    // Accumulate impulse and clamp (non-negative)
    float oldImpulse = contact.normalImpulse;
    contact.normalImpulse = std::max(oldImpulse + lambda, 0.0f);
    lambda = contact.normalImpulse - oldImpulse;

    // Apply impulse
    glm::vec3 impulse = normal * lambda;
    applyImpulse(bodyA, impulse, rA);
    applyImpulse(bodyB, -impulse, rB);
}

void ConstraintSolver::solveFriction(collision::ContactManifold* manifold, int contactIndex, float deltaTime) {
    auto& contact = manifold->getContact(contactIndex);
    RigidBody* bodyA = manifold->getBodyA();
    RigidBody* bodyB = manifold->getBodyB();

    if (!bodyA || !bodyB) return;
    if (bodyA->isStatic() && bodyB->isStatic()) return;

    glm::vec3 tangent1 = manifold->getTangent1();
    glm::vec3 tangent2 = manifold->getTangent2();
    glm::vec3 rA = contact.position - bodyA->getPosition();
    glm::vec3 rB = contact.position - bodyB->getPosition();

    // Compute relative velocity
    glm::vec3 relVel = computeRelativeVelocity(bodyA, bodyB, rA, rB);

    float invMassA = bodyA->isStatic() ? 0.0f : bodyA->getInverseMass();
    float invMassB = bodyB->isStatic() ? 0.0f : bodyB->getInverseMass();

    glm::mat3 invInertiaA = bodyA->isStatic() ? glm::mat3(0.0f) : bodyA->getWorldInverseInertiaTensor();
    glm::mat3 invInertiaB = bodyB->isStatic() ? glm::mat3(0.0f) : bodyB->getWorldInverseInertiaTensor();

    // Solve tangent1
    float tangentVel1 = glm::dot(relVel, tangent1);
    glm::vec3 rAxT1 = glm::cross(rA, tangent1);
    glm::vec3 rBxT1 = glm::cross(rB, tangent1);
    float kTangent1 = invMassA + invMassB +
                       glm::dot(rAxT1, invInertiaA * rAxT1) +
                       glm::dot(rBxT1, invInertiaB * rBxT1);

    float lambda1 = 0.0f;
    if (kTangent1 > 0.0f) {
        lambda1 = -tangentVel1 / kTangent1;
    }

    // Solve tangent2
    float tangentVel2 = glm::dot(relVel, tangent2);
    glm::vec3 rAxT2 = glm::cross(rA, tangent2);
    glm::vec3 rBxT2 = glm::cross(rB, tangent2);
    float kTangent2 = invMassA + invMassB +
                       glm::dot(rAxT2, invInertiaA * rAxT2) +
                       glm::dot(rBxT2, invInertiaB * rBxT2);

    float lambda2 = 0.0f;
    if (kTangent2 > 0.0f) {
        lambda2 = -tangentVel2 / kTangent2;
    }

    // Apply Coulomb friction cone constraint
    float maxFriction = manifold->getFriction() * contact.normalImpulse;
    float frictionMag = std::sqrt(lambda1 * lambda1 + lambda2 * lambda2);

    if (frictionMag > maxFriction && frictionMag > 0.0f) {
        float scale = maxFriction / frictionMag;
        lambda1 *= scale;
        lambda2 *= scale;
    }

    // Accumulate
    contact.tangentImpulse1 += lambda1;
    contact.tangentImpulse2 += lambda2;

    // Apply impulses
    glm::vec3 impulse = tangent1 * lambda1 + tangent2 * lambda2;
    applyImpulse(bodyA, impulse, rA);
    applyImpulse(bodyB, -impulse, rB);
}

void ConstraintSolver::solvePositionContact(collision::ContactManifold* manifold, int contactIndex) {
    auto& contact = manifold->getContact(contactIndex);
    RigidBody* bodyA = manifold->getBodyA();
    RigidBody* bodyB = manifold->getBodyB();

    if (!bodyA || !bodyB) return;
    if (bodyA->isStatic() && bodyB->isStatic()) return;

    glm::vec3 normal = manifold->getNormal();

    // XPBD position correction (Baumgarte stabilization)
    // Use iterative correction with damping to prevent overshooting
    float penetration = contact.penetration;
    if (penetration <= 0.001f) return;  // Skip if penetration < 1mm (converged)

    // Apply Baumgarte-stabilized correction (fraction per iteration)
    // This prevents over-correction across multiple iterations
    float correction = m_config.baumgarte * penetration;
    correction = std::min(correction, m_config.maxLinearCorrection);

    float invMassA = bodyA->isStatic() ? 0.0f : bodyA->getInverseMass();
    float invMassB = bodyB->isStatic() ? 0.0f : bodyB->getInverseMass();
    float totalInvMass = invMassA + invMassB;

    if (totalInvMass <= 0.0f) return;

    glm::vec3 correctionVector = normal * (correction / totalInvMass);

    // Apply position corrections
    if (!bodyA->isStatic()) {
        bodyA->setPosition(bodyA->getPosition() + correctionVector * invMassA);
    }
    if (!bodyB->isStatic()) {
        bodyB->setPosition(bodyB->getPosition() - correctionVector * invMassB);
    }
}

glm::vec3 ConstraintSolver::computeRelativeVelocity(RigidBody* bodyA, RigidBody* bodyB,
                                                     const glm::vec3& rA, const glm::vec3& rB)
{
    glm::vec3 velA = bodyA->getLinearVelocity() + glm::cross(bodyA->getAngularVelocity(), rA);
    glm::vec3 velB = bodyB->getLinearVelocity() + glm::cross(bodyB->getAngularVelocity(), rB);
    return velA - velB;
}

void ConstraintSolver::applyImpulse(RigidBody* body, const glm::vec3& impulse, const glm::vec3& contactPoint) {
    if (body->isStatic()) return;

    // Linear impulse
    body->setLinearVelocity(body->getLinearVelocity() + impulse * body->getInverseMass());

    // Angular impulse
    // CRITICAL FIX: contactPoint is already relative (rA/rB), don't subtract position again!
    glm::vec3 torque = glm::cross(contactPoint, impulse);
    glm::mat3 invInertia = body->getWorldInverseInertiaTensor();
    body->setAngularVelocity(body->getAngularVelocity() + invInertia * torque);
}

}}} // namespace ohao::physics::constraints
