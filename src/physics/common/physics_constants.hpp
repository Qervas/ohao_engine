#pragma once

#include <glm/glm.hpp>

namespace ohao {
namespace physics {

// === PHYSICS CONSTANTS ===
namespace constants {
    // Mathematical constants
    constexpr float PI = 3.14159265359f;
    constexpr float TWO_PI = 2.0f * PI;
    constexpr float HALF_PI = PI * 0.5f;
    constexpr float EPSILON = 1e-6f;
    constexpr float LARGE_NUMBER = 1e6f;
    
    // Physics constants
    constexpr float GRAVITY_EARTH = 9.81f;
    constexpr float GRAVITY_MOON = 1.62f;
    constexpr float GRAVITY_MARS = 3.71f;
    
    // Simulation limits
    constexpr float MAX_LINEAR_VELOCITY = 100.0f;
    constexpr float MAX_ANGULAR_VELOCITY = 50.0f;
    constexpr float MIN_MASS = 1e-3f;
    constexpr float MAX_MASS = 1e6f;
    
    // Sleep system constants
    constexpr float SLEEP_LINEAR_THRESHOLD = 0.1f;
    constexpr float SLEEP_ANGULAR_THRESHOLD = 0.1f;
    constexpr float SLEEP_TIMEOUT = 2.0f;
    
    // Collision constants
    constexpr float CONTACT_PENETRATION_SLOP = 0.01f;
    constexpr float CONTACT_BAUMGARTE_FACTOR = 0.2f;
    constexpr float RESTITUTION_THRESHOLD = 1.0f;
    
    // Integration constants
    constexpr float MIN_TIMESTEP = 1e-6f;
    constexpr float MAX_TIMESTEP = 1.0f / 30.0f; // 30 FPS minimum
    constexpr int MAX_SUBSTEPS = 10;
}

// === PHYSICS CONFIGURATION ===
struct PhysicsConfig {
    // World settings
    glm::vec3 gravity{0.0f, -constants::GRAVITY_EARTH, 0.0f};
    bool enableGravity{true};
    
    // Simulation settings
    float fixedTimeStep{1.0f / 60.0f}; // 60 Hz
    int maxSubsteps{constants::MAX_SUBSTEPS};
    bool enableContinuousCollisionDetection{false};
    
    // Solver settings
    int velocityIterations{8};
    int positionIterations{3};
    bool enableWarmStarting{true};
    
    // Sleep settings
    bool enableSleeping{true};
    float sleepLinearThreshold{constants::SLEEP_LINEAR_THRESHOLD};
    float sleepAngularThreshold{constants::SLEEP_ANGULAR_THRESHOLD};
    float sleepTimeout{constants::SLEEP_TIMEOUT};
    
    // Performance settings
    bool enableBroadPhase{true};
    bool enableSpatialOptimization{true};
    int maxContactsPerPair{4};
    
    // Debug settings
    bool enableDebugVisualization{false};
    bool logCollisions{false};
    bool validateInputs{true};
};

// === INERTIA TENSOR UTILITIES ===
namespace inertia {
    // Calculate inertia tensor for basic shapes
    glm::mat3 calculateBoxTensor(float mass, const glm::vec3& dimensions);
    glm::mat3 calculateSphereTensor(float mass, float radius);
    glm::mat3 calculateCylinderTensor(float mass, float radius, float height);
    glm::mat3 calculateCapsuleTensor(float mass, float radius, float height);
    
    // Transform inertia tensor to world space
    glm::mat3 transformToWorldSpace(const glm::mat3& localTensor, const glm::quat& rotation);
    
    // Calculate inverse inertia tensor
    glm::mat3 calculateInverse(const glm::mat3& tensor);
    
    // Combine multiple inertia tensors (for compound shapes)
    glm::mat3 combine(const glm::mat3& tensorA, float massA, const glm::vec3& offsetA,
                     const glm::mat3& tensorB, float massB, const glm::vec3& offsetB);
}

} // namespace physics
} // namespace ohao