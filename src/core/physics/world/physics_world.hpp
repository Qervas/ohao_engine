#pragma once

#include "physics_settings.hpp"
#include "simulation_state.hpp"
#include "../dynamics/rigid_body.hpp"
#include "../collision/collision_detector.hpp"
#include "../collision/collision_resolver.hpp"
#include "../collision/contact_info.hpp"
#include <vector>
#include <memory>

// Forward declaration  
class PhysicsComponent;

namespace ohao {
namespace physics {

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    
    // === INITIALIZATION ===
    bool initialize(const PhysicsSettings& settings = PhysicsSettings{});
    void cleanup();
    bool isInitialized() const { return m_initialized; }
    
    // === SIMULATION CONTROL ===
    void stepSimulation(float deltaTime);
    void setSimulationState(SimulationState state) { m_simulationState = state; }
    SimulationState getSimulationState() const { return m_simulationState; }
    
    bool isRunning() const { return m_simulationState == SimulationState::RUNNING; }
    bool isPaused() const { return m_simulationState == SimulationState::PAUSED; }
    
    // === SETTINGS ===
    void setSettings(const PhysicsSettings& settings) { m_settings = settings; }
    const PhysicsSettings& getSettings() const { return m_settings; }
    
    void setGravity(const glm::vec3& gravity) { m_settings.gravity = gravity; }
    glm::vec3 getGravity() const { return m_settings.gravity; }
    
    // === RIGID BODY MANAGEMENT ===
    std::shared_ptr<dynamics::RigidBody> createRigidBody(PhysicsComponent* component);
    void removeRigidBody(std::shared_ptr<dynamics::RigidBody> body);
    void removeRigidBody(PhysicsComponent* component);
    
    size_t getRigidBodyCount() const { return m_rigidBodies.size(); }
    const std::vector<std::shared_ptr<dynamics::RigidBody>>& getRigidBodies() const { return m_rigidBodies; }
    
    // === RAYCASTING ===
    struct RaycastResult {
        bool hit{false};
        glm::vec3 hitPoint{0.0f};
        glm::vec3 hitNormal{0.0f};
        float distance{0.0f};
        dynamics::RigidBody* body{nullptr};
    };
    
    RaycastResult raycast(const glm::vec3& from, const glm::vec3& to);
    std::vector<RaycastResult> raycastAll(const glm::vec3& from, const glm::vec3& to);
    
    // === DEBUGGING ===
    void setDebugDrawEnabled(bool enabled) { m_debugDrawEnabled = enabled; }
    bool isDebugDrawEnabled() const { return m_debugDrawEnabled; }
    
    // Get debug statistics
    struct DebugStats {
        size_t numRigidBodies{0};
        size_t numActiveRigidBodies{0};
        size_t numCollisionPairs{0};
        size_t numContacts{0};
        float lastStepTime{0.0f};
        float averageStepTime{0.0f};
    };
    
    const DebugStats& getDebugStats() const { return m_debugStats; }
    
private:
    // === SIMULATION PHASES ===
    void applyGravity();
    void integrateForces(float deltaTime);
    void detectCollisions();
    void resolveCollisions(); 
    void integrateVelocities(float deltaTime);
    void updateSleepStates(float deltaTime);
    void syncWithComponents();
    
    // === UTILITY ===
    void updateDebugStats(float stepTime);
    void removeInvalidBodies();
    
    // === MEMBER VARIABLES ===
    PhysicsSettings m_settings;
    SimulationState m_simulationState{SimulationState::STOPPED};
    
    std::vector<std::shared_ptr<dynamics::RigidBody>> m_rigidBodies;
    std::vector<collision::ContactInfo> m_contacts;
    std::vector<std::pair<dynamics::RigidBody*, dynamics::RigidBody*>> m_contactPairs;
    
    bool m_initialized{false};
    bool m_debugDrawEnabled{false};
    
    DebugStats m_debugStats;
    float m_stepTimeAccumulator{0.0f};
    int m_stepCount{0};
};

} // namespace physics
} // namespace ohao