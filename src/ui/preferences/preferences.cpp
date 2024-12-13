#include "preferences.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "console_widget.hpp"
#include "nlohmann/json.hpp"

namespace ohao {

void Preferences::load() {
    try {
        std::string filePath = getPreferencesFilePath();
        if (!std::filesystem::exists(filePath)) {
            OHAO_LOG_DEBUG("Preferences file not found, creating default preferences");
            createDefaultPreferences();
            return;
        }

        std::ifstream file(filePath);
        if (!file.is_open()) {
            OHAO_LOG_ERROR("Failed to open preferences file");
            createDefaultPreferences();
            return;
        }

        nlohmann::json j;
        file >> j;

        // Load appearance preferences
        auto& appearanceJson = j["appearance"];
        appearance.uiScale = appearanceJson["uiScale"].get<float>();
        appearance.theme = appearanceJson["theme"].get<std::string>();
        appearance.enableDocking = appearanceJson["enableDocking"].get<bool>();
        appearance.enableViewports = appearanceJson["enableViewports"].get<bool>();
        OHAO_LOG_DEBUG("Preferences loaded successfully");

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error loading preferences: " + std::string(e.what()));
        createDefaultPreferences();
    }
}

void Preferences::save() {
    try {
        nlohmann::json j;

        // Save appearance preferences
        j["appearance"] = {
            {"uiScale", appearance.uiScale},
            {"theme", appearance.theme},
            {"enableDocking", appearance.enableDocking},
            {"enableViewports", appearance.enableViewports}
        };

        std::string filePath = getPreferencesFilePath();

        // Make sure the directory exists
        std::filesystem::path path(filePath);
        if (!std::filesystem::exists(path.parent_path())) {
            std::filesystem::create_directories(path.parent_path());
        }

        // Open file with truncation
        std::ofstream file(filePath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            OHAO_LOG_ERROR("Failed to save preferences file: " + filePath);
            return;
        }

        file << j.dump(4);
        file.close();

        if (file.good()) {
            OHAO_LOG_DEBUG("Preferences saved successfully to: " + filePath);
        } else {
            OHAO_LOG_ERROR("Error occurred while saving preferences");
        }

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error saving preferences: " + std::string(e.what()));
    }
}

std::string Preferences::getPreferencesFilePath() const {
    std::filesystem::path appDataPath;
#ifdef _WIN32
    appDataPath = std::getenv("APPDATA");
    appDataPath /= "OhaoEngine";
#else
    appDataPath = std::getenv("HOME");
    appDataPath /= ".config/ohao_engine";
#endif

    if (!std::filesystem::exists(appDataPath)) {
        std::filesystem::create_directories(appDataPath);
        OHAO_LOG_DEBUG("Created preferences directory: " + appDataPath.string());
    }

    return (appDataPath / PREFERENCES_FILENAME).string();
}

void Preferences::createDefaultPreferences() {
    appearance.uiScale = 1.0f;
    appearance.theme = "Dark";
    appearance.enableDocking = true;
    appearance.enableViewports = true;
    save();
}

} // namespace ohao
