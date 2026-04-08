#pragma once

#include <glm/glm.hpp>

namespace ohao {
namespace physics {

struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float timeStep{1.0f / 60.0f};
    int maxSubSteps{10};
    float fixedTimeStep{1.0f / 240.0f};
    bool enableCCD{true}; // Continuous Collision Detection
    bool enableDebugDraw{false};
    
    // Performance settings
    int maxCollisionPairs{1000};
    float sleepThreshold{0.1f};
    float sleepTimeout{2.0f};
};

} // namespace physics  
} // namespace ohao