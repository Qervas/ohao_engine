#include "constraint_solver.hpp"
#include "constraint.hpp"
#include "physics/utils/physics_math.hpp"

namespace ohao {
namespace physics {
namespace constraints {

// ConstraintSolver implementation
ConstraintSolver::ConstraintSolver() : m_config{} {}

ConstraintSolver::ConstraintSolver(const Config& config) : m_config{config} {}

void ConstraintSolver::solveConstraints(std::vector<std::unique_ptr<Constraint>>& constraints, float deltaTime) {
    if (constraints.empty()) return;
    
    // Update all jacobians
    for (const auto& constraint : constraints) {
        if (constraint && constraint->isActive()) {
            constraint->updateJacobians(deltaTime);
        }
    }
    
    // Warm start if enabled
    if (m_config.enableWarmStarting) {
        for (const auto& constraint : constraints) {
            if (constraint && constraint->isActive()) {
                constraint->warmStart();
            }
        }
    }
    
    // Solve velocity constraints
    solveVelocityConstraints(constraints, deltaTime);
    
    // Solve position constraints
    solvePositionConstraints(constraints, deltaTime);
}

void ConstraintSolver::solveVelocityConstraints(std::vector<std::unique_ptr<Constraint>>& constraints, float deltaTime) {
    for (int iteration = 0; iteration < m_config.velocityIterations; ++iteration) {
        for (const auto& constraint : constraints) {
            if (constraint && constraint->isActive()) {
                constraint->solveVelocityConstraints(deltaTime);
            }
        }
    }
}

void ConstraintSolver::solvePositionConstraints(std::vector<std::unique_ptr<Constraint>>& constraints, float deltaTime) {
    for (int iteration = 0; iteration < m_config.positionIterations; ++iteration) {
        for (const auto& constraint : constraints) {
            if (constraint && constraint->isActive()) {
                constraint->solvePositionConstraints(deltaTime);
            }
        }
    }
}

void ConstraintSolver::addConstraint(std::unique_ptr<Constraint> constraint) {
    if (constraint) {
        m_constraints.push_back(std::move(constraint));
    }
}

// ConstraintManager implementation
ConstraintManager::ConstraintManager() : m_solver{} {}

void ConstraintManager::addConstraint(std::unique_ptr<Constraint> constraint) {
    if (constraint) {
        m_constraints.push_back(std::move(constraint));
    }
}

void ConstraintManager::removeConstraint(Constraint* constraint) {
    m_constraints.erase(
        std::remove_if(m_constraints.begin(), m_constraints.end(),
            [constraint](const std::unique_ptr<Constraint>& c) {
                return c.get() == constraint;
            }),
        m_constraints.end()
    );
}

void ConstraintManager::removeConstraintsForBody(dynamics::RigidBody* body) {
    m_constraints.erase(
        std::remove_if(m_constraints.begin(), m_constraints.end(),
            [body](const std::unique_ptr<Constraint>& constraint) {
                return constraint->getBodyA() == body || constraint->getBodyB() == body;
            }),
        m_constraints.end()
    );
}

void ConstraintManager::removeAllConstraints() {
    m_constraints.clear();
}

void ConstraintManager::step(float deltaTime) {
    m_solver.solveConstraints(m_constraints, deltaTime);
}

void ConstraintManager::setSolverConfig(const ConstraintSolver::Config& config) {
    m_solver.setConfig(config);
}

const ConstraintSolver::Config& ConstraintManager::getSolverConfig() const {
    return m_solver.getConfig();
}

// ConstraintFactory implementation (basic implementations)
std::unique_ptr<DistanceConstraint> ConstraintFactory::createDistance(
    dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
    dynamics::RigidBody* bodyB, const glm::vec3& anchorB, float distance) {
    
    if (!bodyA || !bodyB) return nullptr;
    
    // TODO: Implement proper DistanceConstraint constructor
    // For now, return nullptr to avoid compilation errors
    return nullptr;
}

std::unique_ptr<BallSocketConstraint> ConstraintFactory::createBallSocket(
    dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
    dynamics::RigidBody* bodyB, const glm::vec3& anchorB) {
    
    if (!bodyA || !bodyB) return nullptr;
    
    // TODO: Implement proper BallSocketConstraint constructor
    return nullptr;
}

std::unique_ptr<HingeConstraint> ConstraintFactory::createHinge(
    dynamics::RigidBody* bodyA, const glm::vec3& anchorA, const glm::vec3& axisA,
    dynamics::RigidBody* bodyB, const glm::vec3& anchorB, const glm::vec3& axisB) {
    
    if (!bodyA || !bodyB) return nullptr;
    
    // TODO: Implement proper HingeConstraint constructor
    return nullptr;
}

std::unique_ptr<SpringConstraint> ConstraintFactory::createSpring(
    dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
    dynamics::RigidBody* bodyB, const glm::vec3& anchorB,
    float restLength, float stiffness, float damping) {
    
    if (!bodyA || !bodyB) return nullptr;
    
    // TODO: Implement proper SpringConstraint constructor
    return nullptr;
}

std::unique_ptr<ContactConstraint> ConstraintFactory::createContact(
    dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
    const ContactConstraint::ContactData& contactData) {
    
    if (!bodyA || !bodyB) return nullptr;
    
    // TODO: Implement proper ContactConstraint constructor
    return nullptr;
}

} // namespace constraints
} // namespace physics
} // namespace ohao