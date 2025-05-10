#include "scenes_panel.hpp"
#include "imgui.h"
#include "ui/components/console_widget.hpp"
#include "ui/components/file_dialog.hpp"
#include "core/scene/scene.hpp"
#include <filesystem>

namespace ohao {

ScenesPanel::ScenesPanel() 
    : PanelBase("Scenes") 
{
}

void ScenesPanel::render() {
    if (!m_context) {
        ImGui::Text("VulkanContext not set!");
        return;
    }

    if (!ImGui::Begin(name.c_str(), &visible, windowFlags)) {
        ImGui::End();
        return;
    }
    
    // Project information
    if (!m_projectPath.empty()) {
        ImGui::Text("Project: %s", std::filesystem::path(m_projectPath).filename().string().c_str());
    } else {
        ImGui::Text("Project: [Unsaved Project]");
    }
    
    ImGui::Separator();
    
    // Project management buttons
    if (ImGui::Button("New Project")) {
        // Ask to save current project if any unsaved changes
        if (m_context->hasUnsavedChanges()) {
            m_showSaveDirtyDialog = true;
            m_pendingSceneToActivate = ""; // No scene to activate, creating new project
        } else {
            createNewProject("NewProject");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Project")) {
        // Ask to save current project if any unsaved changes
        if (m_context->hasUnsavedChanges()) {
            m_showSaveDirtyDialog = true;
            m_pendingSceneToActivate = ""; // No scene to activate, opening project
        } else {
            std::string path = FileDialog::openFile(
                "Open Project",
                "",
                std::vector<const char*>{".ohao"},
                "OHAO Project Files (*.ohao)"
            );
            if (!path.empty()) {
                loadProject(path);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Project")) {
        saveProject();
    }
    
    ImGui::Separator();

    // Show current active scene
    std::string activeScene = m_context->getActiveSceneName();
    ImGui::Text("Active Scene: %s", activeScene.empty() ? "[None]" : activeScene.c_str());
    
    // Mark if the scene has unsaved changes
    if (m_context->hasUnsavedChanges()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "*");
        ImGui::SameLine();
        ImGui::Text("(unsaved)");
    }
    
    ImGui::Separator();

    // Scene management buttons
    if (ImGui::Button("New Scene")) {
        m_showCreateDialog = true;
        m_newSceneName = "NewScene";
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Scene")) {
        saveCurrentScene();
    }

    ImGui::Separator();
    
    // List of loaded scenes
    ImGui::Text("Scenes in Project:");
    auto sceneNames = m_context->getLoadedSceneNames();
    
    if (sceneNames.empty()) {
        ImGui::Text("No scenes loaded");
    } else {
        for (const auto& sceneName : sceneNames) {
            bool isActive = (sceneName == activeScene);
            
            // Visual indication for active scene
            if (isActive) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
            }
            
            ImGui::BulletText("%s", sceneName.c_str());
            
            if (isActive) {
                ImGui::PopStyleColor();
            }
            
            ImGui::SameLine();
            ImGui::PushID(sceneName.c_str());
            
            // Activate button (only for non-active scenes)
            if (!isActive) {
                if (ImGui::Button("Activate")) {
                    tryActivateScene(sceneName);
                }
                ImGui::SameLine();
            }
            
            // Close button
            if (ImGui::Button("Close")) {
                tryCloseScene(sceneName);
            }
            
            ImGui::PopID();
        }
    }

    // Render dialogs if needed
    if (m_showCreateDialog) renderCreateSceneDialog();
    if (m_showLoadDialog) renderLoadSceneDialog();
    if (m_showSaveDialog) renderSaveSceneDialog();
    if (m_showConfirmClose) renderConfirmCloseDialog();
    if (m_showSaveDirtyDialog) renderSaveDirtySceneDialog();

    ImGui::End();
}

void ScenesPanel::renderCreateSceneDialog() {
    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::BeginPopupModal("Create New Scene", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter a name for the new scene:");
        
        // Use a static buffer for ImGui input
        static char nameBuffer[256] = "";
        ImGui::InputText("Scene Name", nameBuffer, sizeof(nameBuffer));
        
        ImGui::Separator();
        
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            // Update the member string with buffer contents
            m_newSceneName = nameBuffer;
            
            if (m_newSceneName.empty()) {
                OHAO_LOG_ERROR("Scene name cannot be empty");
            } else if (m_context->isSceneLoaded(m_newSceneName)) {
                OHAO_LOG_ERROR("Scene with name '" + m_newSceneName + "' already exists");
            } else {
                createNewScene(m_newSceneName);
                ImGui::CloseCurrentPopup();
                m_showCreateDialog = false;
                // Clear the buffer for next use
                nameBuffer[0] = '\0';
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_showCreateDialog = false;
            // Clear the buffer for next use
            nameBuffer[0] = '\0';
        }
        
        ImGui::EndPopup();
    } else {
        ImGui::OpenPopup("Create New Scene");
    }
}

void ScenesPanel::renderLoadSceneDialog() {
    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::BeginPopupModal("Load Scene", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter the path to the scene file (.ohao):");
        
        // Use a static buffer for ImGui input
        static char filenameBuffer[256] = "";
        ImGui::InputText("Scene File", filenameBuffer, sizeof(filenameBuffer));
        
        ImGui::Separator();
        
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            // Update the member string with buffer contents
            m_sceneToLoad = filenameBuffer;
            
            if (m_sceneToLoad.empty()) {
                OHAO_LOG_ERROR("Scene file path cannot be empty");
            } else {
                // Add extension if missing
                std::filesystem::path filePath(m_sceneToLoad);
                if (filePath.extension().empty()) {
                    m_sceneToLoad += ".ohao";
                }
                
                if (m_context->loadSceneFromFile(m_sceneToLoad)) {
                    OHAO_LOG("Loaded scene from file: " + m_sceneToLoad);
                    ImGui::CloseCurrentPopup();
                    m_showLoadDialog = false;
                    // Clear the buffer for next use
                    filenameBuffer[0] = '\0';
                } else {
                    OHAO_LOG_ERROR("Failed to load scene from file: " + m_sceneToLoad);
                }
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_showLoadDialog = false;
            // Clear the buffer for next use
            filenameBuffer[0] = '\0';
        }
        
        ImGui::EndPopup();
    } else {
        ImGui::OpenPopup("Load Scene");
    }
}

void ScenesPanel::renderSaveSceneDialog() {
    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::BeginPopupModal("Save Scene", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter the path to save the scene file (.ohao):");
        
        // Use a static buffer for ImGui input
        static char filenameBuffer[256] = "";
        if (m_sceneToSave.size() < sizeof(filenameBuffer)) {
            // Only copy into buffer if not already set
            if (filenameBuffer[0] == '\0') {
                strncpy(filenameBuffer, m_sceneToSave.c_str(), sizeof(filenameBuffer) - 1);
                filenameBuffer[sizeof(filenameBuffer) - 1] = '\0';
            }
        }
        
        ImGui::InputText("Scene File", filenameBuffer, sizeof(filenameBuffer));
        
        ImGui::Separator();
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            // Update the member string with buffer contents
            m_sceneToSave = filenameBuffer;
            
            if (m_sceneToSave.empty()) {
                OHAO_LOG_ERROR("Scene file path cannot be empty");
            } else {
                // Add extension if missing
                std::filesystem::path filePath(m_sceneToSave);
                if (filePath.extension().empty()) {
                    m_sceneToSave += ".ohao";
                }
                
                if (saveCurrentScene()) {
                    ImGui::CloseCurrentPopup();
                    m_showSaveDialog = false;
                    // Clear the buffer for next use
                    filenameBuffer[0] = '\0';
                }
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_showSaveDialog = false;
            // Clear the buffer for next use
            filenameBuffer[0] = '\0';
        }
        
        ImGui::EndPopup();
    } else {
        ImGui::OpenPopup("Save Scene");
    }
}

void ScenesPanel::renderConfirmCloseDialog() {
    ImGui::SetNextWindowSize(ImVec2(400, 150), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::BeginPopupModal("Confirm Close Scene", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure you want to close scene '%s'?", m_sceneToClose.c_str());
        ImGui::Text("Any unsaved changes will be lost.");
        
        ImGui::Separator();
        
        if (ImGui::Button("Close Scene", ImVec2(120, 0))) {
            if (m_context->closeScene(m_sceneToClose)) {
                OHAO_LOG("Closed scene: " + m_sceneToClose);
            } else {
                OHAO_LOG_ERROR("Failed to close scene: " + m_sceneToClose);
            }
            
            ImGui::CloseCurrentPopup();
            m_showConfirmClose = false;
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_showConfirmClose = false;
        }
        
        ImGui::EndPopup();
    } else {
        ImGui::OpenPopup("Confirm Close Scene");
    }
}

void ScenesPanel::renderSaveDirtySceneDialog() {
    ImGui::SetNextWindowSize(ImVec2(400, 150), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::BeginPopupModal("Save Changes", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string activeScene = m_context->getActiveSceneName();
        ImGui::Text("Save changes to scene '%s'?", activeScene.empty() ? "Untitled" : activeScene.c_str());
        
        ImGui::Separator();
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            bool saved = saveCurrentScene();
            ImGui::CloseCurrentPopup();
            m_showSaveDirtyDialog = false;
            
            // If we saved successfully and have a pending scene to activate, do it now
            if (saved && !m_pendingSceneToActivate.empty()) {
                m_context->activateScene(m_pendingSceneToActivate);
                m_pendingSceneToActivate.clear();
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            // Proceed without saving
            ImGui::CloseCurrentPopup();
            m_showSaveDirtyDialog = false;
            
            // If we have a pending scene to activate, do it now
            if (!m_pendingSceneToActivate.empty()) {
                m_context->activateScene(m_pendingSceneToActivate);
                m_pendingSceneToActivate.clear();
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_showSaveDirtyDialog = false;
            // Clear pending scene activation
            m_pendingSceneToActivate.clear();
        }
        
        ImGui::EndPopup();
    } else {
        ImGui::OpenPopup("Save Changes");
    }
}

bool ScenesPanel::createNewScene(const std::string& name) {
    if (m_context->createScene(name)) {
        OHAO_LOG("Created new scene: " + name);
        if (m_context->activateScene(name)) {
            OHAO_LOG("Activated new scene: " + name);
            return true;
        }
    } else {
        OHAO_LOG_ERROR("Failed to create scene: " + name);
    }
    return false;
}

bool ScenesPanel::saveCurrentScene() {
    std::string activeScene = m_context->getActiveSceneName();
    if (activeScene.empty()) {
        OHAO_LOG_ERROR("No active scene to save");
        return false;
    }
    
    // Construct the save path based on the project path and scene name
    std::string savePath;
    if (m_projectPath.empty()) {
        // No project path yet, ask user for a project path
        std::string projectPath = FileDialog::saveFile(
            "Save Project",
            "",
            std::vector<const char*>{".ohao"},
            "OHAO Project Files (*.ohao)"
        );
        
        if (projectPath.empty()) {
            return false;
        }
        
        // Add extension if missing
        if (std::filesystem::path(projectPath).extension().empty()) {
            projectPath += ".ohao";
        }
        
        m_projectPath = projectPath;
    }
    
    // Construct scene save path
    // Get project directory
    std::filesystem::path projectDir = std::filesystem::path(m_projectPath).parent_path();
    
    // Create a scenes directory if it doesn't exist
    std::filesystem::path scenesDir = projectDir / "scenes";
    if (!std::filesystem::exists(scenesDir)) {
        std::filesystem::create_directories(scenesDir);
    }
    
    // Create scene file path with scene extension
    std::filesystem::path scenePath = scenesDir / (activeScene + Scene::FILE_EXTENSION);
    savePath = scenePath.string();
    
    // Save the scene to the file
    if (m_context->saveSceneToFile(savePath)) {
        OHAO_LOG("Saved scene '" + activeScene + "' to: " + savePath);
        
        // Now update the project file to reference this scene
        // TODO: Implement project file format with scene references
        
        return true;
    } else {
        OHAO_LOG_ERROR("Failed to save scene to: " + savePath);
        return false;
    }
}

bool ScenesPanel::tryActivateScene(const std::string& name) {
    // Check if current scene has unsaved changes
    if (m_context->hasUnsavedChanges()) {
        // Store the scene we want to activate and show confirmation dialog
        m_pendingSceneToActivate = name;
        m_showSaveDirtyDialog = true;
        return false;
    }
    
    // No unsaved changes, activate immediately
    if (m_context->activateScene(name)) {
        OHAO_LOG("Activated scene: " + name);
        return true;
    } else {
        OHAO_LOG_ERROR("Failed to activate scene: " + name);
        return false;
    }
}

bool ScenesPanel::tryCloseScene(const std::string& name) {
    // If this is the active scene and it has unsaved changes, ask to save first
    std::string activeScene = m_context->getActiveSceneName();
    if (name == activeScene && m_context->hasUnsavedChanges()) {
        m_sceneToClose = name;
        m_showSaveDirtyDialog = true;
        return false;
    }
    
    // Otherwise, confirm closing
    m_sceneToClose = name;
    m_showConfirmClose = true;
    return false;
}

bool ScenesPanel::loadProject(const std::string& path) {
    if (m_context->loadSceneFromFile(path)) {
        m_projectPath = path;
        OHAO_LOG("Loaded project from file: " + path);
        return true;
    } else {
        OHAO_LOG_ERROR("Failed to load project from file: " + path);
        return false;
    }
}

bool ScenesPanel::saveProject() {
    if (m_projectPath.empty()) {
        // No project path yet, ask user for a path
        std::string path = FileDialog::saveFile(
            "Save Project As",
            "",
            std::vector<const char*>{".ohao"},
            "OHAO Project Files (*.ohao)"
        );
        
        if (path.empty()) {
            return false;
        }
        
        // Add extension if missing
        if (std::filesystem::path(path).extension().empty()) {
            path += ".ohao";
        }
        
        m_projectPath = path;
    }
    
    return saveCurrentScene();
}

bool ScenesPanel::createNewProject(const std::string& name) {
    // Create a new project with a default scene
    if (m_context->createNewScene(name)) {
        m_projectPath = "";  // Reset project path since this is a new unsaved project
        OHAO_LOG("Created new project: " + name);
        return true;
    } else {
        OHAO_LOG_ERROR("Failed to create new project");
        return false;
    }
}

} // namespace ohao 