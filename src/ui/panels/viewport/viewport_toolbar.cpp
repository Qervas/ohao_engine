#include "viewport_toolbar.hpp"
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
    
    // Style the toolbar with a subtle background
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 0.8f));
    
    if (ImGui::Begin("##ViewportToolbar", &visible, windowFlags)) {
        // Horizontal layout for toolbar buttons
        ImGui::Text("Visual Aids");
        ImGui::Separator();
        
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
    ImGui::End();
    
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
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
        // Debug output to verify toggle functionality
        printf("Toolbar toggle %s: %s\n", label, toggle ? "ON" : "OFF");
    }
    
    // Show tooltip on hover
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImGui::PopStyleColor(3);
}

} // namespace ohao