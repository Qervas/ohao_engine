#pragma once

#include "physics/dynamics/rigid_body.hpp"
#include "physics/utils/physics_math.hpp"
#include <vector>

namespace ohao {
namespace physics {
namespace dynamics {

// Enhanced physics integrator with multiple integration schemes
class PhysicsIntegrator {
public:
    enum class Method {
        EXPLICIT_EULER,         // Simple but less stable
        SEMI_IMPLICIT_EULER,    // More stable, good for games
        VERLET,                 // Good energy conservation
        RK4,                    // Highly accurate but expensive
        SYMPLECTIC_EULER        // Best for orbital mechanics
    };
    
    // Integration configuration
    struct Config {
        Method method{Method::SEMI_IMPLICIT_EULER};
        float maxTimeStep{1.0f / 60.0f};     // Maximum integration timestep
        int maxSubSteps{4};                   // Maximum sub-stepping
        float velocityDamping{0.99f};         // Global velocity damping
        float angularDamping{0.98f};          // Global angular damping
        bool enableSleeping{true};            // Sleep inactive bodies
        float sleepLinearThreshold{0.1f};     // Linear velocity sleep threshold
        float sleepAngularThreshold{0.1f};    // Angular velocity sleep threshold
        float sleepTime{1.0f};                // Time before sleep
        glm::vec3 gravity{0.0f, -9.81f, 0.0f}; // World gravity
    };
    
    PhysicsIntegrator();
    explicit PhysicsIntegrator(const Config& config);
    ~PhysicsIntegrator() = default;
    
    // Configuration
    void setConfig(const Config& config) { m_config = config; }
    const Config& getConfig() const { return m_config; }
    
    // Main integration interface
    void integrate(std::vector<RigidBody*>& bodies, float deltaTime);
    void integrateBody(RigidBody* body, float deltaTime);
    
    // Individual integration methods
    void integrateExplicitEuler(RigidBody* body, float deltaTime);
    void integrateSemiImplicitEuler(RigidBody* body, float deltaTime);
    void integrateVerlet(RigidBody* body, float deltaTime);
    void integrateRK4(RigidBody* body, float deltaTime);
    void integrateSymplecticEuler(RigidBody* body, float deltaTime);
    
    // Utility functions
    void applyGravity(std::vector<RigidBody*>& bodies);
    void applyDamping(std::vector<RigidBody*>& bodies);
    void updateSleepStates(std::vector<RigidBody*>& bodies, float deltaTime);
    void updateTransforms(std::vector<RigidBody*>& bodies);
    
    // Statistics
    struct IntegratorStats {
        int totalBodies{0};
        int activeBodies{0};
        int sleepingBodies{0};
        int subStepsUsed{0};
        float integrationTimeMs{0.0f};
    };
    
    const IntegratorStats& getStats() const { return m_stats; }
    void clearStats();

private:
    Config m_config;
    IntegratorStats m_stats;
    
    // Verlet integration state (stored between frames)
    struct VerletState {
        glm::vec3 previousPosition{0.0f};
        glm::quat previousRotation{1, 0, 0, 0};
        bool initialized{false};
    };
    
    std::unordered_map<RigidBody*, VerletState> m_verletStates;
    
    // RK4 derivatives structure
    struct RK4Derivative {
        glm::vec3 velocity{0.0f};
        glm::vec3 force{0.0f};
        glm::vec3 angularVelocity{0.0f};
        glm::vec3 torque{0.0f};
    };
    
    // Helper functions
    RK4Derivative calculateDerivative(RigidBody* body, float deltaTime, const RK4Derivative& previous);
    glm::vec3 calculateTotalForce(RigidBody* body);
    glm::vec3 calculateTotalTorque(RigidBody* body);
    bool shouldBodySleep(RigidBody* body, float deltaTime);
    void clampVelocities(RigidBody* body);
    
    // Sub-stepping support
    void integrateWithSubStepping(std::vector<RigidBody*>& bodies, float deltaTime);
};

// Specialized integrator for different physics scenarios
namespace IntegratorPresets {
    // High-precision configuration for simulations
    PhysicsIntegrator::Config createHighPrecision();
    
    // Performance-optimized configuration for games
    PhysicsIntegrator::Config createGameOptimized();
    
    // Stable configuration for stacking scenarios
    PhysicsIntegrator::Config createStableStacking();
    
    // Configuration for space/orbital mechanics
    PhysicsIntegrator::Config createOrbitalMechanics();
}

// Energy and momentum conservation utilities
class ConservationAnalyzer {
public:
    struct ConservationData {
        float totalKineticEnergy{0.0f};
        float totalPotentialEnergy{0.0f};
        glm::vec3 totalLinearMomentum{0.0f};
        glm::vec3 totalAngularMomentum{0.0f};
        glm::vec3 centerOfMass{0.0f};
        float totalMass{0.0f};
    };
    
    static ConservationData analyze(const std::vector<RigidBody*>& bodies, const glm::vec3& gravity);
    static void logConservationViolation(const ConservationData& before, const ConservationData& after);
    
    // Energy correction (experimental)
    static void correctEnergyDrift(std::vector<RigidBody*>& bodies, float targetEnergy);
};

// Advanced integration utilities
namespace IntegrationUtils {
    // Adaptive timestep calculation based on body velocities
    float calculateAdaptiveTimeStep(const std::vector<RigidBody*>& bodies, float maxTimeStep);
    
    // Stability analysis
    bool isSystemStable(const std::vector<RigidBody*>& bodies);
    
    // Velocity clamping with energy preservation
    void clampVelocityPreserveEnergy(RigidBody* body, float maxLinearVelocity, float maxAngularVelocity);
    
    // Position correction for constraint drift
    void correctPositionDrift(RigidBody* body, const glm::vec3& correctionDirection, float maxCorrection);
    
    // Quaternion integration utilities
    glm::quat integrateQuaternion(const glm::quat& current, const glm::vec3& angularVelocity, float deltaTime);
    glm::vec3 quaternionDerivative(const glm::quat& rotation, const glm::vec3& angularVelocity);
}

} // namespace dynamics
} // namespace physics
} // namespace ohao