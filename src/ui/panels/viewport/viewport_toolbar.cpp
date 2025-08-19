#include "viewport_toolbar.hpp"
#include "core/physics/physics_world.hpp"
#include "imgui.h"
#include "renderer/vulkan_context.hpp"

namespace ohao {

ViewportToolbar::ViewportToolbar() : PanelBase("Viewport Toolbar") {
    // Set up as a floating toolbar
    windowFlags = ImGuiWindowFlags_NoCollapse | 
                  ImGuiWindowFlags_NoResize | 
                  ImGuiWindowFlags_NoTitleBar | 
                  ImGuiWindowFlags_AlwaysAutoResize |
                  ImGuiWindowFlags_NoBackground;
}

void ViewportToolbar::render() {
    if (!visible) return;

    // Position the toolbar in the top-left of the viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 toolbarPos = ImVec2(viewport->Pos.x + 10, viewport->Pos.y + 30);
    ImGui::SetNextWindowPos(toolbarPos, ImGuiCond_Always);
    
    // Style the toolbar with a modern look
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 0.8f));
    
    if (ImGui::Begin("##ViewportToolbar", &visible, windowFlags)) {
        // Physics Simulation Controls
        renderPhysicsControls();
        
        ImGui::Separator();
        
        // Visual Aid Controls  
        renderVisualAidControls();
    }
    ImGui::End();
    
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void ViewportToolbar::renderPhysicsControls() {
    ImGui::Text("[PHYSICS] Simulation");
    
    // Play/Pause/Stop buttons
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, spacing));
    
    // Play button
    bool isPlaying = (physicsState == PhysicsSimulationState::PLAYING);
    if (isPlaying) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.9f, 0.3f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.7f, 0.1f, 0.8f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    }
    
    if (ImGui::Button("PLAY", ImVec2(buttonSize + 10, buttonSize))) {
        physicsState = PhysicsSimulationState::PLAYING;
        printf("Physics simulation: PLAYING\n");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Start physics simulation");
    }
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine();
    
    // Pause button
    bool isPaused = (physicsState == PhysicsSimulationState::PAUSED);
    if (isPaused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.7f, 0.3f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.5f, 0.1f, 0.8f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    }
    
    if (ImGui::Button("PAUSE", ImVec2(buttonSize + 10, buttonSize))) {
        physicsState = PhysicsSimulationState::PAUSED;
        printf("Physics simulation: PAUSED\n");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pause physics simulation");
    }
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine();
    
    // Stop button
    bool isStopped = (physicsState == PhysicsSimulationState::STOPPED);
    if (isStopped) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    }
    
    if (ImGui::Button("STOP", ImVec2(buttonSize + 10, buttonSize))) {
        physicsState = PhysicsSimulationState::STOPPED;
        printf("Physics simulation: STOPPED\n");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stop and reset physics simulation");
    }
    ImGui::PopStyleColor(3);
    
    ImGui::PopStyleVar();
    
    // Speed control slider
    ImGui::Text("Speed: %.1fx", simulationSpeed);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderFloat("##Speed", &simulationSpeed, 0.1f, 3.0f, "%.1fx")) {
        printf("Physics speed: %.1fx\n", simulationSpeed);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Adjust simulation speed\n0.1x = Slow motion\n1.0x = Normal speed\n3.0x = Fast forward");
    }
    
    // Quick speed buttons
    ImGui::SameLine();
    if (ImGui::Button("0.5x", ImVec2(35, 20))) simulationSpeed = 0.5f;
    ImGui::SameLine();
    if (ImGui::Button("1x", ImVec2(25, 20))) simulationSpeed = 1.0f;
    ImGui::SameLine();
    if (ImGui::Button("2x", ImVec2(25, 20))) simulationSpeed = 2.0f;
    
    // Physics enabled toggle
    ImGui::Checkbox("Physics Enabled", &physicsEnabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable/disable physics simulation globally");
    }
    
    // Status display
    const char* statusText = "";
    ImVec4 statusColor;
    switch (physicsState) {
        case PhysicsSimulationState::PLAYING:
            statusText = "[RUNNING]";
            statusColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            break;
        case PhysicsSimulationState::PAUSED:
            statusText = "[PAUSED]";
            statusColor = ImVec4(0.8f, 0.6f, 0.2f, 1.0f);
            break;
        case PhysicsSimulationState::STOPPED:
            statusText = "[STOPPED]";
            statusColor = ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
            break;
    }
    
    ImGui::TextColored(statusColor, "%s", statusText);
}

void ViewportToolbar::renderVisualAidControls() {
    ImGui::Text("[VIEW] Visual Aids");
    
    // Axis gizmo toggle
    renderToolbarButton("Axis", showAxis, "Toggle XYZ axis gizmo with ruler markings");
    
    ImGui::SameLine();
    
    // Grid toggle
    renderToolbarButton("Grid", showGrid, "Toggle XOY plane grid");
    
    ImGui::SameLine();
    
    // Wireframe toggle
    renderToolbarButton("Wireframe", wireframeMode, "Toggle wireframe rendering mode");
    
    // Apply changes to the systems
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

void ViewportToolbar::renderToolbarButton(const char* label, bool& toggle, const char* tooltip) {
    // Style the button based on toggle state
    if (toggle) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.5f, 0.1f, 0.8f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    }
    
    if (ImGui::Button(label, ImVec2(buttonSize * 2, buttonSize))) {
        toggle = !toggle;
        printf("Toolbar toggle %s: %s\n", label, toggle ? "ON" : "OFF");
    }
    
    // Show tooltip on hover
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImGui::PopStyleColor(3);
}

} // namespace ohao