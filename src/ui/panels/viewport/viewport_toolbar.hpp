#pragma once
#include "ui/common/panel_base.hpp"
#include "renderer/gizmo/axis_gizmo.hpp"
#include <memory>
#include "imgui.h"

namespace ohao {

class AxisGizmo;

// Modern UI Icons using ASCII and basic Unicode symbols
#define ICON_VIEW "VIEW"
#define ICON_AXIS " X "
#define ICON_GRID " # "
#define ICON_WIREFRAME " <> "

class ViewportToolbar : public PanelBase {
public:
    ViewportToolbar();
    void render() override;
    
    // Connect to the axis gizmo system
    void setAxisGizmo(AxisGizmo* gizmo) { axisGizmo = gizmo; }
    
    // Visual aid toggles
    bool isAxisVisible() const { return showAxis; }
    bool isGridVisible() const { return showGrid; }
    bool isWireframeEnabled() const { return wireframeMode; }
    
    void setAxisVisible(bool visible) { showAxis = visible; }
    void setGridVisible(bool visible) { showGrid = visible; }
    void setWireframeEnabled(bool enabled) { wireframeMode = enabled; }

private:
    // Modern UI rendering methods
    void renderModernVisualAidControls();
    
    // Modern UI helper methods
    void renderModernButton(const char* icon, bool isActive, 
                          const ImVec4& activeColor, const ImVec4& inactiveColor,
                          float size, const char* tooltip);
    void renderModernToggleButton(const char* icon, bool& toggle, float size,
                                const ImVec4& activeColor, const char* tooltip);
    void renderModernCheckbox(const char* id, bool* value, const char* tooltip);
    void renderSectionSeparator();
    void applyVisualAidSettings();
    
    AxisGizmo* axisGizmo = nullptr;
    
    // Visual aid states
    bool showAxis = true;
    bool showGrid = true;
    bool wireframeMode = false;
    bool hasInitializedGizmo = false;
    
    // UI state
    const float buttonSize = 32.0f;
    const float spacing = 4.0f;
};

} // namespace ohao