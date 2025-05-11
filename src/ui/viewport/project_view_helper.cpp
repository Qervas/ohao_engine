#include "project_view_helper.hpp"
#include "scene_viewport.hpp"
#include "ui/components/file_dialog.hpp"
#include "ui/components/console_widget.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace ohao {

ProjectViewHelper::ProjectViewHelper(SceneViewport* viewport)
    : m_viewport(viewport)
    , m_showStartupDialog(true)
{
    loadRecentProjects();
}

void ProjectViewHelper::loadRecentProjects() {
    // Stub implementation - will be expanded later
    OHAO_LOG("Loading recent projects...");
    // For now, just clear the list
    m_recentProjects.clear();
}

bool ProjectViewHelper::createNewProject(VulkanContext* context, const std::string& projectDir) {
    // Stub implementation - will be expanded later
    OHAO_LOG("Creating new project in directory: " + (projectDir.empty() ? "default" : projectDir));
    return false; // Not yet implemented
}

bool ProjectViewHelper::loadProject(VulkanContext* context, const std::string& projectPath) {
    // Stub implementation - will be expanded later
    OHAO_LOG("Loading project from: " + (projectPath.empty() ? "dialog" : projectPath));
    return false; // Not yet implemented
}

bool ProjectViewHelper::renderStartupDialog(VulkanContext* context) {
    // Basic implementation to be expanded later
    bool result = false;
    
    if (ImGui::BeginPopupModal("Project Selection", nullptr, 
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        
        ImGui::Text("Please select an option to continue:");
        ImGui::Separator();
        
        if (ImGui::Button("Create New Project", ImVec2(200, 0))) {
            result = createNewProject(context);
            if (result) {
                m_showStartupDialog = false;
                ImGui::CloseCurrentPopup();
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Open Existing Project", ImVec2(200, 0))) {
            std::string projectPath = FileDialog::openFile(
                "Open Project",
                "",
                std::vector<const char*>{".json"},
                "Project File (*.json)"
            );
            
            if (!projectPath.empty()) {
                result = loadProject(context, projectPath);
                if (result) {
                    m_showStartupDialog = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        
        ImGui::EndPopup();
    }
    
    return result;
}

std::string ProjectViewHelper::getEngineConfigPath() const {
    // Create ~/.ohao directory if it doesn't exist
    std::string homeDir = std::getenv("HOME") ? std::getenv("HOME") : ".";
    std::string configDir = homeDir + "/.ohao";
    
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Failed to create config directory: " + std::string(e.what()));
        }
    }
    
    return configDir;
}

} // namespace ohao 