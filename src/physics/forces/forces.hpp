#pragma once

/**
 * Comprehensive Force System for OHAO Physics Engine
 * 
 * This header includes all available force generators:
 * - Core forces: Gravity, Drag, Springs
 * - Field forces: Explosion, Implosion, Vortex, Directional fields
 * - Environmental forces: Buoyancy, Wind, Magnetic, Surface tension
 * 
 * Usage:
 *   #include "physics/forces/forces.hpp"
 *   
 *   // Create force registry
 *   forces::ForceRegistry registry;
 *   
 *   // Add gravity force
 *   auto gravity = std::make_unique<forces::GravityForce>(glm::vec3(0, -9.81f, 0));
 *   registry.registerForce(std::move(gravity), "world_gravity");
 *   
 *   // Apply forces each frame
 *   registry.applyForces(allBodies, deltaTime);
 */

// Core force system
#include "force_generator.hpp"
#include "force_registry.hpp"

// Basic force types
#include "gravity_force.hpp"
#include "drag_force.hpp"
#include "spring_force.hpp"

// Advanced field forces
#include "field_force.hpp"

// Environmental forces
#include "environmental_force.hpp"

namespace ohao {
namespace physics {
namespace forces {

/**
 * Convenience factory for creating common force setups
 */
class ForceFactory {
public:
    // Gravity forces
    static std::unique_ptr<ForceGenerator> createGravity(const glm::vec3& gravity = glm::vec3(0.0f, -9.81f, 0.0f)) {
        return std::make_unique<GravityForce>(gravity);
    }
    
    static std::unique_ptr<ForceGenerator> createPointGravity(const glm::vec3& center, float strength = 100.0f) {
        return std::make_unique<PointGravityForce>(center, strength);
    }
    
    // Drag forces
    static std::unique_ptr<ForceGenerator> createLinearDrag(float coefficient = 0.1f) {
        return std::make_unique<LinearDragForce>(coefficient);
    }
    
    static std::unique_ptr<ForceGenerator> createAirDrag(float coefficient = 0.01f) {
        return std::make_unique<FluidDragForce>(1.2f, 0.47f, coefficient); // Air properties
    }
    
    static std::unique_ptr<ForceGenerator> createWaterDrag(float coefficient = 1.0f) {
        return std::make_unique<FluidDragForce>(1000.0f, 0.47f, coefficient); // Water properties
    }
    
    // Springs
    static std::unique_ptr<ForceGenerator> createSpring(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                                       float k = 10.0f, float restLength = 1.0f, float damping = 0.5f) {
        return std::make_unique<SpringForce>(bodyA, bodyB, k, restLength, damping);
    }
    
    static std::unique_ptr<ForceGenerator> createAnchorSpring(dynamics::RigidBody* body, const glm::vec3& anchor,
                                                             float k = 10.0f, float restLength = 1.0f, float damping = 0.5f) {
        return std::make_unique<AnchorSpringForce>(body, anchor, k, restLength, damping);
    }
    
    // Field forces
    static std::unique_ptr<ForceGenerator> createExplosion(const glm::vec3& center, float force = 1000.0f, float radius = 10.0f) {
        return std::make_unique<ExplosionForce>(center, force, radius);
    }
    
    static std::unique_ptr<ForceGenerator> createWind(const glm::vec3& direction, float strength = 10.0f, 
                                                     float turbulence = 0.1f) {
        auto wind = std::make_unique<WindForce>(direction, strength);
        wind->setTurbulence(turbulence);
        return std::move(wind);
    }
    
    static std::unique_ptr<ForceGenerator> createVortex(const glm::vec3& center, const glm::vec3& axis,
                                                       float strength = 100.0f, float radius = 10.0f) {
        return std::make_unique<VortexForce>(center, axis, strength, radius);
    }
    
    // Environmental forces
    static std::unique_ptr<ForceGenerator> createBuoyancy(float fluidDensity = 1000.0f, float liquidLevel = 0.0f) {
        return std::make_unique<BuoyancyForce>(fluidDensity, liquidLevel);
    }
    
    static std::unique_ptr<ForceGenerator> createMagneticForce(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                                              float strengthA = 1.0f, float strengthB = -1.0f) {
        return std::make_unique<MagneticForce>(bodyA, bodyB, strengthA, strengthB);
    }
    
    // Utility forces
    static std::unique_ptr<ForceGenerator> createTurbulence(float intensity = 10.0f, float frequency = 1.0f) {
        return std::make_unique<TurbulenceForce>(intensity, frequency);
    }
};

/**
 * Extended force presets for common scenarios
 */
class ForcePresets {
public:
    /**
     * Setup realistic Earth environment (gravity + air drag)
     */
    static void setupEarthEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies);
    
    /**
     * Setup space environment (no gravity, no drag)
     */
    static void setupSpaceEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies);
    
    /**
     * Setup underwater environment (buoyancy + drag)
     */
    static void setupUnderwaterEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies);
    
    /**
     * Setup windy environment
     */
    static void setupWindyEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies, 
                                     const glm::vec3& windDirection = glm::vec3(1.0f, 0.0f, 0.0f));
    
    /**
     * Setup game physics (balanced for fun gameplay)
     */
    static void setupGamePhysics(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies);
};

} // namespace forces
} // namespace physics
} // namespace ohao