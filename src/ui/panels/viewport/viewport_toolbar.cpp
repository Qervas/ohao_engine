#include "viewport_toolbar.hpp"
#include "core/physics/world/physics_world.hpp"
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
    
    // Modern professional styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
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
        // Create horizontal layout with sections
        renderModernPhysicsControls();
        
        // Elegant separator
        renderSectionSeparator();
        
        // Visual Aid Controls  
        renderModernVisualAidControls();
    }
    ImGui::End();
    
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(6);
}

void ViewportToolbar::renderModernPhysicsControls() {
    // Physics section with subtle header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::Text(ICON_PHYSICS " Physics");
    ImGui::PopStyleColor();
    
    // Create horizontal layout for control buttons
    const float buttonSize = 36.0f;
    const float iconSize = 16.0f;
    
    // Play button with modern icon
    bool isPlaying = (physicsState == PhysicsSimulationState::RUNNING);
    renderModernButton(ICON_PLAY, isPlaying, 
        ImVec4(0.15f, 0.75f, 0.15f, 1.0f), // Active color - vibrant green
        ImVec4(0.12f, 0.12f, 0.12f, 1.0f), // Inactive color - dark
        buttonSize, "Start physics simulation");
    
    if (ImGui::IsItemClicked() && !isPlaying) {
        physicsState = PhysicsSimulationState::RUNNING;
        printf("Physics simulation: PLAYING\n");
    }
    
    ImGui::SameLine(0.0f, 4.0f);
    
    // Pause button
    bool isPaused = (physicsState == PhysicsSimulationState::PAUSED);
    renderModernButton(ICON_PAUSE, isPaused,
        ImVec4(0.85f, 0.65f, 0.15f, 1.0f), // Active color - amber
        ImVec4(0.12f, 0.12f, 0.12f, 1.0f), // Inactive color
        buttonSize, "Pause physics simulation");
    
    if (ImGui::IsItemClicked() && !isPaused) {
        physicsState = PhysicsSimulationState::PAUSED;
        printf("Physics simulation: PAUSED\n");
    }
    
    ImGui::SameLine(0.0f, 4.0f);
    
    // Stop button (keep disabled for testing as in original)
    bool isStopped = (physicsState == PhysicsSimulationState::STOPPED);
    ImGui::BeginDisabled(true);
    renderModernButton(ICON_STOP, isStopped,
        ImVec4(0.85f, 0.25f, 0.25f, 1.0f), // Active color - red
        ImVec4(0.12f, 0.12f, 0.12f, 1.0f), // Inactive color
        buttonSize, "Stop physics simulation (temporarily disabled)");
    ImGui::EndDisabled();
    
    ImGui::SameLine(0.0f, 12.0f); // Extra space before speed controls
    
    // Speed control with modern styling
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
    ImGui::Text("%.1fx", simulationSpeed);
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.65f, 0.95f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
    if (ImGui::SliderFloat("##Speed", &simulationSpeed, 0.1f, 3.0f, "")) {
        printf("Physics speed: %.1fx\n", simulationSpeed);
    }
    ImGui::PopStyleColor(3);
    
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Simulation Speed\n0.1x - Slow motion\n1.0x - Normal speed\n3.0x - Fast forward");
    }
    
    // Quick speed presets with modern mini buttons
    ImGui::SameLine();
    renderSpeedPresetButton("0.5x", 0.5f);
    ImGui::SameLine(0.0f, 2.0f);
    renderSpeedPresetButton("1x", 1.0f);  
    ImGui::SameLine(0.0f, 2.0f);
    renderSpeedPresetButton("2x", 2.0f);
    
    // Physics enabled toggle with modern styling
    ImGui::SameLine(0.0f, 8.0f);
    renderModernCheckbox("##PhysicsEnabled", &physicsEnabled, "Enable/disable physics simulation");
    
    // Compact status indicator
    ImGui::SameLine();
    renderPhysicsStatusIndicator();
}

void ViewportToolbar::renderModernVisualAidControls() {
    // Visual aids section header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::Text(ICON_VIEW " View");
    ImGui::PopStyleColor();
    
    const float toggleSize = 32.0f;
    
    // Axis gizmo toggle with modern icon
    renderModernToggleButton(ICON_AXIS, showAxis, toggleSize, 
        ImVec4(0.25f, 0.65f, 0.95f, 1.0f), // Active - blue
        "Toggle XYZ axis gizmo with ruler markings");
    
    ImGui::SameLine(0.0f, 4.0f);
    
    // Grid toggle
    renderModernToggleButton(ICON_GRID, showGrid, toggleSize,
        ImVec4(0.65f, 0.35f, 0.95f, 1.0f), // Active - purple  
        "Toggle XOY plane grid");
    
    ImGui::SameLine(0.0f, 4.0f);
    
    // Wireframe toggle
    renderModernToggleButton(ICON_WIREFRAME, wireframeMode, toggleSize,
        ImVec4(0.95f, 0.55f, 0.15f, 1.0f), // Active - orange
        "Toggle wireframe rendering mode");
    
    // Apply changes to the systems (same logic as before)
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

void ViewportToolbar::renderSpeedPresetButton(const char* label, float speed) {
    bool isActive = (fabs(simulationSpeed - speed) < 0.01f);
    ImVec4 color = isActive ? ImVec4(0.4f, 0.65f, 0.95f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x + 0.1f, color.y + 0.1f, color.z + 0.1f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    
    if (ImGui::Button(label, ImVec2(28, 20))) {
        simulationSpeed = speed;
    }
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
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

void ViewportToolbar::renderPhysicsStatusIndicator() {
    const char* icon = "";
    ImVec4 color;
    
    switch (physicsState) {
        case PhysicsSimulationState::RUNNING:
            icon = ICON_STATUS_RUNNING;
            color = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            break;
        case PhysicsSimulationState::PAUSED:
            icon = ICON_STATUS_PAUSED;
            color = ImVec4(0.8f, 0.6f, 0.2f, 1.0f);
            break;
        case PhysicsSimulationState::STOPPED:
            icon = ICON_STATUS_STOPPED;
            color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            break;
    }
    
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("%s", icon);
    ImGui::PopStyleColor();
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