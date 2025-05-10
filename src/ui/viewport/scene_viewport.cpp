#include "scene_viewport.hpp"
#include "ui/components/console_widget.hpp"
#include "ui/components/file_dialog.hpp"
#include <filesystem>
#include <algorithm>

namespace ohao {

SceneViewport::SceneViewport() {
}

void SceneViewport::render(VulkanContext* context) {
    if (!context) return;
    
    // First, refresh tabs to ensure we're showing current engine state
    refreshTabsFromContext(context);
    
    // Set up viewport window styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    
    // Begin the viewport window with tabs
    if (ImGui::Begin("Scene Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Render the scene tabs at the top
        renderSceneTabs(context);
        
        // Get viewport content area size
        m_viewportSize = ImGui::GetContentRegionAvail();
        m_isHovered = ImGui::IsWindowHovered();
        m_isFocused = ImGui::IsWindowFocused();
        
        // Render the scene texture
        ImVec2 pos = ImGui::GetCursorScreenPos();
        
        auto sceneTexture = context->getSceneRenderer()->getViewportTexture();
        if (sceneTexture) {
            ImTextureID imguiTexID = imgui::convertVulkanTextureToImGui(sceneTexture);
            ImGui::GetWindowDrawList()->AddImage(
                imguiTexID,
                pos,
                ImVec2(pos.x + m_viewportSize.x, pos.y + m_viewportSize.y),
                ImVec2(0, 0),
                ImVec2(1, 1)
            );
        }
        
        // Viewport resolution text at the bottom
        ImGui::SetCursorPos(ImVec2(10, m_viewportSize.y - 30));
        ImGui::Text("Viewport: %dx%d", (int)m_viewportSize.x, (int)m_viewportSize.y);

        // Handle keyboard shortcuts when the viewport is focused
        if (m_isFocused) {
            handleKeyboardShortcuts(context);
        }
    }
    
    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
    
    // Handle creating a new scene if dialog is open
    if (m_creatingNewTab) {
        ImGui::OpenPopup("Create New Scene");
        ImGui::SetNextWindowSize(ImVec2(300, 120));
        
        if (ImGui::BeginPopupModal("Create New Scene", &m_creatingNewTab, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter a name for the new scene:");
            ImGui::InputText("##SceneName", m_newSceneName, sizeof(m_newSceneName));
            
            ImGui::Separator();
            
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                std::string name = m_newSceneName;
                if (!name.empty()) {
                    createNewScene(context, name);
                    m_creatingNewTab = false;
                    ImGui::CloseCurrentPopup();
                    m_newSceneName[0] = '\0'; // Clear input buffer
                }
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                m_creatingNewTab = false;
                ImGui::CloseCurrentPopup();
                m_newSceneName[0] = '\0'; // Clear input buffer
            }
            
            ImGui::EndPopup();
        }
    }
}

void SceneViewport::handleKeyboardShortcuts(VulkanContext* context) {
    ImGuiIO& io = ImGui::GetIO();
    
    // Ctrl+S to save current scene
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        saveCurrentScene(context);
    }
    
    // Ctrl+T to create new scene (similar to browser new tab)
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T)) {
        m_creatingNewTab = true;
        strcpy(m_newSceneName, "NewScene");
    }
}

void SceneViewport::renderSceneTabs(VulkanContext* context) {
    if (m_sceneTabs.empty()) {
        // If no tabs, just show a message and the "+" button
        ImGui::Text("No scenes open");
        ImGui::SameLine(ImGui::GetWindowWidth() - 30);
        if (ImGui::Button("+")) {
            m_creatingNewTab = true;
            strcpy(m_newSceneName, "NewScene");
        }
        return;
    }
    
    // Create a modified ImGuiTabBarFlags with close buttons
    ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll;

    if (ImGui::BeginTabBar("SceneTabs", tab_bar_flags)) {
        // Render each scene tab
        for (int i = 0; i < m_sceneTabs.size(); i++) {
            auto& tab = m_sceneTabs[i];
            
            // Add an asterisk for modified scenes
            std::string tabLabel = tab.name + (tab.isModified ? "*" : "");
            
            // Create tab with optional close button
            ImGuiTabItemFlags flags = tab.isActive ? ImGuiTabItemFlags_SetSelected : 0;
            bool open = true;
            
            if (ImGui::BeginTabItem(tabLabel.c_str(), &open, flags)) {
                // This tab is now selected
                if (m_activeTabIndex != i) {
                    activateTab(context, i);
                }
                ImGui::EndTabItem();
            }
            
            // If tab was closed
            if (!open) {
                closeTab(context, i);
                // Don't process further since tabs array changed
                break;
            }
        }
        
        // Add a "+" button at the end of the tabs for creating a new scene
        ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
        if (ImGui::IsItemClicked()) {
            m_creatingNewTab = true;
            strcpy(m_newSceneName, "NewScene");
        }
        
        ImGui::EndTabBar();
    }
}

void SceneViewport::activateTab(VulkanContext* context, int index) {
    if (index < 0 || index >= m_sceneTabs.size()) return;
    
    // Get the current active tab and check if it has unsaved changes
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size() && 
        m_sceneTabs[m_activeTabIndex].isModified) {
        
        // Ask user if they want to save changes
        ImGui::OpenPopup("Save Changes?");
    } else {
        // No unsaved changes, activate the new tab directly
        m_activeTabIndex = index;
        context->activateScene(m_sceneTabs[index].name);
    }
    
    // Handle save changes popup if opened
    bool open = true;
    if (ImGui::BeginPopupModal("Save Changes?", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before switching scenes?", 
                    m_sceneTabs[m_activeTabIndex].name.c_str());
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            saveCurrentScene(context);
            // Now activate the new tab
            m_activeTabIndex = index;
            context->activateScene(m_sceneTabs[index].name);
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            // Activate without saving
            m_activeTabIndex = index;
            context->activateScene(m_sceneTabs[index].name);
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            // Don't switch tabs
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

void SceneViewport::closeTab(VulkanContext* context, int index) {
    if (index < 0 || index >= m_sceneTabs.size()) return;
    
    // If this is the currently active tab and has unsaved changes, prompt to save
    if (index == m_activeTabIndex && m_sceneTabs[index].isModified) {
        ImGui::OpenPopup("Save Before Closing?");
    } else {
        // Close the tab directly
        std::string sceneName = m_sceneTabs[index].name;
        m_sceneTabs.erase(m_sceneTabs.begin() + index);
        
        // Update the active tab index if necessary
        if (m_activeTabIndex >= m_sceneTabs.size()) {
            m_activeTabIndex = m_sceneTabs.empty() ? -1 : m_sceneTabs.size() - 1;
        } else if (m_activeTabIndex > index) {
            m_activeTabIndex--;
        }
        
        // Close the scene in the engine
        context->closeScene(sceneName);
        
        // Activate a new scene if available
        if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
            context->activateScene(m_sceneTabs[m_activeTabIndex].name);
        }
    }
    
    // Handle save before closing popup if opened
    bool open = true;
    if (ImGui::BeginPopupModal("Save Before Closing?", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before closing?", m_sceneTabs[index].name.c_str());
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            saveCurrentScene(context);
            // Now close the tab
            std::string sceneName = m_sceneTabs[index].name;
            m_sceneTabs.erase(m_sceneTabs.begin() + index);
            
            // Update active tab index
            if (m_activeTabIndex >= m_sceneTabs.size()) {
                m_activeTabIndex = m_sceneTabs.empty() ? -1 : m_sceneTabs.size() - 1;
            } else if (m_activeTabIndex > index) {
                m_activeTabIndex--;
            }
            
            // Close the scene in the engine
            context->closeScene(sceneName);
            
            // Activate a new scene if available
            if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
                context->activateScene(m_sceneTabs[m_activeTabIndex].name);
            }
            
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            // Close without saving
            std::string sceneName = m_sceneTabs[index].name;
            m_sceneTabs.erase(m_sceneTabs.begin() + index);
            
            // Update active tab index
            if (m_activeTabIndex >= m_sceneTabs.size()) {
                m_activeTabIndex = m_sceneTabs.empty() ? -1 : m_sceneTabs.size() - 1;
            } else if (m_activeTabIndex > index) {
                m_activeTabIndex--;
            }
            
            // Close the scene in the engine
            context->closeScene(sceneName);
            
            // Activate a new scene if available
            if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
                context->activateScene(m_sceneTabs[m_activeTabIndex].name);
            }
            
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            // Don't close the tab
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

void SceneViewport::createNewScene(VulkanContext* context, const std::string& name) {
    // Check if a scene with this name already exists
    auto it = std::find_if(m_sceneTabs.begin(), m_sceneTabs.end(), 
                         [&name](const SceneTab& tab) { return tab.name == name; });
    
    if (it != m_sceneTabs.end()) {
        // A scene with this name already exists, activate it instead
        int index = std::distance(m_sceneTabs.begin(), it);
        activateTab(context, index);
        return;
    }
    
    // Create a new scene in the engine
    if (context->createScene(name)) {
        // Add the new tab
        SceneTab newTab;
        newTab.name = name;
        newTab.isActive = true;
        newTab.isModified = false;
        
        // Add the tab and update active index
        m_sceneTabs.push_back(newTab);
        m_activeTabIndex = m_sceneTabs.size() - 1;
        
        // Make sure the engine activates it
        context->activateScene(name);
        
        OHAO_LOG("Created new scene: " + name);
    } else {
        OHAO_LOG_ERROR("Failed to create new scene: " + name);
    }
}

void SceneViewport::saveCurrentScene(VulkanContext* context) {
    // Get current active scene name
    std::string activeScene = context->getActiveSceneName();
    if (activeScene.empty()) {
        OHAO_LOG_ERROR("No active scene to save");
        return;
    }
    
    // If we don't have a project path yet, ask for one
    if (m_projectPath.empty()) {
        std::string projectPath = FileDialog::saveFile(
            "Save Project",
            "",
            std::vector<const char*>{".ohao"},
            "OHAO Project Files (*.ohao)"
        );
        
        if (projectPath.empty()) {
            return; // User canceled
        }
        
        // Add extension if missing
        if (std::filesystem::path(projectPath).extension().empty()) {
            projectPath += ".ohao";
        }
        
        m_projectPath = projectPath;
    }
    
    // Save the current scene to the project
    if (context->saveSceneToFile(m_projectPath)) {
        OHAO_LOG("Saved scene '" + activeScene + "' to project: " + m_projectPath);
        
        // Update tab modified state
        if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
            m_sceneTabs[m_activeTabIndex].isModified = false;
        }
    } else {
        OHAO_LOG_ERROR("Failed to save scene to project: " + m_projectPath);
    }
}

void SceneViewport::refreshTabsFromContext(VulkanContext* context) {
    // Get the current active scene name and loaded scenes
    std::string activeScene = context->getActiveSceneName();
    auto loadedScenes = context->getLoadedSceneNames();
    
    // First, remove any tabs for scenes that are no longer loaded
    auto it = m_sceneTabs.begin();
    while (it != m_sceneTabs.end()) {
        if (std::find(loadedScenes.begin(), loadedScenes.end(), it->name) == loadedScenes.end()) {
            // Scene was unloaded, remove the tab
            if (m_activeTabIndex > std::distance(m_sceneTabs.begin(), it)) {
                m_activeTabIndex--;
            } else if (m_activeTabIndex == std::distance(m_sceneTabs.begin(), it)) {
                m_activeTabIndex = -1; // Active tab was removed
            }
            it = m_sceneTabs.erase(it);
        } else {
            ++it;
        }
    }
    
    // Add tabs for any new scenes
    for (const auto& sceneName : loadedScenes) {
        auto it = std::find_if(m_sceneTabs.begin(), m_sceneTabs.end(), 
                             [&sceneName](const SceneTab& tab) { return tab.name == sceneName; });
        
        if (it == m_sceneTabs.end()) {
            // This scene doesn't have a tab yet, add one
            SceneTab newTab;
            newTab.name = sceneName;
            newTab.isActive = (sceneName == activeScene);
            newTab.isModified = false;
            m_sceneTabs.push_back(newTab);
            
            // If this is the active scene, update active index
            if (newTab.isActive) {
                m_activeTabIndex = m_sceneTabs.size() - 1;
            }
        }
    }
    
    // Update active state and modified state for all tabs
    for (int i = 0; i < m_sceneTabs.size(); i++) {
        m_sceneTabs[i].isActive = (m_sceneTabs[i].name == activeScene);
        if (m_sceneTabs[i].isActive) {
            m_activeTabIndex = i;
            m_sceneTabs[i].isModified = context->hasUnsavedChanges();
        }
    }
}

} // namespace ohao 