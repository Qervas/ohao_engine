#pragma once
#include "ui/common/panel_base.hpp"
#include "renderer/gizmo/axis_gizmo.hpp"
#include <memory>

namespace ohao {

class AxisGizmo;
class PhysicsWorld;

enum class PhysicsSimulationState {
    STOPPED,
    PLAYING,
    PAUSED
};

class ViewportToolbar : public PanelBase {
public:
    ViewportToolbar();
    void render() override;
    
    // Connect to the axis gizmo system
    void setAxisGizmo(AxisGizmo* gizmo) { axisGizmo = gizmo; }
    
    // Connect to physics world
    void setPhysicsWorld(PhysicsWorld* world) { physicsWorld = world; }
    
    // Visual aid toggles
    bool isAxisVisible() const { return showAxis; }
    bool isGridVisible() const { return showGrid; }
    bool isWireframeEnabled() const { return wireframeMode; }
    
    void setAxisVisible(bool visible) { showAxis = visible; }
    void setGridVisible(bool visible) { showGrid = visible; }
    void setWireframeEnabled(bool enabled) { wireframeMode = enabled; }
    
    // Physics simulation controls
    PhysicsSimulationState getPhysicsState() const { return physicsState; }
    float getSimulationSpeed() const { return simulationSpeed; }
    bool isPhysicsEnabled() const { return physicsEnabled; }

private:
    void renderToolbarButton(const char* label, bool& toggle, const char* tooltip = nullptr);
    void renderPhysicsControls();
    void renderVisualAidControls();
    
    AxisGizmo* axisGizmo = nullptr;
    PhysicsWorld* physicsWorld = nullptr;
    
    // Visual aid states
    bool showAxis = true;
    bool showGrid = true;
    bool wireframeMode = false;
    bool hasInitializedGizmo = false;
    
    // Physics simulation states
    PhysicsSimulationState physicsState = PhysicsSimulationState::STOPPED;
    float simulationSpeed = 1.0f;
    bool physicsEnabled = true;
    
    // UI state
    const float buttonSize = 32.0f;
    const float spacing = 4.0f;
};

} // namespace ohao