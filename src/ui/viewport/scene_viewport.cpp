#include "scene_viewport.hpp"
#include "imgui.h"
#include "ui/components/console_widget.hpp"
#include "ui/components/file_dialog.hpp"
#include "core/scene/scene.hpp"
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include "project_view_helper.hpp"
#include "scene_tab_helper.hpp"

namespace ohao {

// Visual notification system for scene switching
namespace {
    struct SceneNotification {
        std::string message;
        float timer;
        ImVec4 color;
    };
    
    std::vector<SceneNotification> activeNotifications;
    
    void addNotification(const std::string& message, const ImVec4& color = ImVec4(0.0f, 0.8f, 0.0f, 1.0f)) {
        SceneNotification notification;
        notification.message = message;
        notification.timer = 5.0f;  // Show for 5 seconds
        notification.color = color;
        activeNotifications.push_back(notification);
        
        // Also log to console
        OHAO_LOG("NOTIFICATION: " + message);
    }
    
    void renderNotifications() {
        if (activeNotifications.empty()) return;
        
        // Update timers and remove expired notifications
        float deltaTime = ImGui::GetIO().DeltaTime;
        for (auto it = activeNotifications.begin(); it != activeNotifications.end();) {
            it->timer -= deltaTime;
            if (it->timer <= 0.0f) {
                it = activeNotifications.erase(it);
            } else {
                ++it;
            }
        }
        
        // Calculate notification area at the center of the viewport
        ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
        float notificationWidth = 400.0f;
        float startX = (viewportSize.x - notificationWidth) * 0.5f;
        float startY = viewportSize.y * 0.25f;  // Position it 25% from the top
        
        // Render each notification
        for (int i = 0; i < activeNotifications.size(); i++) {
            auto& notification = activeNotifications[i];
            
            // Calculate fade based on timer
            float alpha = notification.timer > 1.0f ? 1.0f : notification.timer;
            ImVec4 textColor = notification.color;
            textColor.w = alpha;
            
            // Background with transparency
            ImVec4 bgColor(0.1f, 0.1f, 0.1f, alpha * 0.8f);
            
            // Position each notification below the previous one
            float posY = startY + i * 60.0f;
            
            // Set window position and size
            ImGui::SetNextWindowPos(ImVec2(startX, posY));
            ImGui::SetNextWindowSize(ImVec2(notificationWidth, 0));
            
            // Create a borderless, transparent window
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 10.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, bgColor);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(notification.color.x, notification.color.y, notification.color.z, alpha * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
            
            // Create a window with no decorations
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | 
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            
            if (ImGui::Begin(("##Notification" + std::to_string(i)).c_str(), nullptr, flags)) {
                ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                
                // Use larger font for notifications
                ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
                float originalScale = ImGui::GetFont()->Scale;
                ImGui::GetFont()->Scale *= 1.5f;
                ImGui::PushTextWrapPos(notificationWidth - 30.0f);
                
                // Center the text
                float textWidth = ImGui::CalcTextSize(notification.message.c_str()).x;
                ImGui::TextWrapped("%s", notification.message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::End();
            
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }
    }
}

SceneViewport::SceneViewport()
    : m_viewportSize(1280, 720)
    , m_isHovered(false)
    , m_isFocused(false)
    , m_showStartupDialog(true)
    , m_activeTabIndex(-1)
    , m_creatingNewTab(false)
    , m_renamingTab(false)
    , m_defaultSceneInitialized(false)
{
    // Initialize helper classes
    m_projectHelper = std::make_unique<ProjectViewHelper>(this);
    m_tabHelper = std::make_unique<SceneTabViewHelper>(this);
    
    // Initialize other state
    m_newSceneName[0] = '\0';
    loadRecentProjects();
}

SceneViewport::~SceneViewport()
{
    // Explicitly destroy helpers to control order
    m_tabHelper.reset();
    m_projectHelper.reset();
    
    // Save recent projects list when destroying
    saveRecentProjects();
}

std::string SceneViewport::getEngineConfigPath() const {
    // Get user home directory for storing config
    std::string configPath;
    
    // Check environment variables for home directory
    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        homeDir = getenv("USERPROFILE"); // Windows alternative
    }
    
    if (homeDir) {
        configPath = std::string(homeDir) + "/.ohao_engine";
        
        // Create directory if it doesn't exist
        if (!std::filesystem::exists(configPath)) {
            std::filesystem::create_directories(configPath);
        }
    } else {
        // Fallback to current directory if we can't get home dir
        configPath = "./.ohao_engine";
    }
    
    return configPath;
}

void SceneViewport::ensureDefaultScene(VulkanContext* context) {
    if (!context || m_defaultSceneInitialized) return;
    
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

void SceneViewport::render(VulkanContext* context) {
    // Store the context for use in other methods
    m_context = context;
    
    if (!context) return;
    
    // Show startup dialog if needed
    if (m_showStartupDialog && m_projectPath.empty()) {
        if (renderStartupDialog(context)) {
            // Dialog has been handled and a project was loaded or created
            m_showStartupDialog = false;
        }
        return;
    }
    
    // Check if we need to display popups based on recent actions
    if (!m_projectDir.empty() && m_projectPath.empty()) {
        // If we have a selected directory but no project file, show appropriate popup
        std::filesystem::path dirPath(m_projectDir);
        std::filesystem::path projectFile = dirPath / "project.json";
        
        if (std::filesystem::exists(projectFile)) {
            // Found a project file in the directory
            ImGui::OpenPopup("Found Project");
        } else {
            // No project file, prompt to create new
            ImGui::OpenPopup("Directory Selected");
        }
    }
    
    // Ensure we have a default scene if needed
    ensureDefaultScene(context);
    
    // First, refresh tabs to ensure we're showing current engine state
    m_tabHelper->refreshTabsFromContext(context);
    
    // Set up viewport window styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    
    // Begin the viewport window with tabs
    if (ImGui::Begin("Scene Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Render the scene tabs at the top using the tab helper
        m_tabHelper->renderTabs(context);
        
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
        
        // Project info in the bottom right
        if (!m_projectPath.empty()) {
            std::string projectInfo = "Project: " + m_projectName;
            ImVec2 textSize = ImGui::CalcTextSize(projectInfo.c_str());
            ImGui::SetCursorPos(ImVec2(m_viewportSize.x - textSize.x - 10, m_viewportSize.y - 30));
            ImGui::Text("%s", projectInfo.c_str());
        }

        // Handle keyboard shortcuts when the viewport is focused
        if (m_isFocused) {
            handleKeyboardShortcuts(context);
        }
    }
    
    // Add this near the end of the method, before ImGui::End()
    renderNotifications();
    
    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
    
    // Handle all the popups for project and scene management
    handlePopups(context);
}

void SceneViewport::handleKeyboardShortcuts(VulkanContext* context) {
    ImGuiIO& io = ImGui::GetIO();
    
    // Ctrl+S to save current scene and project
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (!m_projectPath.empty()) {
            OHAO_LOG("Saving project and current scene (Ctrl+S)");
        saveCurrentScene(context);
        }
    }
    
    // Ctrl+Shift+S for "Save As" - force new project location
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S)) {
        OHAO_LOG("Save project as... (Ctrl+Shift+S)");
        saveProject(context, true);
        saveCurrentScene(context);
    }
    
    // Ctrl+O to open project
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        OHAO_LOG("Opening project... (Ctrl+O)");
        // Check if we have an open project with changes
        if (!m_projectPath.empty()) {
            ImGui::OpenPopup("Close Current Project?");
        } else {
            loadProject(context);
        }
    }
    
    // Ctrl+N for new project
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
        OHAO_LOG("Creating new project... (Ctrl+N)");
        
        // Check if we have an open project with changes
        if (!m_projectPath.empty()) {
            ImGui::OpenPopup("Close Current Project?");
        } else {
            createNewProject(context);
        }
    }
    
    // Ctrl+T to create new scene (similar to browser new tab)
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T)) {
        OHAO_LOG("Creating new scene... (Ctrl+T)");
        m_creatingNewTab = true;
        strcpy(m_newSceneName, "NewScene");
    }
    
    // F2 to rename current scene
    if (ImGui::IsKeyPressed(ImGuiKey_F2) && m_activeTabIndex >= 0) {
        OHAO_LOG("Renaming scene... (F2)");
        m_renamingTab = true;
        strcpy(m_newSceneName, m_sceneTabs[m_activeTabIndex].name.c_str());
    }
    
    // ESC to show project selection dialog
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && m_projectPath.empty()) {
        m_showStartupDialog = true;
    }
}

void SceneViewport::handlePopups(VulkanContext* context) {
    // Handle "Directory Selected" popup
    if (ImGui::BeginPopupModal("Directory Selected", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You've selected the following directory:");
        ImGui::Separator();
        
        // Display the selected directory in a highlighted box
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.2f, 1.0f));
        char buffer[512];
        strncpy(buffer, m_projectDir.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        ImGui::InputText("##DirectoryPath", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        
        ImGui::Separator();
        ImGui::Text("Would you like to create a new project in this location?");
        
        // Center the button
        float buttonWidth = 200.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - buttonWidth) * 0.5f;
        if (offset > 0.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        
        if (ImGui::Button("Create Project", ImVec2(buttonWidth, 0))) {
            ImGui::CloseCurrentPopup();
            bool result = createNewProject(context);
            if (result) {
                m_showStartupDialog = false;
                return;
            }
        }
        
        ImGui::EndPopup();
    }
    
    // Handle "Close Current Project?" popup
    if (ImGui::BeginPopupModal("Close Current Project?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Do you want to save changes to the current project before closing?");
        
        if (ImGui::Button("Save and Close", ImVec2(150, 0))) {
            // Save all modified scenes
            for (int i = 0; i < m_sceneTabs.size(); i++) {
                if (m_sceneTabs[i].isModified) {
                    // Temporarily set as active to save
                    int oldActive = m_activeTabIndex;
                    m_activeTabIndex = i;
                    saveCurrentScene(context);
                    m_activeTabIndex = oldActive;
                }
            }
            
            // Update project file
            saveProject(context, false);
            
            // Now close the project
            closeProject(context);
            
            // TODO: If we have a pending project to open, open it here
            
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Close Without Saving", ImVec2(150, 0))) {
            closeProject(context);
            // TODO: If we have a pending project to open, open it here
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
    
    // Handle "Create Project?" popup
    if (ImGui::BeginPopupModal("Create Project?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("No project file found in this directory:");
        ImGui::Separator();
        
        // Display the selected directory in a highlighted box
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.2f, 1.0f));
        char buffer[512];
        strncpy(buffer, m_projectDir.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        ImGui::InputText("##DirectoryPath", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        
        ImGui::Separator();
        ImGui::Text("Would you like to create a new project here?");
        
        // Center the button
        float buttonWidth = 200.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - buttonWidth) * 0.5f;
        if (offset > 0.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        
        if (ImGui::Button("Create Project", ImVec2(buttonWidth, 0))) {
            ImGui::CloseCurrentPopup();
            bool result = createNewProject(context);
            if (result) {
                m_showStartupDialog = false;
                return;
            }
        }
        
        ImGui::EndPopup();
    }
    
    // Handle "Found Project" popup
    if (ImGui::BeginPopupModal("Found Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Found existing project in this directory:");
        ImGui::Separator();
        
        // Display the selected directory in a highlighted box
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.2f, 1.0f));
        char buffer[512];
        strncpy(buffer, m_projectDir.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        ImGui::InputText("##DirectoryPath", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        
        ImGui::Separator();
        ImGui::Text("Would you like to open this project?");
        
        // Center the buttons
        float buttonWidth = 120.0f;
        float totalWidth = buttonWidth * 2 + ImGui::GetStyle().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - totalWidth) * 0.5f;
        if (offset > 0.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        
        if (ImGui::Button("Open Project", ImVec2(buttonWidth, 30))) {
            ImGui::CloseCurrentPopup();
            
            // Load the project from the directory
            std::filesystem::path projectFile = std::filesystem::path(m_projectDir) / "project.json";
            bool result = loadProject(context, projectFile.string());
            if (result) {
                m_showStartupDialog = false;
                // Clear the temporary directory path since we've handled it
                m_projectDir = "";
                return;
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 30))) {
            ImGui::CloseCurrentPopup();
            // Clear the temporary directory path since user canceled
            m_projectDir = "";
        }
        
        ImGui::EndPopup();
    }
    
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
                    m_tabHelper->createNewScene(context, name);
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
    
    // Handle renaming a scene if dialog is open
    if (m_renamingTab) {
        ImGui::OpenPopup("Rename Scene");
        ImGui::SetNextWindowSize(ImVec2(300, 120));
        
        if (ImGui::BeginPopupModal("Rename Scene", &m_renamingTab, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter a new name for the scene:");
            ImGui::InputText("##NewSceneName", m_newSceneName, sizeof(m_newSceneName));
            
            ImGui::Separator();
            
            if (ImGui::Button("Rename", ImVec2(120, 0))) {
                std::string newName = m_newSceneName;
                if (!newName.empty()) {
                    m_tabHelper->renameCurrentScene(context, newName);
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
            
            ImGui::EndPopup();
        }
    }
}

void SceneViewport::activateTab(VulkanContext* context, int index) {
    if (index < 0 || index >= m_sceneTabs.size()) return;
    
    // If this tab is already active, do nothing
    if (m_activeTabIndex == index) {
        return;
    }
    
    // Get the current active tab and check if it has unsaved changes
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size() && 
        m_sceneTabs[m_activeTabIndex].isModified) {
        
        // Ask user if they want to save changes
        ImGui::OpenPopup("Save Changes?");
    } else {
        // No unsaved changes, activate the new tab directly
        
        // Get current scene name before switching
        std::string currentSceneName = "";
        if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
            currentSceneName = m_sceneTabs[m_activeTabIndex].name;
            
            // Cache the current scene before switching to preserve its state
            cacheActiveScene(context);
            
            // Mark the previously active tab as inactive
            m_sceneTabs[m_activeTabIndex].isActive = false;
        }
        
        // Set the new active tab index
        m_activeTabIndex = index;
        m_sceneTabs[index].isActive = true;
        
        // Get the new scene name
        std::string newSceneName = m_sceneTabs[index].name;
        
        // Activate the scene in the context if it's not already active
        std::string activeScene = context->getActiveSceneName();
        if (activeScene != newSceneName) {
            OHAO_LOG("Switching from scene '" + activeScene + "' to '" + newSceneName + "'");
            
            // Check if we have this scene cached - if so, try to restore it
            auto cached = restoreSceneFromCache(context, newSceneName);
            if (!cached) {
                // If not cached, just activate it directly
                bool activated = context->activateScene(newSceneName);
                OHAO_LOG(activated ? "Successfully activated scene directly" : "Failed to activate scene directly");
            }
            
            // Wait for the engine to process the change
            context->updateSceneBuffers();
            
            // Force a UI update to reflect the new scene
            refreshTabsFromContext(context);
        }
        
        // Force focus to the viewport window to ensure the tab is visible
        ImGui::SetNextWindowFocus();
    }
    
    // Handle save changes popup if opened
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
            m_activeTabIndex = index;
            m_sceneTabs[index].isActive = true;
            
            // Activate the scene in the context
            bool activated = context->activateScene(m_sceneTabs[index].name);
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
            m_activeTabIndex = index;
            m_sceneTabs[index].isActive = true;
            
            // Activate the scene in the context
            bool activated = context->activateScene(m_sceneTabs[index].name);
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
    
    // Cache the current scene before creating a new one
    cacheActiveScene(context);
    
    // Mark existing active tab as inactive
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
        m_sceneTabs[m_activeTabIndex].isActive = false;
    }
    
    // Create a new scene in the engine - this will now make it active by default
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
        bool activated = context->activateScene(name);
        OHAO_LOG(activated ? "Successfully activated new scene" : "Failed to activate new scene");
        
        // Force refresh to show correct state
        refreshTabsFromContext(context);
        
        // Force focus to the viewport window to ensure the scene tab is visible
        ImGui::SetWindowFocus("Scene Viewport");
        
        OHAO_LOG("Created new scene: " + name);
    } else {
        OHAO_LOG_ERROR("Failed to create new scene: " + name);
    }
}

bool SceneViewport::saveCurrentScene(VulkanContext* context) {
    // Get current active scene
    OHAO_LOG("saveCurrentScene called");
    auto activeScene = context->getActiveScene();
    std::string activeSceneName = context->getActiveSceneName();
    
    // If we have a scene with content but no name, give it a name
    if (activeScene && activeSceneName.empty()) {
        // Scene exists but doesn't have a proper name in loadedScenes
        OHAO_LOG("Scene exists but doesn't have a registered name, registering it");
        std::string defaultName = "Scene_" + std::to_string(std::time(nullptr));
        activeScene->setName(defaultName);
        
        // Try to make the engine register this scene
        if (context->createNewScene(defaultName)) {
            activeSceneName = defaultName;
            OHAO_LOG("Registered unnamed scene as: " + defaultName);
        }
    }
    // Check if we still don't have an active scene
    else if (activeSceneName.empty()) {
        OHAO_LOG("No active scene, creating default scene for saving");
        ensureDefaultScene(context);
        activeSceneName = context->getActiveSceneName();
        
        // If still no active scene, bail out
        if (activeSceneName.empty()) {
            OHAO_LOG_ERROR("No active scene to save");
            return false;
        }
    }
    
    // If we don't have a project path yet, ask for one
    if (m_projectPath.empty()) {
        // Show start project dialog instead of forcing save-as
        OHAO_LOG_ERROR("Cannot save scene: No project path set");
        m_showStartupDialog = true;
        return false;
    }
    
    // Construct scene save path using the project structure
    std::filesystem::path scenesDir = std::filesystem::path(m_projectDir) / "scenes";
    std::filesystem::path scenePath = scenesDir / (activeSceneName + Scene::FILE_EXTENSION);
    std::string savePath = scenePath.string();
    
    // Make sure the scenes directory exists
    if (!std::filesystem::exists(scenesDir)) {
        try {
        std::filesystem::create_directories(scenesDir);
            OHAO_LOG("Created scenes directory: " + scenesDir.string());
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Failed to create scenes directory: " + std::string(e.what()));
            return false;
        }
    }
    
    // Save the current scene to the project
    if (context->saveSceneToFile(savePath)) {
        OHAO_LOG("Saved scene '" + activeSceneName + "' to: " + savePath);
        
        // Update tab modified state
        if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
            m_sceneTabs[m_activeTabIndex].isModified = false;
            // Store the file path in the tab
            m_sceneTabs[m_activeTabIndex].filePath = savePath;
        }
        
        // Make sure we refresh the tabs to show the saved scene
        refreshTabsFromContext(context);
        
        // Now save the project file to include this scene
        saveProject(context, false);
        
        return true;
    } else {
        OHAO_LOG_ERROR("Failed to save scene to: " + savePath);
        return false;
    }
}

void SceneViewport::renameCurrentScene(VulkanContext* context, const std::string& newName) {
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

void SceneViewport::refreshTabsFromContext(VulkanContext* context) {
    // Get the current active scene name and loaded scenes
    std::string activeScene = context->getActiveSceneName();
    auto loadedScenes = context->getLoadedSceneNames();
    
    OHAO_LOG_DEBUG("Refreshing tabs: active scene = '" + activeScene + 
                   "', loaded scenes count = " + std::to_string(loadedScenes.size()));
    
    // First, remove any tabs for scenes that are no longer loaded
    auto it = m_sceneTabs.begin();
    while (it != m_sceneTabs.end()) {
        if (std::find(loadedScenes.begin(), loadedScenes.end(), it->name) == loadedScenes.end()) {
            // Scene was unloaded, remove the tab
            OHAO_LOG_DEBUG("Removing tab for unloaded scene: " + it->name);
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
            OHAO_LOG_DEBUG("Adding new tab for scene: " + sceneName);
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
        
        // Only log if state is changing
        if (m_sceneTabs[i].isActive != isActiveScene) {
            OHAO_LOG_DEBUG("Tab state change: '" + m_sceneTabs[i].name + 
                           "' isActive changing from " + 
                           (m_sceneTabs[i].isActive ? "true" : "false") + 
                           " to " + (isActiveScene ? "true" : "false"));
        }
        
        m_sceneTabs[i].isActive = isActiveScene;
        if (isActiveScene) {
            m_activeTabIndex = i;
            foundActiveTab = true;
            m_sceneTabs[i].isModified = context->hasUnsavedChanges();
        }
    }
    
    // If no active tab was found but we have tabs, set the first one as active
    if (!foundActiveTab && !m_sceneTabs.empty() && m_activeTabIndex < 0) {
        OHAO_LOG("No active tab found but have tabs - setting first tab active");
        m_activeTabIndex = 0;
        m_sceneTabs[0].isActive = true;
        
        // Make sure the engine state matches
        bool activated = context->activateScene(m_sceneTabs[0].name);
        OHAO_LOG(activated ? "Successfully activated first scene" : "Failed to activate first scene");
    }
    
    // Consistency check to make sure UI state is sane
    if (m_activeTabIndex >= 0 && m_activeTabIndex < m_sceneTabs.size()) {
        if (!m_sceneTabs[m_activeTabIndex].isActive) {
            OHAO_LOG_ERROR("Tab consistency error: activeTabIndex is " + 
                           std::to_string(m_activeTabIndex) + 
                           " but that tab is not marked as active. Fixing.");
            m_sceneTabs[m_activeTabIndex].isActive = true;
        }
    }
}

bool SceneViewport::cacheActiveScene(VulkanContext* context) {
    // Get the current active scene
    auto activeScene = context->getActiveScene();
    std::string sceneName = context->getActiveSceneName();
    
    // Don't cache if no active scene
    if (!activeScene || sceneName.empty()) {
        OHAO_LOG_WARNING("Cannot cache scene: No active scene or empty scene name");
        return false;
    }
    
    // Store a copy of the scene in our cache
    m_cachedScenes[sceneName] = activeScene;
    OHAO_LOG("Cached scene: " + sceneName);
    return true;
}

bool SceneViewport::restoreSceneFromCache(VulkanContext* context, const std::string& sceneName) {
    // Check if the scene exists in our cache
    auto it = m_cachedScenes.find(sceneName);
    if (it == m_cachedScenes.end()) {
        OHAO_LOG_DEBUG("Scene not found in cache: " + sceneName);
        return false;
    }
    
    // Get the cached scene
    auto cachedScene = it->second;
    if (!cachedScene) {
        OHAO_LOG_WARNING("Cached scene is null: " + sceneName);
        return false;
    }
    
    OHAO_LOG("Restoring scene '" + sceneName + "' from cache");
    
    // Check if the scene exists in loaded scenes
    if (context->isSceneLoaded(sceneName)) {
        // Just activate it, but ensure the state is synchronized
        context->activateScene(sceneName);
        
        // We've successfully activated the scene
        OHAO_LOG("Activated existing scene from context: " + sceneName);
        return true;
    }
    
    // If the scene doesn't exist in the engine, we need to add it
    // This should be extremely rare since scenes should be in loadedScenes
    OHAO_LOG_WARNING("Scene not found in context, adding from cache: " + sceneName);
    
    // Add the cached scene to the loaded scenes
    if (context->createScene(sceneName)) {
        // Now activate the scene
        context->activateScene(sceneName);
        OHAO_LOG("Created and activated scene from cache: " + sceneName);
        return true;
    }
    
    OHAO_LOG_ERROR("Failed to restore scene from cache: " + sceneName);
    return false;
}

void SceneViewport::removeSceneFromCache(const std::string& sceneName) {
    m_cachedScenes.erase(sceneName);
}

bool SceneViewport::saveProject(VulkanContext* context, bool forceSaveAs) {
    // If we need to ask for a project path (first save or Save As)
    if (forceSaveAs || m_projectPath.empty()) {
        // Use the dedicated method to select a directory
        std::string selectedDir = FileDialog::selectDirectory(
            "Select Project Directory",
            ""
        );
        
        if (selectedDir.empty()) {
            return false; // User canceled
        }
        
        // Store the selected directory path immediately
        m_projectDir = selectedDir;
        
        // Create the project.json path
        std::filesystem::path dirPath(selectedDir);
        std::filesystem::path projectFile = dirPath / "project.json";
        m_projectPath = projectFile.string();
        
        // Check if project.json already exists
        if (std::filesystem::exists(projectFile)) {
            // Ask if user wants to overwrite the existing project
            ImGui::OpenPopup("Overwrite Project?");
            return false;
        }
        
        // If this is a new project, ask for a name
        if (m_projectName.empty()) {
            ImGui::OpenPopup("Project Name");
            strcpy(m_newSceneName, "New Project");
        }
    }
    
    // Handle project name popup if it was opened
    if (ImGui::BeginPopupModal("Project Name", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter a name for your project:");
        ImGui::InputText("##ProjectName", m_newSceneName, sizeof(m_newSceneName));
        
        // Display the selected directory
        ImGui::Text("Project Directory: %s", m_projectDir.c_str());
        
        ImGui::Separator();
        
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            m_projectName = m_newSceneName;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
    
    // Save the project file itself
    try {
        // Create project file based on JSON
        nlohmann::json projectJson;
        
        // Make sure we have a valid project name
        if (m_projectName.empty()) {
            // Use directory name as project name if not set
            std::filesystem::path dirPath(m_projectDir);
            m_projectName = dirPath.filename().string();
            
            if (m_projectName.empty()) {
                m_projectName = "OHAO Project";
            }
            
            OHAO_LOG("Setting project name to: " + m_projectName);
        }
        
        // Add project metadata
        projectJson["name"] = m_projectName;
        projectJson["version"] = "1.0";
        projectJson["createdAt"] = std::time(nullptr);
        projectJson["lastModified"] = std::time(nullptr);
        projectJson["engine"] = "OHAO Engine";
        
        // Add scenes list
        nlohmann::json scenesJson = nlohmann::json::array();
        
        // Get current scene names
        for (const auto& tab : m_sceneTabs) {
            nlohmann::json sceneJson;
            sceneJson["name"] = tab.name;
            
            // Use relative paths from project directory for scene files
            std::string relativePath = "scenes/" + tab.name + Scene::FILE_EXTENSION;
            sceneJson["path"] = relativePath;
            sceneJson["active"] = tab.isActive;
            scenesJson.push_back(sceneJson);
        }
        
        projectJson["scenes"] = scenesJson;
        
        // Write to file
        std::ofstream projectFile(m_projectPath);
        if (!projectFile.is_open()) {
            OHAO_LOG_ERROR("Failed to open project file for writing: " + m_projectPath);
            return false;
        }
        
        projectFile << std::setw(4) << projectJson;
        projectFile.close();
        
        // Add to recent projects list
        addToRecentProjects(m_projectPath);
        
        OHAO_LOG("Saved project to: " + m_projectPath);
        return true;
    }
    catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to save project: " + std::string(e.what()));
        return false;
    }
}

bool SceneViewport::loadProject(VulkanContext* context, const std::string& projectPath) {
    if (!context) return false;
    
    try {
        // Load project file
        m_projectPath = projectPath;
        nlohmann::json projectJson;
        
        std::ifstream projectFile(m_projectPath);
        if (!projectFile.is_open()) {
            OHAO_LOG_ERROR("Failed to open project file: " + m_projectPath);
            return false;
        }
        
        projectFile >> projectJson;
        
        // Parse basic project info
        m_projectName = projectJson.value("name", "");
        m_projectDir = std::filesystem::path(m_projectPath).parent_path().string();
        
        // If project name is not specified, use directory name
        if (m_projectName.empty()) {
            m_projectName = std::filesystem::path(m_projectDir).filename().string();
            if (m_projectName.empty()) {
                m_projectName = "OHAO Project";
            }
            OHAO_LOG("Using directory name as project name: " + m_projectName);
        }
        
        OHAO_LOG("Loading project: " + m_projectName + " from " + m_projectDir);
        
        // First detach all UI panels and selections from active scenes to prevent dangling references
        if (m_outlinePanel) {
            m_outlinePanel->setScene(nullptr);
        }
        // Clear any selections
        SelectionManager::get().clearSelection();
        
        // Clear existing scenes one by one
        auto sceneNames = context->getLoadedSceneNames();
        for (const auto& name : sceneNames) {
            context->closeScene(name);
        }
        m_sceneTabs.clear();
        m_cachedScenes.clear();
        
        // Check if scenes array exists
        if (projectJson.contains("scenes") && projectJson["scenes"].is_array()) {
            // Load each scene
            std::string activeSceneName;
            
            for (const auto& sceneJson : projectJson["scenes"]) {
                std::string sceneName = sceneJson.value("name", "");
                std::string scenePath = sceneJson.value("path", "");
                bool isActive = sceneJson.value("active", false);
                
                // Skip invalid scenes
                if (sceneName.empty() || scenePath.empty()) {
                    continue;
                }
                
                // Make path absolute if it's relative
                std::filesystem::path fullScenePath = scenePath;
                if (fullScenePath.is_relative()) {
                    fullScenePath = std::filesystem::path(m_projectDir) / scenePath;
                }
                
                OHAO_LOG("Loading scene: " + sceneName + " from " + fullScenePath.string());
                
                // Load the scene
                if (std::filesystem::exists(fullScenePath)) {
                    if (context->loadSceneFromFile(fullScenePath.string())) {
                        // Add scene tab
                        SceneTab tab;
                        tab.name = sceneName;
                        tab.isActive = isActive;
                        tab.filePath = fullScenePath.string();
                        m_sceneTabs.push_back(tab);
                        
                        // Remember active scene
                        if (isActive) {
                            activeSceneName = sceneName;
                        }
                        
                        OHAO_LOG("Successfully loaded scene: " + sceneName);
                    } else {
                        OHAO_LOG_ERROR("Failed to load scene: " + fullScenePath.string());
                    }
                } else {
                    OHAO_LOG_WARNING("Scene file not found: " + fullScenePath.string());
                }
            }
            
            // Activate the specified scene or the first one
            if (!activeSceneName.empty()) {
                OHAO_LOG("Activating scene: " + activeSceneName);
                context->activateScene(activeSceneName);
            } else if (!m_sceneTabs.empty()) {
                activeSceneName = m_sceneTabs[0].name;
                OHAO_LOG("Activating first scene: " + activeSceneName);
                context->activateScene(activeSceneName);
                m_sceneTabs[0].isActive = true;
            } else {
                // No scenes in project, create a default one
                ensureDefaultScene(context);
            }
            
            // Force buffer update for the active scene
            context->updateSceneBuffers();
        } else {
            // No scenes section in project file, create default scene
            ensureDefaultScene(context);
        }
        
        // Reconnect UI once everything is loaded
        auto activeScene = context->getActiveScene();
        if (activeScene && m_outlinePanel) {
            m_outlinePanel->setScene(activeScene.get());
        }
        
        // Add to recent projects
        addToRecentProjects(m_projectPath);
        
        // Refresh tabs from context to make sure everything is in sync
        refreshTabsFromContext(context);
        
        // Hide startup dialog if it was showing
        m_showStartupDialog = false;
        
        OHAO_LOG("Successfully opened project: " + m_projectPath);
        return true;
    }
    catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to load project: " + std::string(e.what()));
        return false;
    }
}

void SceneViewport::loadRecentProjects() {
    std::string configFile = getEngineConfigPath() + "/recent_projects.json";
    
    // Clear existing list
    m_recentProjects.clear();
    
    if (std::filesystem::exists(configFile)) {
        try {
            std::ifstream file(configFile);
            if (file.is_open()) {
                nlohmann::json json;
                file >> json;
                
                if (json.contains("recentProjects") && json["recentProjects"].is_array()) {
                    for (const auto& projectJson : json["recentProjects"]) {
                        RecentProject project;
                        project.name = projectJson.value("name", "");
                        project.path = projectJson.value("path", "");
                        project.lastOpened = projectJson.value("lastOpened", "");
                        
                        // Only add valid projects with existing files
                        if (!project.path.empty() && std::filesystem::exists(project.path)) {
                            m_recentProjects.push_back(project);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Failed to load recent projects: " + std::string(e.what()));
        }
    }
}

void SceneViewport::saveRecentProjects() {
    std::string configFile = getEngineConfigPath() + "/recent_projects.json";
    
    try {
        nlohmann::json json;
        nlohmann::json projectsArray = nlohmann::json::array();
        
        for (const auto& project : m_recentProjects) {
            nlohmann::json projectJson;
            projectJson["name"] = project.name;
            projectJson["path"] = project.path;
            projectJson["lastOpened"] = project.lastOpened;
            projectsArray.push_back(projectJson);
        }
        
        json["recentProjects"] = projectsArray;
        
        std::ofstream file(configFile);
        if (file.is_open()) {
            file << std::setw(4) << json;
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to save recent projects: " + std::string(e.what()));
    }
}

void SceneViewport::addToRecentProjects(const std::string& projectPath) {
    if (projectPath.empty() || !std::filesystem::exists(projectPath)) {
        OHAO_LOG_ERROR("Cannot add project to recent list: path is empty or doesn't exist: " + projectPath);
        return;
    }
    
    OHAO_LOG("Adding project to recent list: " + projectPath);
    
    // Get current timestamp for sorting
    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&nowTime), "%Y-%m-%d %H:%M:%S");
    std::string timestamp = ss.str();
    
    // Try to read the project name from project.json
    std::string projectName = "";
    
    try {
        std::ifstream projectFile(projectPath);
        if (projectFile.is_open()) {
            nlohmann::json projectJson;
            projectFile >> projectJson;
            
            // Get the project name from the JSON file
            if (projectJson.contains("name") && projectJson["name"].is_string()) {
                projectName = projectJson["name"];
                OHAO_LOG("Read project name from JSON: " + projectName);
            }
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to read project name from JSON: " + std::string(e.what()));
    }
    
    // If we couldn't get the name from the file, use the current project name or fallback
    if (projectName.empty()) {
        if (!m_projectName.empty()) {
            projectName = m_projectName;
            OHAO_LOG("Using current project name: " + projectName);
        } else {
            // Extract name from parent directory, not the filename
            std::filesystem::path projectParentDir = std::filesystem::path(projectPath).parent_path();
            projectName = projectParentDir.filename().string();
            OHAO_LOG("Using parent directory as project name: " + projectName);
            
            if (projectName.empty()) {
                projectName = "Unnamed Project";
                OHAO_LOG("Using fallback project name: " + projectName);
            }
        }
    }
    
    // Check if project already exists in list
    auto it = std::find_if(m_recentProjects.begin(), m_recentProjects.end(),
        [&projectPath](const RecentProject& p) { return p.path == projectPath; });
    
    if (it != m_recentProjects.end()) {
        // Update existing entry
        it->lastOpened = timestamp;
        it->name = projectName; // Update the name in case it changed
        
        OHAO_LOG("Updated existing project in recent list: " + projectName);
        
        // Move to front of list (most recent)
        std::rotate(m_recentProjects.begin(), it, it + 1);
    } else {
        // Add new project to list
        RecentProject project;
        project.name = projectName;
        project.path = projectPath;
        project.lastOpened = timestamp;
        
        OHAO_LOG("Added new project to recent list: " + projectName);
        
        // Insert at beginning
        m_recentProjects.insert(m_recentProjects.begin(), project);
        
        // Truncate list if too long
        if (m_recentProjects.size() > MAX_RECENT_PROJECTS) {
            m_recentProjects.resize(MAX_RECENT_PROJECTS);
        }
    }
    
    // Save changes
    saveRecentProjects();
}

bool SceneViewport::openStartupProjectDialog(VulkanContext* context) {
    // Return false if we already have a project or the dialog is not visible
    if (!m_showStartupDialog || !m_projectPath.empty()) {
        return false;
    }
    
    // Show the startup dialog
    bool result = renderStartupDialog(context);
    return result;
}

bool SceneViewport::renderStartupDialog(VulkanContext* context) {
    ImGui::OpenPopup("OHAO Engine Startup");
    
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 400));
    
    bool result = false;
    
    if (ImGui::BeginPopupModal("OHAO Engine Startup", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        // If we have a selected directory but no project path yet, show the directory confirmation UI
        if (!m_projectDir.empty() && m_projectPath.empty()) {
            ImGui::Text("Selected Directory:");
            ImGui::Separator();
            
            // Display the directory in a highlighted box
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.2f, 1.0f));
            char buffer[512];
            strncpy(buffer, m_projectDir.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            ImGui::InputText("##DirectoryPath", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            
            ImGui::Separator();
            
            // Check if there's a project.json file in the directory
            std::filesystem::path dirPath(m_projectDir);
            std::filesystem::path projectFile = dirPath / "project.json";
            
            if (std::filesystem::exists(projectFile)) {
                ImGui::Text("Found an existing project in this directory.");
                ImGui::Text("Would you like to open it?");
                
                // Center the buttons
                float buttonWidth = 120.0f;
                float totalWidth = buttonWidth * 2 + ImGui::GetStyle().ItemSpacing.x;
                float avail = ImGui::GetContentRegionAvail().x;
                float offset = (avail - totalWidth) * 0.5f;
                if (offset > 0.0f) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                }
                
                if (ImGui::Button("Open Project", ImVec2(buttonWidth, 30))) {
                    result = loadProject(context, projectFile.string());
                    if (result) {
                        m_projectDir = ""; // Clear temp directory
                        m_showStartupDialog = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(buttonWidth, 30))) {
                    m_projectDir = ""; // Clear the stored directory
                }
            } else {
                ImGui::Text("No project found in this directory.");
                ImGui::Text("Would you like to create a new project here?");
                
                // Center the buttons
                float buttonWidth = 120.0f;
                float totalWidth = buttonWidth * 2 + ImGui::GetStyle().ItemSpacing.x;
                float avail = ImGui::GetContentRegionAvail().x;
                float offset = (avail - totalWidth) * 0.5f;
                if (offset > 0.0f) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                }
                
                if (ImGui::Button("Create Project", ImVec2(buttonWidth, 30))) {
                    // Use the directory name as default project name
                    std::filesystem::path dirPath(m_projectDir);
                    m_projectName = dirPath.filename().string();
                    if (m_projectName.empty()) {
                        m_projectName = "New Project";
                    }
                    
                    // Keep the project dir as already set
                    // m_projectDir is already set at this point
                    m_projectPath = (std::filesystem::path(m_projectDir) / "project.json").string();
                    
                    OHAO_LOG("Creating project: " + m_projectName + " at " + m_projectDir);
                    
                    // Create project directories
                    try {
                        if (!std::filesystem::exists(m_projectDir)) {
                            std::filesystem::create_directories(m_projectDir);
                        }
                        
                        // Create standard subdirectories
                        std::filesystem::create_directories(m_projectDir + "/scenes");
                        std::filesystem::create_directories(m_projectDir + "/assets");
                        std::filesystem::create_directories(m_projectDir + "/textures");
                        std::filesystem::create_directories(m_projectDir + "/models");
                        
                        // Create a default scene
                        ensureDefaultScene(context);
                        
                        // Save project files
                        bool projectSaved = saveProject(context, false);
                        bool sceneSaved = saveCurrentScene(context);
                        
                        if (projectSaved && sceneSaved) {
                            // Add to recent projects
                            addToRecentProjects(m_projectPath);
                            
                            // Clear temp directory as we're done with it
                            std::string tempDir = m_projectDir;
                            m_projectDir = ""; // Clear temp variable
                            
                            // Close the dialog and return true
                            m_showStartupDialog = false;
                            ImGui::CloseCurrentPopup();
                            
                            OHAO_LOG("Project created successfully at: " + tempDir);
                            result = true;
                        } else {
                            OHAO_LOG_ERROR("Failed to save project or scene");
                        }
                    } catch (const std::exception& e) {
                        OHAO_LOG_ERROR("Failed to create project: " + std::string(e.what()));
                    }
                }
            }
        }
        else {
            // Regular startup dialog content
            ImGui::Text("Welcome to OHAO Engine!");
            ImGui::Separator();
            
            ImGui::Text("Choose an option:");
            
            ImGui::Separator();
            
            // Recent Projects List
            static int selectedProject = -1;
            if (ImGui::BeginListBox("##RecentProjects", ImVec2(-FLT_MIN, 200))) {
                if (m_recentProjects.empty()) {
                    ImGui::Text("No recent projects");
                } else {
                    for (int i = 0; i < m_recentProjects.size(); i++) {
                        const bool is_selected = (selectedProject == i);
                        
                        // Format display string with project name and date
                        std::string displayName = m_recentProjects[i].name;
                        std::string displayPath = m_recentProjects[i].path;
                        std::string displayDate = m_recentProjects[i].lastOpened;
                        
                        // Truncate path if it's too long
                        if (displayPath.length() > 40) {
                            displayPath = "..." + displayPath.substr(displayPath.length() - 40);
                        }
                        
                        // Combine project name and last opened date
                        std::string selectionText = displayName;
                        
                        if (ImGui::Selectable(selectionText.c_str(), is_selected)) {
                            selectedProject = i;
                        }
                        
                        // Show path and date as tooltip on hover
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Path: %s", displayPath.c_str());
                            ImGui::Text("Last opened: %s", displayDate.c_str());
                            ImGui::EndTooltip();
                        }
                        
                        // Set the initial focus when opening the combo
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                }
                ImGui::EndListBox();
            }
            
            if (selectedProject >= 0 && selectedProject < m_recentProjects.size()) {
                if (ImGui::Button("Open Selected Project", ImVec2(200, 0))) {
                    std::string path = m_recentProjects[selectedProject].path;
                    result = loadProject(context, path);
                    if (result) {
                        m_showStartupDialog = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            
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
                std::string selectedDir = FileDialog::selectDirectory(
                    "Select Project Directory",
                    ""
                );
                
                if (!selectedDir.empty()) {
                    OHAO_LOG("Directory selected in welcome dialog: " + selectedDir);
                    m_projectDir = selectedDir;
                    // Will show directory UI on next frame
                    return false;
                }
            }
        }
        
        ImGui::EndPopup();
    }
    
    return result;
}

bool SceneViewport::createNewProject(VulkanContext* context) {
    OHAO_LOG("createNewProject called with projectDir = " + (m_projectDir.empty() ? "empty" : m_projectDir));
    
    // Get directory for the new project if not already selected
    std::string projectDir = m_projectDir;
    if (projectDir.empty()) {
        OHAO_LOG("No projectDir provided, opening directory selection dialog");
        projectDir = FileDialog::selectDirectory(
            "Select Project Directory",
            ""
        );
        
        OHAO_LOG("Directory selection result: " + (projectDir.empty() ? "canceled" : projectDir));
        if (projectDir.empty()) {
            return false; // User canceled
        }
    }
    
    // Create a clean project directory path (handle click on directory itself)
    std::filesystem::path dirPath(projectDir);
    projectDir = dirPath.string();
    OHAO_LOG("Using project directory: " + projectDir);
    
    // Initialize project name from directory if needed
    std::string defaultProjectName = dirPath.filename().string();
    if (defaultProjectName.empty()) {
        defaultProjectName = "New Project";
    }
    
    // Set up project paths immediately to avoid losing path info
    m_projectName = defaultProjectName;
    m_projectDir = projectDir;
    m_projectPath = (std::filesystem::path(projectDir) / "project.json").string();
    
    OHAO_LOG("Creating new project: Name=" + m_projectName + ", Dir=" + m_projectDir + ", Path=" + m_projectPath);
    
    // Create project directory structure
    try {
        // Create project folder if it doesn't exist
        if (!std::filesystem::exists(m_projectDir)) {
            OHAO_LOG("Creating project directory: " + m_projectDir);
            std::filesystem::create_directories(m_projectDir);
        }
        
        // Create standard subdirectories
        std::filesystem::create_directories(m_projectDir + "/scenes");
        std::filesystem::create_directories(m_projectDir + "/assets");
        std::filesystem::create_directories(m_projectDir + "/textures");
        std::filesystem::create_directories(m_projectDir + "/models");
        
        OHAO_LOG("Created project directory structure at: " + m_projectDir);
        
        // Create a default scene and save everything
        ensureDefaultScene(context);
        OHAO_LOG("Created default scene");
        
        bool projectSaved = saveProject(context, false);
        OHAO_LOG("Project saved: " + std::string(projectSaved ? "success" : "failure"));
        
        bool sceneSaved = saveCurrentScene(context);
        OHAO_LOG("Scene saved: " + std::string(sceneSaved ? "success" : "failure"));
        
        // Add to recent projects
        addToRecentProjects(m_projectPath);
        
        m_showStartupDialog = false; // Hide the startup dialog
        OHAO_LOG("Project created successfully");
        return true;
        
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to create project directories: " + std::string(e.what()));
        return false;
    }
}

void SceneViewport::closeProject(VulkanContext* context) {
    // Ask to save any unsaved changes before closing
    bool hasUnsavedChanges = false;
    for (const auto& tab : m_sceneTabs) {
        if (tab.isModified) {
            hasUnsavedChanges = true;
            break;
        }
    }
    
    if (hasUnsavedChanges) {
        ImGui::OpenPopup("Save Project Changes?");
    } else {
        // No unsaved changes, just close
        m_projectPath = "";
        m_projectDir = "";
        m_projectName = "";
        
        // Close all scenes
        auto sceneNames = context->getLoadedSceneNames();
        for (const auto& name : sceneNames) {
            context->closeScene(name);
        }
        
        m_sceneTabs.clear();
        m_activeTabIndex = -1;
        m_defaultSceneInitialized = false;
        
        // Clear scene caches
        m_cachedScenes.clear();
    }
    
    // Handle save changes popup
    bool open = true;
    if (ImGui::BeginPopupModal("Save Project Changes?", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("There are unsaved changes in your project. Save before closing?");
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            // Save all modified scenes
            for (int i = 0; i < m_sceneTabs.size(); i++) {
                if (m_sceneTabs[i].isModified) {
                    // Temporarily set as active to save
                    int oldActive = m_activeTabIndex;
                    m_activeTabIndex = i;
                    saveCurrentScene(context);
                    m_activeTabIndex = oldActive;
                }
            }
            
            // Update project file
            saveProject(context, false);
            
            // Now close the project
            m_projectPath = "";
            m_projectDir = "";
            m_projectName = "";
            
            // Close all scenes
            auto sceneNames = context->getLoadedSceneNames();
            for (const auto& name : sceneNames) {
                context->closeScene(name);
            }
            
            m_sceneTabs.clear();
            m_activeTabIndex = -1;
            m_defaultSceneInitialized = false;
            
            // Clear scene caches
            m_cachedScenes.clear();
            
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            // Close without saving
            m_projectPath = "";
            m_projectDir = "";
            m_projectName = "";
            
            // Close all scenes
            auto sceneNames = context->getLoadedSceneNames();
            for (const auto& name : sceneNames) {
                context->closeScene(name);
            }
            
            m_sceneTabs.clear();
            m_activeTabIndex = -1;
            m_defaultSceneInitialized = false;
            
            // Clear scene caches
            m_cachedScenes.clear();
            
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

// Add notification when scene is changed
void SceneViewport::notifySceneChanged(const std::string& sceneName) {
    // Get the context from the parameter that is passed to the render method
    VulkanContext* context = m_context;
    if (!context) {
        OHAO_LOG_ERROR("Cannot notify scene change: context is null");
        return;
    }
    
    // Update the outliner panel to show the new scene's objects
    if (m_outlinePanel) {
        m_outlinePanel->setScene(context->getScene());
    }
    
    // Update properties panel
    if (m_propertiesPanel) {
        m_propertiesPanel->setScene(context->getScene());
    }
    
    // Show a notification about the scene change
    std::string notificationText = "Switched to scene: " + sceneName;
    addNotification(notificationText);
}

} // namespace ohao 