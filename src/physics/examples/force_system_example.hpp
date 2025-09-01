#pragma once

/**
 * Example usage of the OHAO Physics Force System
 * 
 * This file demonstrates how to use the comprehensive force system
 * that has been implemented for rigid body physics simulation.
 */

#include "physics/world/physics_world.hpp"
#include "physics/forces/forces.hpp"
#include "physics/debug/force_debugger.hpp"
#include <memory>
#include <vector>

namespace ohao {
namespace examples {

class ForceSystemExample {
public:
    void setupBasicPhysicsWorld() {
        // Create physics world with default configuration
        auto config = physics::PhysicsWorldConfig{};
        config.enableDebugVisualization = true;
        config.enableStatistics = true;
        
        m_physicsWorld = std::make_unique<physics::PhysicsWorld>(config);
        
        // Enable force debugging for visualization and analysis
        m_physicsWorld->enableForceDebugging(true);
        
        // Setup Earth-like environment (gravity + air resistance)
        m_physicsWorld->setupEarthEnvironment();
    }
    
    void addCustomForces() {
        // Get all bodies for force targeting
        std::vector<physics::dynamics::RigidBody*> allBodies;
        // In real usage, you'd populate this from your rigid bodies
        
        auto& forceRegistry = m_physicsWorld->getForceRegistry();
        
        // Add a wind force affecting all objects
        auto windForce = physics::forces::ForceFactory::createWind(
            glm::vec3(1.0f, 0.0f, 0.0f),  // Direction (right)
            15.0f,                         // Strength
            0.2f                          // Turbulence
        );
        forceRegistry.registerForce(std::move(windForce), \"environmental_wind\", allBodies);
        
        // Add explosion force at a specific location
        auto explosionForce = physics::forces::ForceFactory::createExplosion(
            glm::vec3(0.0f, 0.0f, 0.0f),  // Center
            1000.0f,                       // Force magnitude
            10.0f                         // Radius
        );
        forceRegistry.registerForce(std::move(explosionForce), \"explosion_demo\");
        
        // Add spring between two specific bodies (if available)
        if (allBodies.size() >= 2) {
            auto springForce = physics::forces::ForceFactory::createSpring(
                allBodies[0], allBodies[1],  // Connected bodies
                50.0f,                       // Spring constant
                2.0f,                        // Rest length
                5.0f                         // Damping
            );
            forceRegistry.registerForce(std::move(springForce), \"connection_spring\");
        }
    }
    
    void simulationStep(float deltaTime) {
        // Step the physics simulation
        m_physicsWorld->step(deltaTime);
        
        // Access force debugging information
        if (m_physicsWorld->isForceDebuggingEnabled()) {
            auto* debugger = m_physicsWorld->getForceDebugger();
            
            // Get force vectors for visualization
            const auto& forceVectors = debugger->getForceVectors();
            const auto& torqueVectors = debugger->getTorqueVectors();
            
            // Get statistics for analysis
            const auto& frameStats = debugger->getFrameStats();
            
            // Log statistics every few frames
            if (m_frameCounter % 60 == 0) {  // Every second at 60 FPS
                debugger->logForceStatistics();
            }
            
            // Generate detailed report if needed
            if (m_frameCounter % 600 == 0) {  // Every 10 seconds
                std::string report = debugger->generateForceReport();
                debugger->saveForceReport(\"force_report.txt\");
            }
        }
        
        m_frameCounter++;
    }
    
    void demonstrateForcePresets() {
        // Different environment presets
        switch (m_currentPreset) {
            case 0:
                m_physicsWorld->setupEarthEnvironment();
                break;
            case 1:
                m_physicsWorld->setupSpaceEnvironment();
                break;
            case 2:
                m_physicsWorld->setupUnderwaterEnvironment();
                break;
            case 3:
                m_physicsWorld->setupGamePhysics();
                break;
        }
    }
    
    void demonstrateForceDebugging() {
        auto* debugger = m_physicsWorld->getForceDebugger();
        if (!debugger) return;
        
        // Configure visualization
        debugger->setVisualizationMode(physics::debug::ForceDebugger::VisualizationMode::ALL_FORCES);
        debugger->setForceScale(0.1f);              // Scale force vectors for visibility
        debugger->setMinimumMagnitudeThreshold(1.0f); // Filter out small forces
        debugger->setShowTorques(true);
        debugger->setShowForceLabels(true);
        
        // Set custom colors for different force types
        debugger->setForceTypeColor(\"gravity\", glm::vec3(1.0f, 1.0f, 0.0f));    // Yellow
        debugger->setForceTypeColor(\"wind\", glm::vec3(0.0f, 0.8f, 1.0f));       // Light blue
        debugger->setForceTypeColor(\"explosion\", glm::vec3(1.0f, 0.0f, 0.0f));  // Red
        
        // Enable performance profiling
        debugger->setProfilingEnabled(true);
        
        // The debugger will automatically collect data during simulation steps
    }
    
    void demonstrateAdvancedForces() {
        auto& forceRegistry = m_physicsWorld->getForceRegistry();
        
        // Vortex force (tornado effect)
        auto vortexForce = physics::forces::ForceFactory::createVortex(
            glm::vec3(5.0f, 0.0f, 0.0f),    // Center
            glm::vec3(0.0f, 1.0f, 0.0f),    // Axis (up)
            200.0f,                          // Strength
            8.0f                            // Radius
        );
        forceRegistry.registerForce(std::move(vortexForce), \"tornado_effect\");
        
        // Buoyancy for underwater simulation
        auto buoyancyForce = physics::forces::ForceFactory::createBuoyancy(
            1000.0f,                        // Fluid density (water)
            0.0f                           // Liquid surface level
        );
        forceRegistry.registerForce(std::move(buoyancyForce), \"water_buoyancy\");
        
        // Turbulence for chaotic motion
        auto turbulence = physics::forces::ForceFactory::createTurbulence(
            25.0f,                          // Intensity
            2.0f                           // Frequency
        );
        forceRegistry.registerForce(std::move(turbulence), \"chaos_generator\");
    }
    
    void cleanup() {
        m_physicsWorld.reset();
    }

private:
    std::unique_ptr<physics::PhysicsWorld> m_physicsWorld;
    int m_frameCounter = 0;
    int m_currentPreset = 0;
};

} // namespace examples
} // namespace ohao