#pragma once

#include "ui/common/panel_base.hpp"
#include "physics/world/physics_world.hpp"
#include <memory>

namespace ohao {

// Forward declaration
class Scene;

class PhysicsPanel : public PanelBase {
public:
    PhysicsPanel();
    ~PhysicsPanel() override = default;
    
    void render() override;
    void setPhysicsWorld(physics::PhysicsWorld* world);
    void setScene(Scene* scene) { m_scene = scene; }
    
    // Physics state getters for external systems (replacing toolbar)
    physics::SimulationState getPhysicsState() const { return m_simulationState; }
    float getSimulationSpeed() const { return m_simulationSpeed; }
    bool isPhysicsEnabled() const { return m_physicsEnabled; }

    // Frame tracking for stepping system
    void incrementFrame() { m_currentFrame++; }
    int getCurrentFrame() const { return m_currentFrame; }

private:
    void renderPlaybackControls();
    void renderSimulationSettings();
    void renderWorldSettings();
    void renderDebugTools();
    void renderPerformanceStats();
    
    // References
    physics::PhysicsWorld* m_physicsWorld = nullptr;
    Scene* m_scene = nullptr;
    
    // Physics state (migrated from toolbar)
    physics::SimulationState m_simulationState = physics::SimulationState::STOPPED;  // Start stopped by default
    float m_simulationSpeed = 1.0f;
    bool m_physicsEnabled = true;
    
    // Settings
    glm::vec3 m_gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    int m_solverIterations = 10;
    bool m_useFixedTimestep = true;
    float m_fixedTimestep = 1.0f / 60.0f;
    
    // Debug
    bool m_showAABBs = false;
    bool m_showContacts = false;
    bool m_showForces = false;

    // Stepping system
    int m_currentFrame = 0;
    bool m_singleStepMode = false;

    // UI
    bool m_resetConfirmation = false;
};

} // namespace ohao