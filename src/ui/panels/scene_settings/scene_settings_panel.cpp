#include "scene_settings_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"

namespace ohao {

void SceneSettingsPanel::render() {
    if (!visible) return;

    // Check if we're in a docked/child window context (used by SidePanelManager)
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool isInChildWindow = (window && window->ParentWindow != nullptr);

    bool shouldRenderContent = true;

    if (!isInChildWindow) {
        shouldRenderContent = ImGui::Begin(name.c_str(), &visible, windowFlags);
    }

    if (shouldRenderContent) {
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            renderEnvironmentSettings();
        }

        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            renderLightingSettings();
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

void SceneSettingsPanel::renderEnvironmentSettings() {
    static float ambientIntensity = 0.1f;
    static ImVec4 ambientColor = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    static float fogDensity = 0.0f;
    static ImVec4 fogColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

    ImGui::Text("Ambient Light");
    ImGui::SliderFloat("Intensity##Ambient", &ambientIntensity, 0.0f, 1.0f);
    ImGui::ColorEdit3("Color##Ambient", (float*)&ambientColor);

    ImGui::Separator();

    ImGui::Text("Fog");
    ImGui::SliderFloat("Density##Fog", &fogDensity, 0.0f, 1.0f);
    ImGui::ColorEdit3("Color##Fog", (float*)&fogColor);
}

void SceneSettingsPanel::renderLightingSettings() {
    static float directionalIntensity = 1.0f;
    static ImVec4 directionalColor = ImVec4(1.0f, 1.0f, 0.95f, 1.0f);
    static ImVec4 skyColor = ImVec4(0.5f, 0.7f, 1.0f, 1.0f);

    ImGui::Text("Directional Light");
    ImGui::SliderFloat("Intensity##Directional", &directionalIntensity, 0.0f, 2.0f);
    ImGui::ColorEdit3("Color##Directional", (float*)&directionalColor);

    ImGui::Separator();

    ImGui::Text("Sky");
    ImGui::ColorEdit3("Sky Color", (float*)&skyColor);
}

} // namespace ohao
