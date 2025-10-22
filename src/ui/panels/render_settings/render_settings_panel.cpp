#include "render_settings_panel.hpp"
#include <imgui.h>
#include <imgui_internal.h>

namespace ohao {

RenderSettingsPanel::RenderSettingsPanel()
    : PanelBase("Render Settings") {
}

void RenderSettingsPanel::render() {
    if (!visible) return;

    // Note: When used with SidePanelManager, Begin/End is handled externally
    // But we keep it for standalone compatibility
    bool shouldRenderContent = true;

    // Check if we're in a docked/child window context
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool isInChildWindow = (window && window->ParentWindow != nullptr);

    if (!isInChildWindow) {
        shouldRenderContent = ImGui::Begin(name.c_str(), &visible, windowFlags);
    }

    if (shouldRenderContent) {
        // Shadows Section
        if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Shadows", &m_enableShadows);

            if (m_enableShadows) {
                ImGui::Indent();

                // Shadow resolution
                const char* resolutionLabels[] = { "512", "1024", "2048", "4096" };
                int resolutionValues[] = { 512, 1024, 2048, 4096 };
                int currentResIndex = 2; // Default 2048

                for (int i = 0; i < 4; i++) {
                    if (m_shadowResolution == resolutionValues[i]) {
                        currentResIndex = i;
                        break;
                    }
                }

                if (ImGui::Combo("Resolution##Shadow", &currentResIndex, resolutionLabels, 4)) {
                    m_shadowResolution = resolutionValues[currentResIndex];
                }

                ImGui::SliderFloat("Shadow Bias", &m_shadowBias, 0.0f, 0.01f, "%.4f");

                ImGui::Unindent();
            }
        }

        ImGui::Spacing();

        // Post-Processing Section
        if (ImGui::CollapsingHeader("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("SSAO (Ambient Occlusion)", &m_enableSSAO);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Screen Space Ambient Occlusion");
            }

            ImGui::Checkbox("Bloom", &m_enableBloom);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Glow effect for bright areas");
            }

            ImGui::Checkbox("HDR", &m_enableHDR);
            if (m_enableHDR) {
                ImGui::Indent();
                ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 5.0f);
                ImGui::Unindent();
            }
        }

        ImGui::Spacing();

        // Anti-Aliasing Section
        if (ImGui::CollapsingHeader("Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable MSAA", &m_enableAntiAliasing);

            if (m_enableAntiAliasing) {
                ImGui::Indent();

                const char* msaaLabels[] = { "2x", "4x", "8x", "16x" };
                int msaaValues[] = { 2, 4, 8, 16 };
                int currentMSAAIndex = 1; // Default 4x

                for (int i = 0; i < 4; i++) {
                    if (m_msaaSamples == msaaValues[i]) {
                        currentMSAAIndex = i;
                        break;
                    }
                }

                if (ImGui::Combo("MSAA Samples", &currentMSAAIndex, msaaLabels, 4)) {
                    m_msaaSamples = msaaValues[currentMSAAIndex];
                }

                ImGui::Unindent();
            }
        }

        ImGui::Spacing();

        // Performance Info
        if (ImGui::CollapsingHeader("Performance")) {
            ImGui::TextDisabled("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::TextDisabled("Frame Time: %.2f ms", 1000.0f / ImGui::GetIO().Framerate);
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

} // namespace ohao
