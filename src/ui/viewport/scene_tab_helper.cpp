#include "scene_tab_helper.hpp"
#include "scene_viewport.hpp"
#include "core/scene/scene.hpp"
#include "imgui.h"
#include "ui/components/console_widget.hpp"
#include <string>
#include <algorithm>

namespace ohao {

SceneTabViewHelper::SceneTabViewHelper(SceneViewport* viewport)
    : m_viewport(viewport), m_activeTabIndex(-1)
{
    // Initialize with empty state
}

void SceneTabViewHelper::renderTabs(VulkanContext* context) {
    // Even if there are no tabs, we still want to refresh from context
    // as there might be scenes in the engine that don't have tabs yet
    refreshTabsFromContext(context);
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 7));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1, 0));
    
    // If no tabs, just show a simple text and a "+" button
    if (m_sceneTabs.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No scenes open");
        
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.2f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.3f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.4f, 0.3f, 1.0f));
        
        if (ImGui::Button("+##NewTabEmpty", ImVec2(30, 30))) {
            OHAO_LOG("Creating new scene from empty state");
            m_creatingNewTab = true;
            memset(m_newSceneName, 0, sizeof(m_newSceneName));
            strncpy(m_newSceneName, "New Scene", sizeof(m_newSceneName) - 1);
        }
        
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        
        // Process any popups that need to be shown
        renderNewScenePopup(context);
        renderRenameScenePopup(context);
        return;
    }
    
    float tabBarWidth = ImGui::GetContentRegionAvail().x;
    float totalTabWidth = 0;
    float tabWidth = 150.0f;
    
    // Draw tab buttons
    for (size_t i = 0; i < m_sceneTabs.size(); i++) {
        auto& tab = m_sceneTabs[i];
        bool isActive = (static_cast<int>(i) == m_activeTabIndex);
        
        // Set button style based on whether tab is active or not
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.4f, 0.7f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.5f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.6f, 0.9f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
        }
        
        // Prepare tab label
        std::string tabLabel = tab.name;
        if (tab.isModified) {
            tabLabel += "*";
        }
        tabLabel += "##Tab" + std::to_string(i);
        
        if (i > 0) {
            ImGui::SameLine();
        }
        
        // Draw the tab button
        if (ImGui::Button(tabLabel.c_str(), ImVec2(tabWidth, 30))) {
            // Only handle click if this isn't already the active tab
            if (!isActive) {
                OHAO_LOG("Tab clicked: " + tab.name + " (index: " + std::to_string(i) + ")");
                
                // Call our activateTab method to handle scene switching properly
                activateTab(context, i);
            }
        }
        
        // Draw right-click context menu
        if (ImGui::BeginPopupContextItem(("TabMenu" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem("Close")) {
                closeTab(context, i);
            }
            
            if (ImGui::MenuItem("Rename")) {
                // Set renaming flag and focus on this tab
                m_renamingTab = true;
                m_activeTabIndex = i;
                strncpy(m_newSceneName, tab.name.c_str(), sizeof(m_newSceneName) - 1);
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::PopStyleColor(4); // Pop all the style colors
    }
    
    // Add "+" button to create a new tab
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.2f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.3f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.4f, 0.3f, 1.0f));
    
    if (ImGui::Button("+##NewTab", ImVec2(30, 30))) {
        OHAO_LOG("Creating new scene tab");
        m_creatingNewTab = true;
        memset(m_newSceneName, 0, sizeof(m_newSceneName));
        strncpy(m_newSceneName, "New Scene", sizeof(m_newSceneName) - 1);
    }
    
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    
    // Process any popups that need to be shown
    renderNewScenePopup(context);
    renderRenameScenePopup(context);
}

void SceneTabViewHelper::activateTab(VulkanContext* context, int index) {
    // Validate the index is within range
    if (index < 0 || index >= m_sceneTabs.size()) {
        return;
    }
    
    // Do nothing if this tab is already active
    if (m_activeTabIndex == index) {
        return;
    }
    
    // Get the scene name of the tab we're activating
    std::string newSceneName = m_sceneTabs[index].name;
    
    // Cache the current scene before switching
    cacheActiveScene(context);
    
    // Deactivate the current tab
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
        m_sceneTabs[m_activeTabIndex].isActive = false;
    }
    
    // Try to restore the scene from cache or activate it in the engine
    bool success = false;
    
    // First try to find the scene in currently loaded engine scenes
    if (context->isSceneLoaded(newSceneName)) {
        success = context->activateScene(newSceneName);
    } else {
        // Try to restore from our cache if loaded scenes doesn't have it
        success = restoreSceneFromCache(context, newSceneName);
    }
    
    if (success) {
        // Update UI state
        m_activeTabIndex = index;
        m_sceneTabs[index].isActive = true;
        
        // Force scene buffers update in the engine
        context->updateSceneBuffers();
        
        // Force the renderer to refresh
        if (context->getSceneRenderer()) {
            context->getSceneRenderer()->forceRefresh();
        }
        
        // Make sure the viewport gets focus to display the scene
        ImGui::SetWindowFocus("Scene Viewport");
    }
}

void SceneTabViewHelper::handleSaveChangesPopup(VulkanContext* context, int destinationTabIndex) {
    bool open = true;
    if (ImGui::BeginPopupModal("Save Changes?", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before switching scenes?", 
                    m_sceneTabs[m_activeTabIndex].name.c_str());
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            // Save the current scene
            saveCurrentScene(context);
            
            // Cache the current scene before switching
            cacheActiveScene(context);
            
            // Mark the previously active tab as inactive
            m_sceneTabs[m_activeTabIndex].isActive = false;
            
            // Now activate the new tab
            m_activeTabIndex = destinationTabIndex;
            m_sceneTabs[destinationTabIndex].isActive = true;
            
            // Activate the scene in the context
            bool activated = context->activateScene(m_sceneTabs[destinationTabIndex].name);
            OHAO_LOG(activated ? "Successfully activated scene after save" : "Failed to activate scene after save");
            
            // Wait for engine to process the change
            context->updateSceneBuffers();
            
            // Force a UI update to reflect the new scene
            refreshTabsFromContext(context);
            
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            // Cache the current scene before switching, even if unsaved
            cacheActiveScene(context);
            
            // Mark the previously active tab as inactive
            m_sceneTabs[m_activeTabIndex].isActive = false;
            
            // Activate without saving
            m_activeTabIndex = destinationTabIndex;
            m_sceneTabs[destinationTabIndex].isActive = true;
            
            // Activate the scene in the context
            bool activated = context->activateScene(m_sceneTabs[destinationTabIndex].name);
            OHAO_LOG(activated ? "Successfully activated scene without save" : "Failed to activate scene without save");
            
            // Wait for engine to process the change
            context->updateSceneBuffers();
            
            // Force a UI update to reflect the new scene
            refreshTabsFromContext(context);
            
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

void SceneTabViewHelper::closeTab(VulkanContext* context, int index) {
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

bool SceneTabViewHelper::createNewScene(VulkanContext* context, const std::string& name) {
    // Check if a scene with this name already exists
    auto it = std::find_if(m_sceneTabs.begin(), m_sceneTabs.end(), 
                         [&name](const SceneTab& tab) { return tab.name == name; });
    
    if (it != m_sceneTabs.end()) {
        // A scene with this name already exists, activate it instead
        int index = std::distance(m_sceneTabs.begin(), it);
        activateTab(context, index);
        return true;
    }
    
    // Cache the current scene before creating a new one
    cacheActiveScene(context);
    
    // Mark existing active tab as inactive
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
        m_sceneTabs[m_activeTabIndex].isActive = false;
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
        
        // Force engine to update its state
        context->updateSceneBuffers();
        
        // Explicitly make sure the engine has this scene active
        context->activateScene(name);
        
        // Force refresh to show correct state
        refreshTabsFromContext(context);
        
        OHAO_LOG("Created new scene: " + name);
        return true;
    } else {
        OHAO_LOG_ERROR("Failed to create new scene: " + name);
        return false;
    }
}

void SceneTabViewHelper::saveCurrentScene(VulkanContext* context) {
    // Delegate to the viewport
    m_viewport->saveCurrentScene(context);
}

void SceneTabViewHelper::renameCurrentScene(VulkanContext* context, const std::string& newName) {
    if (m_activeTabIndex < 0 || m_activeTabIndex >= m_sceneTabs.size()) return;
    
    std::string oldName = m_sceneTabs[m_activeTabIndex].name;
    
    // Check if a scene with this name already exists
    auto it = std::find_if(m_sceneTabs.begin(), m_sceneTabs.end(), 
                         [&newName](const SceneTab& tab) { return tab.name == newName; });
    
    if (it != m_sceneTabs.end()) {
        OHAO_LOG_ERROR("A scene with the name '" + newName + "' already exists");
        return;
    }
    
    // Rename the scene in the engine
    if (context->renameScene(oldName, newName)) {
        // Update the tab name
        m_sceneTabs[m_activeTabIndex].name = newName;
        OHAO_LOG("Renamed scene from '" + oldName + "' to '" + newName + "'");
    } else {
        OHAO_LOG_ERROR("Failed to rename scene from '" + oldName + "' to '" + newName + "'");
    }
}

void SceneTabViewHelper::refreshTabsFromContext(VulkanContext* context) {
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
    // Important: always sync with what the engine says is the active scene
    bool foundActiveTab = false;
    for (int i = 0; i < m_sceneTabs.size(); i++) {
        bool isActiveScene = (m_sceneTabs[i].name == activeScene);
        
        m_sceneTabs[i].isActive = isActiveScene;
        if (isActiveScene) {
            m_activeTabIndex = i;
            foundActiveTab = true;
            m_sceneTabs[i].isModified = context->hasUnsavedChanges();
        }
    }
    
    // If no active tab was found but we have tabs, set the first one as active
    if (!foundActiveTab && !m_sceneTabs.empty() && m_activeTabIndex < 0) {
        m_activeTabIndex = 0;
        m_sceneTabs[0].isActive = true;
        
        // Make sure the engine state matches
        context->activateScene(m_sceneTabs[0].name);
    }
    
    // Consistency check to make sure UI state is sane
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
        if (!m_sceneTabs[m_activeTabIndex].isActive) {
            m_sceneTabs[m_activeTabIndex].isActive = true;
        }
    }
}

bool SceneTabViewHelper::cacheActiveScene(VulkanContext* context) {
    // Get the current active scene
    auto activeScene = context->getActiveScene();
    std::string sceneName = context->getActiveSceneName();
    
    // Don't cache if no active scene
    if (!activeScene || sceneName.empty()) {
        return false;
    }
    
    // Store a copy of the scene in our cache
    m_cachedScenes[sceneName] = activeScene;
    return true;
}

bool SceneTabViewHelper::restoreSceneFromCache(VulkanContext* context, const std::string& sceneName) {
    // Check if the scene exists in our cache
    auto it = m_cachedScenes.find(sceneName);
    if (it == m_cachedScenes.end()) {
        return false;
    }
    
    // Get the cached scene
    auto cachedScene = it->second;
    if (!cachedScene) {
        return false;
    }
    
    // Check if the scene exists in loaded scenes
    if (context->isSceneLoaded(sceneName)) {
        // Just activate it, but ensure the state is synchronized
        context->activateScene(sceneName);
        return true;
    }
    
    // If the scene doesn't exist in the engine, we need to add it
    if (context->createScene(sceneName)) {
        // Now activate the scene
        context->activateScene(sceneName);
        return true;
    }
    
    return false;
}

void SceneTabViewHelper::removeSceneFromCache(const std::string& sceneName) {
    m_cachedScenes.erase(sceneName);
}

void SceneTabViewHelper::ensureDefaultScene(VulkanContext* context) {
    if (!context) return;
    
    // Check if any scenes are loaded
    auto loadedScenes = context->getLoadedSceneNames();
    if (loadedScenes.empty()) {
        // Create a default scene
        if (context->createScene("DefaultScene")) {
            context->activateScene("DefaultScene");
            OHAO_LOG("Created default scene");
            m_defaultSceneInitialized = true;
            refreshTabsFromContext(context);
        }
    } else {
        m_defaultSceneInitialized = true;
    }
}

void SceneTabViewHelper::renderNewScenePopup(VulkanContext* context) {
    // Only open the popup if the flag is set and it's not already open
    if (m_creatingNewTab) {
        ImGui::OpenPopup("Create New Scene");
    }
    
    // Always set a reasonable size for the popup
    ImGui::SetNextWindowSize(ImVec2(300, 120));
    
    // Begin the popup - this will display it if it was opened above
    bool popupOpen = true;
    if (ImGui::BeginPopupModal("Create New Scene", &popupOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter a name for the new scene:");
        ImGui::InputText("##SceneName", m_newSceneName, sizeof(m_newSceneName));
        
        ImGui::Separator();
        
        // Handle Enter key to confirm
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            std::string name = m_newSceneName;
            if (!name.empty()) {
                createNewScene(context, name);
                m_creatingNewTab = false;
                ImGui::CloseCurrentPopup();
                m_newSceneName[0] = '\0'; // Clear input buffer
            }
        }
        
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
        
        // If popup was closed with X or Escape, reset state
        if (!popupOpen) {
            m_creatingNewTab = false;
            m_newSceneName[0] = '\0';
        }
        
        ImGui::EndPopup();
    } 
    else if (m_creatingNewTab) {
        // If popup failed to open but flag is still set, reset it
        m_creatingNewTab = false;
    }
}

void SceneTabViewHelper::renderRenameScenePopup(VulkanContext* context) {
    // Only open the popup if the flag is set
    if (m_renamingTab) {
        ImGui::OpenPopup("Rename Scene");
    }
    
    // Always set a reasonable size for the popup
    ImGui::SetNextWindowSize(ImVec2(300, 120));
    
    // Begin the popup - this will display it if it was opened above
    bool popupOpen = true;
    if (ImGui::BeginPopupModal("Rename Scene", &popupOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter a new name for the scene:");
        ImGui::InputText("##NewSceneName", m_newSceneName, sizeof(m_newSceneName));
        
        ImGui::Separator();
        
        // Handle Enter key to confirm
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            std::string newName = m_newSceneName;
            if (!newName.empty()) {
                renameCurrentScene(context, newName);
                m_renamingTab = false;
                ImGui::CloseCurrentPopup();
                m_newSceneName[0] = '\0'; // Clear input buffer
            }
        }
        
        if (ImGui::Button("Rename", ImVec2(120, 0))) {
            std::string newName = m_newSceneName;
            if (!newName.empty()) {
                renameCurrentScene(context, newName);
                m_renamingTab = false;
                ImGui::CloseCurrentPopup();
                m_newSceneName[0] = '\0'; // Clear input buffer
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_renamingTab = false;
            ImGui::CloseCurrentPopup();
            m_newSceneName[0] = '\0'; // Clear input buffer
        }
        
        // If popup was closed with X or Escape, reset state
        if (!popupOpen) {
            m_renamingTab = false;
            m_newSceneName[0] = '\0';
        }
        
        ImGui::EndPopup();
    }
    else if (m_renamingTab) {
        // If popup failed to open but flag is still set, reset it
        m_renamingTab = false;
    }
}

} // namespace ohao 