#pragma once
#include "ui/common/panel_base.hpp"
#include "renderer/gizmo/axis_gizmo.hpp"
#include "core/physics/world/simulation_state.hpp"
#include <memory>
#include "imgui.h"

namespace ohao {

class AxisGizmo;
class PhysicsWorld;

// Use the new physics simulation state enum
using PhysicsSimulationState = physics::SimulationState;

// Modern UI Icons using ASCII and basic Unicode symbols
#define ICON_PHYSICS "PHY"
#define ICON_PLAY " > "
#define ICON_PAUSE "||"
#define ICON_STOP "[]"
#define ICON_VIEW "VIEW"
#define ICON_AXIS " X "
#define ICON_GRID " # "
#define ICON_WIREFRAME " <> "
#define ICON_STATUS_RUNNING "*"
#define ICON_STATUS_PAUSED "-"
#define ICON_STATUS_STOPPED "o"

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
    // Modern UI rendering methods
    void renderModernPhysicsControls();
    void renderModernVisualAidControls();
    
    // Modern UI helper methods
    void renderModernButton(const char* icon, bool isActive, 
                          const ImVec4& activeColor, const ImVec4& inactiveColor,
                          float size, const char* tooltip);
    void renderModernToggleButton(const char* icon, bool& toggle, float size,
                                const ImVec4& activeColor, const char* tooltip);
    void renderSpeedPresetButton(const char* label, float speed);
    void renderModernCheckbox(const char* id, bool* value, const char* tooltip);
    void renderPhysicsStatusIndicator();
    void renderSectionSeparator();
    void applyVisualAidSettings();
    
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