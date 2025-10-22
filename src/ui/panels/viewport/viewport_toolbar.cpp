#include "viewport_toolbar.hpp"
#include "imgui.h"
#include "renderer/vulkan_context.hpp"

namespace ohao {

ViewportToolbar::ViewportToolbar() : PanelBase("Viewport Toolbar") {
    // Set up as a modern floating toolbar
    windowFlags = ImGuiWindowFlags_NoCollapse | 
                  ImGuiWindowFlags_NoResize | 
                  ImGuiWindowFlags_NoTitleBar | 
                  ImGuiWindowFlags_AlwaysAutoResize |
                  ImGuiWindowFlags_NoBackground |
                  ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoScrollWithMouse;
}

void ViewportToolbar::render() {
    if (!visible) return;

    // Position the toolbar in the top-left of the viewport with better positioning
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 toolbarPos = ImVec2(viewport->Pos.x + 20, viewport->Pos.y + 60);
    ImGui::SetNextWindowPos(toolbarPos, ImGuiCond_Always);
    
    // Modern professional styling with better padding for icon visibility
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));     // More padding to prevent clipping
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));        // More spacing between icons
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    
    // Professional dark theme colors
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.28f, 0.28f, 0.28f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.25f, 0.8f));
    
    if (ImGui::Begin("##ViewportToolbar", &visible, windowFlags)) {
        // Only render visual aid controls now
        renderModernVisualAidControls();
    }
    ImGui::End();
    
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(6);
}

void ViewportToolbar::renderModernVisualAidControls() {
    // Visual aids section header (small text label)
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.67f, 1.0f));
    ImGui::TextUnformatted(ICON_VIEW);
    ImGui::PopStyleColor();

    ImGui::Separator();
    ImGui::Spacing();

    const float toggleSize = 42.0f;  // Larger size for clear icon display

    // Axis gizmo toggle with FontAwesome icon
    renderModernToggleButton(ICON_AXIS, showAxis, toggleSize,
        ImVec4(0.28f, 0.65f, 0.95f, 1.0f), // Active - vibrant blue
        "Toggle XYZ axis gizmo");

    ImGui::SameLine(0.0f, 8.0f);  // More spacing between icons

    // Grid toggle with FontAwesome icon
    renderModernToggleButton(ICON_GRID, showGrid, toggleSize,
        ImVec4(0.70f, 0.40f, 0.95f, 1.0f), // Active - purple
        "Toggle ground grid");

    ImGui::SameLine(0.0f, 8.0f);  // More spacing between icons

    // Wireframe toggle with FontAwesome icon
    renderModernToggleButton(ICON_WIREFRAME, wireframeMode, toggleSize,
        ImVec4(0.95f, 0.60f, 0.20f, 1.0f), // Active - orange
        "Toggle wireframe mode");

    // Apply changes to the systems
    applyVisualAidSettings();
}

// === Modern UI Helper Methods ===

void ViewportToolbar::renderModernButton(const char* icon, bool isActive, 
                                       const ImVec4& activeColor, const ImVec4& inactiveColor,
                                       float size, const char* tooltip) {
    ImVec4 buttonColor = isActive ? activeColor : inactiveColor;
    ImVec4 hoverColor = isActive ? 
        ImVec4(activeColor.x + 0.1f, activeColor.y + 0.1f, activeColor.z + 0.1f, activeColor.w) :
        ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    ImVec4 pressColor = isActive ? 
        ImVec4(activeColor.x - 0.1f, activeColor.y - 0.1f, activeColor.z - 0.1f, activeColor.w) :
        ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    
    ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, pressColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    
    ImGui::Button(icon, ImVec2(size, size));
    
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void ViewportToolbar::renderModernToggleButton(const char* icon, bool& toggle, float size,
                                             const ImVec4& activeColor, const char* tooltip) {
    ImVec4 buttonColor = toggle ? activeColor : ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
    ImVec4 hoverColor = toggle ? 
        ImVec4(activeColor.x + 0.1f, activeColor.y + 0.1f, activeColor.z + 0.1f, activeColor.w) :
        ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    
    ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, buttonColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    
    if (ImGui::Button(icon, ImVec2(size, size))) {
        toggle = !toggle;
        printf("Toggle %s: %s\n", icon, toggle ? "ON" : "OFF");
    }
    
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void ViewportToolbar::renderModernCheckbox(const char* id, bool* value, const char* tooltip) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.4f, 0.75f, 0.4f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    
    ImGui::Checkbox(id, value);
    
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void ViewportToolbar::renderSectionSeparator() {
    ImGui::SameLine();
    // Create a vertical separator using text
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    ImGui::Text("|");
    ImGui::PopStyleColor();
    ImGui::SameLine();
}

void ViewportToolbar::applyVisualAidSettings() {
    // Apply changes to the systems (same logic as original)
    if (axisGizmo) {
        axisGizmo->setVisible(showAxis);
        axisGizmo->setGridVisible(showGrid);
        hasInitializedGizmo = true;
    } else {
        // Try to reconnect to axis gizmo if not connected yet
        if (auto* vulkanContext = VulkanContext::getContextInstance()) {
            if (auto* gizmo = vulkanContext->getAxisGizmo()) {
                axisGizmo = gizmo;
                axisGizmo->setVisible(showAxis);
                axisGizmo->setGridVisible(showGrid);
                hasInitializedGizmo = true;
                printf("Axis gizmo connected and initialized!\n");
            }
        }
    }
    
    // Force initial sync on first connection
    if (axisGizmo && !hasInitializedGizmo) {
        axisGizmo->setVisible(showAxis);
        axisGizmo->setGridVisible(showGrid);
        hasInitializedGizmo = true;
        printf("Axis gizmo force synced!\n");
    }
    
    // Apply wireframe mode to Vulkan context
    if (auto* vulkanContext = VulkanContext::getContextInstance()) {
        vulkanContext->setWireframeMode(wireframeMode);
    }
}

} // namespace ohao