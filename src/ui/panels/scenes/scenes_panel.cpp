#include "scenes_panel.hpp"
#include "imgui.h"
#include "ui/components/console_widget.hpp"
#include "ui/components/file_dialog.hpp"
#include "core/scene/scene.hpp"
#include <filesystem>
#include <algorithm>

namespace ohao {

ScenesPanel::ScenesPanel(VulkanContext* context)
    : PanelBase("Scenes")
    , context(context)
    , showNewSceneDialog(false)
    , showSaveSceneDialog(false)
    , showLoadSceneDialog(false)
{
    // Register scene change callback
    context->registerSceneChangeCallback([this](const std::string& sceneName) {
        onSceneChanged(sceneName);
    });
}

ScenesPanel::~ScenesPanel() {
    // Unregister scene change callback
    context->unregisterSceneChangeCallback([this](const std::string& sceneName) {
        onSceneChanged(sceneName);
    });
}

void ScenesPanel::render() {
    if (ImGui::Begin("Scenes")) {
        renderSceneTabs();
        renderSceneToolbar();
        renderSceneContent();
        renderUndoRedoButtons();
        renderSceneHistory();

        if (showNewSceneDialog) {
            renderNewSceneDialog();
        }
        if (showSaveSceneDialog) {
            renderSaveSceneDialog();
        }
        if (showLoadSceneDialog) {
            renderLoadSceneDialog();
        }
    }
        ImGui::End();
}

void ScenesPanel::renderSceneTabs() {
    const auto& scenes = context->getScenes();
    auto activeScene = context->getActiveScene();

    if (ImGui::BeginTabBar("SceneTabs")) {
        for (const auto& scene : scenes) {
            bool isActive = scene == activeScene;
            bool isDirty = scene->isDirty();
            std::string tabName = scene->getName();
            if (isDirty) {
                tabName += " *";
            }

            if (ImGui::BeginTabItem(tabName.c_str(), nullptr, isActive ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!isActive) {
                    activateScene(scene->getName());
    }
                ImGui::EndTabItem();
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                selectedScene = scene->getName();
                ImGui::OpenPopup("SceneContextMenu");
            }
        }

        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) {
            showNewSceneDialog = true;
        }

        if (ImGui::BeginPopup("SceneContextMenu")) {
            if (ImGui::MenuItem("Save")) {
                sceneToSave = selectedScene;
                showSaveSceneDialog = true;
            }
            if (ImGui::MenuItem("Close")) {
                removeScene(selectedScene);
            }
            ImGui::EndPopup();
    }
    
        ImGui::EndTabBar();
    }
}

void ScenesPanel::renderSceneContent() {
    auto activeScene = context->getActiveScene();
    if (!activeScene) {
        ImGui::Text("No active scene");
        return;
    }

    ImGui::BeginChild("SceneContent", ImVec2(0, 0), true);
    // TODO: Render scene content (actors, components, etc.)
    ImGui::EndChild();
}

void ScenesPanel::renderSceneToolbar() {
    if (ImGui::Button("New Scene")) {
        showNewSceneDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Scene")) {
        auto activeScene = context->getActiveScene();
        if (activeScene) {
            sceneToSave = activeScene->getName();
            showSaveSceneDialog = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Scene")) {
        showLoadSceneDialog = true;
            }
}

void ScenesPanel::renderUndoRedoButtons() {
    if (ImGui::Button("Undo")) {
        context->undo();
                }
                ImGui::SameLine();
    if (ImGui::Button("Redo")) {
        context->redo();
        }
    }

void ScenesPanel::renderSceneHistory() {
    auto activeScene = context->getActiveScene();
    if (!activeScene) return;

    if (ImGui::CollapsingHeader("Scene History")) {
        // The actual change history API is not available yet
        // Display a placeholder message instead
        ImGui::TextDisabled("Scene history tracking not implemented yet.");
        
        // Display basic scene info
        ImGui::Text("Scene Name: %s", activeScene->getName().c_str());
        ImGui::Text("Is Dirty: %s", activeScene->isDirty() ? "Yes" : "No");
        ImGui::Text("Can Undo: %s", context->canUndo() ? "Yes" : "No");
        ImGui::Text("Can Redo: %s", context->canRedo() ? "Yes" : "No");
    }
}

void ScenesPanel::renderNewSceneDialog() {
    if (ImGui::BeginPopupModal("New Scene", &showNewSceneDialog)) {
        // Using a fixed size buffer for ImGui::InputText
        static char nameBuffer[128] = "";
        ImGui::InputText("Scene Name", nameBuffer, sizeof(nameBuffer));
        
        if (ImGui::Button("Create")) {
            std::string newName = nameBuffer;
            if (!newName.empty()) {
                createNewScene();
                newSceneName = newName; // Store the name for later use
            }
            showNewSceneDialog = false;
            // Clear the buffer after use
            nameBuffer[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showNewSceneDialog = false;
            // Clear the buffer on cancel
            nameBuffer[0] = '\0';
        }
        ImGui::EndPopup();
    }
}

void ScenesPanel::renderSaveSceneDialog() {
    if (ImGui::BeginPopupModal("Save Scene", &showSaveSceneDialog)) {
        // Using a fixed size buffer for ImGui::InputText
        static char nameBuffer[128] = "";
        // Pre-fill with current scene name
        if (nameBuffer[0] == '\0' && !sceneToSave.empty()) {
            strncpy(nameBuffer, sceneToSave.c_str(), sizeof(nameBuffer) - 1);
            nameBuffer[sizeof(nameBuffer) - 1] = '\0'; // Ensure null termination
        }
        
        ImGui::InputText("Scene Name", nameBuffer, sizeof(nameBuffer));
        
        if (ImGui::Button("Save")) {
            std::string filename = nameBuffer;
            if (!filename.empty()) {
                saveScene(filename);
            }
            showSaveSceneDialog = false;
            // Clear the buffer after use
            nameBuffer[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showSaveSceneDialog = false;
            // Clear the buffer on cancel
            nameBuffer[0] = '\0';
        }
        ImGui::EndPopup();
    }
}

void ScenesPanel::renderLoadSceneDialog() {
    if (ImGui::BeginPopupModal("Load Scene", &showLoadSceneDialog)) {
        // Using a fixed size buffer for ImGui::InputText
        static char nameBuffer[128] = "";
        ImGui::InputText("Scene File", nameBuffer, sizeof(nameBuffer));
        
        if (ImGui::Button("Load")) {
            std::string filename = nameBuffer;
            if (!filename.empty()) {
                loadScene(filename);
            }
            showLoadSceneDialog = false;
            // Clear the buffer after use
            nameBuffer[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showLoadSceneDialog = false;
            // Clear the buffer on cancel
            nameBuffer[0] = '\0';
        }
        ImGui::EndPopup();
    }
}

void ScenesPanel::createNewScene() {
    if (!newSceneName.empty()) {
        context->createNewScene(newSceneName);
        newSceneName.clear();
    }
}

void ScenesPanel::saveScene(const std::string& name) {
    if (!name.empty()) {
        context->saveScene(name);
        }
}

void ScenesPanel::loadScene(const std::string& filename) {
    if (!filename.empty()) {
        context->loadScene(filename);
        }
}

void ScenesPanel::activateScene(const std::string& name) {
    context->activateScene(name);
}

void ScenesPanel::removeScene(const std::string& name) {
    if (name.empty()) return;
    
    // Check if there are unsaved changes
    if (context->hasUnsavedChanges()) {
        // For now, just log a warning - in a real app we'd show a confirmation dialog
        OHAO_LOG_WARNING("Closing scene with unsaved changes: " + name);
    }
    
    // Use the VulkanContext to close the scene
    context->closeScene(name);
}

void ScenesPanel::onSceneChanged(const std::string& sceneName) {
    // Update UI state when scene changes
    selectedScene = sceneName;
}

} // namespace ohao 