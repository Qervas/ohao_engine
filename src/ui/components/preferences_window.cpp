#include "preferences_window.hpp"
#include "console_widget.hpp"
#include "imgui.h"
#include <system/ui_manager.hpp>
#include <array>

namespace ohao {

void PreferencesWindow::render(bool* open) {
    if (!isWindowOpen) return;

    // Initialize temp preferences with current values when window opens
    if (!tempPrefsInitialized) {
        tempPrefs = Preferences::get().getAppearance();
        tempPrefsInitialized = true;
    }

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Preferences", &isWindowOpen)) {

        // Left side: Categories
        static int selected = 0;
        ImGui::BeginChild("categories", ImVec2(150, 0), true);
        if (ImGui::Selectable("Appearance", selected == 0)) selected = 0;
        ImGui::EndChild();

        ImGui::SameLine();

        // Right side: Settings
        ImGui::BeginChild("settings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        switch (selected) {
            case 0:
                renderAppearanceTab();
                break;
        }
        ImGui::EndChild();

        // Bottom buttons
        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            applySettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            applySettings();
            isWindowOpen = false;
            tempPrefsInitialized = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            // Restore original settings
            tempPrefs = Preferences::get().getAppearance();
            applySettings();
            isWindowOpen = false;
            tempPrefsInitialized = false;
        }
    }
    ImGui::End();

    if (open) *open = isWindowOpen;
}

void PreferencesWindow::renderAppearanceTab() {
    bool changed = false;

    ImGui::Text("UI Scale");
    if (ImGui::SliderFloat("##UIScale", &tempPrefs.uiScale, 0.5f, 5.0f, "%.2fx")) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##Scale")) {
        tempPrefs.uiScale = 1.25f;
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Text("Theme");
    const char* themes[] = { "Dark", "Light", "Classic" };
    if (ImGui::BeginCombo("##Theme", tempPrefs.theme.c_str())) {
        for (const auto& theme : themes) {
            bool isSelected = (tempPrefs.theme == theme);
            if (ImGui::Selectable(theme, isSelected)) {
                tempPrefs.theme = theme;
                changed = true;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Enable Docking", &tempPrefs.enableDocking)) {
        changed = true;
    }
    if (ImGui::Checkbox("Enable Viewports", &tempPrefs.enableViewports)) {
        changed = true;
    }

    // Apply changes immediately if something changed
    if (changed) {
        applySettings();
    }
}

void PreferencesWindow::applySettings() {
    auto& prefs = Preferences::get();

    // Apply UI scale and theme immediately
    ImGui::GetIO().FontGlobalScale = tempPrefs.uiScale;

    // Use UIManager's theme application
    if (UIManager* uiManager = UIManager::getInstance()) {
        uiManager->applyTheme(tempPrefs.theme);
    }

    // Save the changes to preferences
    prefs.setAppearance(tempPrefs);

    OHAO_LOG_DEBUG("UI Scale: " + std::to_string(tempPrefs.uiScale));
    OHAO_LOG_DEBUG("Theme: " + tempPrefs.theme);
    OHAO_LOG_DEBUG("Docking: " + std::string(tempPrefs.enableDocking ? "enabled" : "disabled"));
    OHAO_LOG_DEBUG("Viewports: " + std::string(tempPrefs.enableViewports ? "enabled" : "disabled"));
}

} // namespace ohao
