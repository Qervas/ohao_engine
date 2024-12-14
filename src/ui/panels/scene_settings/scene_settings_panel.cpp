#include "scene_settings_panel.hpp"
#include "imgui.h"

namespace ohao {

void SceneSettingsPanel::render() {
    if (!visible) return;

    ImGui::Begin(name.c_str(), &visible, windowFlags);

    if (ImGui::CollapsingHeader("Environment")) {
        renderEnvironmentSettings();
    }

    if (ImGui::CollapsingHeader("Render Settings")) {
        renderRenderSettings();
    }

    if (ImGui::CollapsingHeader("Physics Settings")) {
        renderPhysicsSettings();
    }

    ImGui::End();
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

void SceneSettingsPanel::renderRenderSettings() {
    static bool enableShadows = true;
    static int shadowResolution = 2048;
    static float shadowBias = 0.005f;
    static bool enableSSAO = false;
    static bool enableBloom = false;

    ImGui::Checkbox("Enable Shadows", &enableShadows);
    if (enableShadows) {
        ImGui::Indent();
        ImGui::SliderInt("Shadow Resolution", &shadowResolution, 512, 4096);
        ImGui::SliderFloat("Shadow Bias", &shadowBias, 0.0f, 0.01f);
        ImGui::Unindent();
    }

    ImGui::Checkbox("Enable SSAO", &enableSSAO);
    ImGui::Checkbox("Enable Bloom", &enableBloom);
}

void SceneSettingsPanel::renderPhysicsSettings() {
    static bool enablePhysics = true;
    static float gravity = -9.81f;
    static int substeps = 2;
    static float fixedTimeStep = 1.0f / 60.0f;

    ImGui::Checkbox("Enable Physics", &enablePhysics);
    if (enablePhysics) {
        ImGui::Indent();
        ImGui::DragFloat("Gravity", &gravity, 0.1f, -20.0f, 20.0f);
        ImGui::SliderInt("Substeps", &substeps, 1, 10);
        ImGui::DragFloat("Fixed Timestep", &fixedTimeStep, 0.001f, 0.001f, 0.1f);
        ImGui::Unindent();
    }
}

} // namespace ohao
