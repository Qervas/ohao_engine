#pragma once
#include "ui/common/panel_base.hpp"
#include "renderer/gizmo/axis_gizmo.hpp"
#include "ui/icons/font_awesome_icons.hpp"
#include <memory>
#include "imgui.h"

namespace ohao {

class AxisGizmo;

// Viewport Toolbar Icons using FontAwesome
#define ICON_VIEW "View"  // Header text, not an icon
#define ICON_PLAY ICON_FA_PLAY                       // Play/focus icon
#define ICON_AXIS ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT  // 3D axis gizmo icon
#define ICON_GRID ICON_FA_BORDER_ALL                 // Grid pattern icon
#define ICON_WIREFRAME ICON_FA_DRAW_POLYGON          // Wireframe/polygon icon

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

    // Viewport focus control
    bool isViewportFocused() const { return viewportFocused; }
    void setViewportFocused(bool focused) { viewportFocused = focused; }

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

    // Viewport focus state
    bool viewportFocused = false;

    // UI state
    const float buttonSize = 32.0f;
    const float spacing = 4.0f;
};

} // namespace ohao