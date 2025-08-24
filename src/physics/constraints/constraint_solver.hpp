#pragma once

#include "constraint.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <vector>
#include <memory>
#include <chrono>

namespace ohao {
namespace physics {
namespace constraints {

// Constraint solver using Sequential Impulses (SI) method
class ConstraintSolver {
public:
    // Solver configuration
    struct Config {
        int velocityIterations{8};      // Velocity constraint iterations
        int positionIterations{3};      // Position constraint iterations
        float baumgarteSlop{0.01f};     // Allowed penetration before correction
        float baumgarteFactor{0.2f};    // Position correction strength
        bool enableWarmStarting{true};  // Use previous frame impulses
        bool enableFriction{true};      // Enable friction constraints
        float linearSlop{0.005f};       // Linear position tolerance
        float angularSlop{0.1f};        // Angular position tolerance (radians)
        float maxLinearCorrection{0.2f}; // Max position correction per iteration
        float maxAngularCorrection{8.0f * 3.14159f / 180.0f}; // Max angular correction (8 degrees)
    };
    
    ConstraintSolver();
    explicit ConstraintSolver(const Config& config);
    ~ConstraintSolver() = default;
    
    // Configuration
    void setConfig(const Config& config) { m_config = config; }
    const Config& getConfig() const { return m_config; }
    
    // Solver interface
    void solveConstraints(std::vector<std::unique_ptr<Constraint>>& constraints, float deltaTime);
    void solveVelocityConstraints(std::vector<std::unique_ptr<Constraint>>& constraints, float deltaTime);
    void solvePositionConstraints(std::vector<std::unique_ptr<Constraint>>& constraints, float deltaTime);
    
    // Add/remove constraints
    void addConstraint(std::unique_ptr<Constraint> constraint);
    void removeConstraint(Constraint* constraint);
    void removeAllConstraints();
    
    // Solver island management (for performance)
    struct ConstraintIsland {
        std::vector<dynamics::RigidBody*> bodies;
        std::vector<Constraint*> constraints;
        bool needsSolving{true};
    };
    
    void buildConstraintIslands(std::vector<std::unique_ptr<Constraint>>& constraints);
    void solveIsland(ConstraintIsland& island, float deltaTime);
    
    // Statistics
    struct SolverStats {
        int totalConstraints{0};
        int activeConstraints{0};
        int velocityIterationsUsed{0};
        int positionIterationsUsed{0};
        int constraintIslands{0};
        float solverTimeMs{0.0f};
        float velocityTimeMs{0.0f};
        float positionTimeMs{0.0f};
    };
    
    const SolverStats& getStats() const { return m_stats; }
    void clearStats();

private:
    Config m_config;
    SolverStats m_stats;
    
    // Constraint storage
    std::vector<std::unique_ptr<Constraint>> m_constraints;
    std::vector<ConstraintIsland> m_islands;
    
    // Island building helpers
    void addBodyToIsland(dynamics::RigidBody* body, ConstraintIsland& island, std::vector<bool>& visited);
    bool areConnected(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, 
                      const std::vector<Constraint*>& constraints);
    
    // Performance tracking
    std::chrono::high_resolution_clock::time_point m_solverStartTime;
};

// Constraint factory for creating common constraint types
class ConstraintFactory {
public:
    // Distance constraints
    static std::unique_ptr<DistanceConstraint> createDistance(
        dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
        dynamics::RigidBody* bodyB, const glm::vec3& anchorB,
        float distance = -1.0f);
    
    // Joint constraints
    static std::unique_ptr<BallSocketConstraint> createBallSocket(
        dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
        dynamics::RigidBody* bodyB, const glm::vec3& anchorB);
    
    static std::unique_ptr<HingeConstraint> createHinge(
        dynamics::RigidBody* bodyA, const glm::vec3& anchorA, const glm::vec3& axisA,
        dynamics::RigidBody* bodyB, const glm::vec3& anchorB, const glm::vec3& axisB);
    
    // Spring constraints
    static std::unique_ptr<SpringConstraint> createSpring(
        dynamics::RigidBody* bodyA, const glm::vec3& anchorA,
        dynamics::RigidBody* bodyB, const glm::vec3& anchorB,
        float restLength, float stiffness = 1000.0f, float damping = 50.0f);
    
    // Contact constraints (usually created by collision system)
    static std::unique_ptr<ContactConstraint> createContact(
        dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
        const ContactConstraint::ContactData& contactData);
    
    // Utility functions
    static glm::vec3 calculateWorldAnchor(dynamics::RigidBody* body, const glm::vec3& localAnchor);
    static glm::vec3 calculateLocalAnchor(dynamics::RigidBody* body, const glm::vec3& worldAnchor);
    static glm::vec3 calculateRelativeVelocity(dynamics::RigidBody* bodyA, const glm::vec3& pointA,
                                              dynamics::RigidBody* bodyB, const glm::vec3& pointB);
};

// Constraint manager - high-level interface for managing constraints
class ConstraintManager {
public:
    ConstraintManager();
    ~ConstraintManager() = default;
    
    // Constraint management
    void addConstraint(std::unique_ptr<Constraint> constraint);
    void removeConstraint(Constraint* constraint);
    void removeConstraintsForBody(dynamics::RigidBody* body);
    void removeAllConstraints();
    
    // Solver interface
    void step(float deltaTime);
    
    // Configuration
    void setSolverConfig(const ConstraintSolver::Config& config);
    const ConstraintSolver::Config& getSolverConfig() const;
    
    // Access
    const std::vector<std::unique_ptr<Constraint>>& getConstraints() const { return m_constraints; }
    ConstraintSolver& getSolver() { return m_solver; }
    const ConstraintSolver& getSolver() const { return m_solver; }
    
    // Statistics
    size_t getConstraintCount() const { return m_constraints.size(); }
    size_t getActiveConstraintCount() const;
    const ConstraintSolver::SolverStats& getSolverStats() const { return m_solver.getStats(); }
    
    // Debug utilities
    void enableDebugVisualization(bool enable) { m_debugVisualization = enable; }
    bool isDebugVisualizationEnabled() const { return m_debugVisualization; }
    
    struct DebugData {
        std::vector<std::pair<glm::vec3, glm::vec3>> constraintLines;
        std::vector<glm::vec3> anchorPoints;
        std::vector<std::pair<glm::vec3, glm::vec3>> forceVectors;
    };
    
    const DebugData& getDebugData() const { return m_debugData; }

private:
    std::vector<std::unique_ptr<Constraint>> m_constraints;
    ConstraintSolver m_solver;
    
    // Debug visualization
    bool m_debugVisualization{false};
    DebugData m_debugData;
    
    void updateDebugData();
};

// Constraint presets for common scenarios
namespace ConstraintPresets {
    // Chain of distance constraints
    std::vector<std::unique_ptr<DistanceConstraint>> createChain(
        const std::vector<dynamics::RigidBody*>& bodies,
        float segmentLength = -1.0f);
    
    // Rope simulation with springs
    std::vector<std::unique_ptr<SpringConstraint>> createRope(
        const std::vector<dynamics::RigidBody*>& bodies,
        float stiffness = 1000.0f,
        float damping = 50.0f);
    
    // Cloth simulation (grid of constraints)
    std::vector<std::unique_ptr<Constraint>> createCloth(
        const std::vector<std::vector<dynamics::RigidBody*>>& bodyGrid,
        float stiffness = 1000.0f);
    
    // Vehicle suspension (springs + dampers)
    std::unique_ptr<SpringConstraint> createSuspension(
        dynamics::RigidBody* chassis, dynamics::RigidBody* wheel,
        const glm::vec3& anchorPoint,
        float restLength, float stiffness, float damping);
}

} // namespace constraints
} // namespace physics
} // namespace ohao