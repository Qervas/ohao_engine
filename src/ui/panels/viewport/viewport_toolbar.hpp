#pragma once
#include "ui/common/panel_base.hpp"
#include "renderer/gizmo/axis_gizmo.hpp"
#include <memory>

namespace ohao {

class AxisGizmo;

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
    void renderToolbarButton(const char* label, bool& toggle, const char* tooltip = nullptr);
    
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