#pragma once

#include "physics/utils/physics_math.hpp"

namespace ohao {
namespace physics {
namespace dynamics {

// Forward declaration
class RigidBody;

enum class RigidBodyType {
    STATIC = 0,     // Never moves (ground, walls) - infinite mass
    KINEMATIC = 1,  // Moves but not affected by forces - controlled movement  
    DYNAMIC = 2     // Full physics simulation - affected by forces
};

// Physics integrator for updating object positions and velocities
class Integrator {
public:
    // Euler integration (simple but stable for most cases)
    static void integrateVelocity(RigidBody* body, float deltaTime);
    static void integratePosition(RigidBody* body, float deltaTime);
    
    // Semi-implicit Euler (more stable)
    static void integratePhysics(RigidBody* body, float deltaTime);
    
    // Apply damping to velocities
    static void applyDamping(RigidBody* body, float deltaTime);
    
    // Clamp velocities to reasonable limits
    static void clampVelocities(RigidBody* body, float maxLinearVel = 100.0f, float maxAngularVel = 50.0f);
};

} // namespace dynamics
} // namespace physics
} // namespace ohao