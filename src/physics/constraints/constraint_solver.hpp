#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace ohao {
namespace physics {

// Forward declarations
namespace dynamics {
    class RigidBody;
}

namespace collision {
    class ContactManifold;
}

namespace constraints {
using dynamics::RigidBody;

// Solver configuration
struct SolverConfig {
    int velocityIterations = 8;        // PGS velocity iterations
    int positionIterations = 1;        // XPBD position iterations (single pass to prevent cumulative over-correction)
    float baumgarte = 0.8f;            // Position correction strength (high for single-iteration convergence)
    float slop = 0.001f;               // Allowed penetration (1mm, reduced from 5mm to prevent jiggling)
    float maxLinearCorrection = 0.2f;  // Max position correction per iteration
    float warmStartFactor = 0.8f;      // Warm start multiplier
    bool splitImpulses = true;         // Separate position/velocity correction
};

// Constraint solver (PGS + XPBD hybrid)
class ConstraintSolver {
public:
    ConstraintSolver(const SolverConfig& config = SolverConfig());
    ~ConstraintSolver();

    // Solve constraints
    void solve(std::vector<collision::ContactManifold*>& manifolds, float deltaTime);

    // Configuration
    void setConfig(const SolverConfig& config) { m_config = config; }
    const SolverConfig& getConfig() const { return m_config; }

    // Stats
    int getLastVelocityIterations() const { return m_lastVelocityIterations; }
    int getLastPositionIterations() const { return m_lastPositionIterations; }

private:
    // Solver phases
    void setupConstraints(std::vector<collision::ContactManifold*>& manifolds);
    void warmStart(std::vector<collision::ContactManifold*>& manifolds);
    void solveVelocityConstraints(std::vector<collision::ContactManifold*>& manifolds, float deltaTime);
    void solvePositionConstraints(std::vector<collision::ContactManifold*>& manifolds);

    // Helper methods
    void solveContact(collision::ContactManifold* manifold, int contactIndex, float deltaTime);
    void solveFriction(collision::ContactManifold* manifold, int contactIndex, float deltaTime);
    void solvePositionContact(collision::ContactManifold* manifold, int contactIndex);

    // Compute relative velocity at contact
    glm::vec3 computeRelativeVelocity(RigidBody* bodyA, RigidBody* bodyB,
                                       const glm::vec3& rA, const glm::vec3& rB);

    // Apply impulse to body
    void applyImpulse(RigidBody* body, const glm::vec3& impulse, const glm::vec3& contactPoint);

    // Configuration
    SolverConfig m_config;

    // Stats
    int m_lastVelocityIterations;
    int m_lastPositionIterations;
};

}}} // namespace ohao::physics::constraints
