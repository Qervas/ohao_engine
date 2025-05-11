#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include "imgui.h"
#include "renderer/vulkan_context.hpp"
#include "ui/imgui/imgui_vulkan_utils.hpp"
#include "ui/selection/selection_manager.hpp"
#include "ui/panels/outliner/outliner_panel.hpp"
#include "ui/panels/properties/properties_panel.hpp"

namespace ohao {

// Forward declarations
class ProjectViewHelper;
class SceneTabViewHelper;
class OutlinerPanel;
class PropertiesPanel;

// File operation type for save dialogs
enum class FileAction {
    None,
    NewProject,
    OpenProject,
    SaveProject,
    SaveProjectAs,
    CloseProject
};

class SceneViewport {
public:
    SceneViewport();
    ~SceneViewport();

    void render(VulkanContext* context);
    void handleKeyboardShortcuts(VulkanContext* context);
    
    // Viewport size and interaction state
    ImVec2 getViewportSize() const { return m_viewportSize; }
    bool isHovered() const { return m_isHovered; }
    bool isFocused() const { return m_isFocused; }
    
    // Project and scene management
    bool saveProject(VulkanContext* context, bool forceSaveAs = false);
    bool loadProject(VulkanContext* context, const std::string& projectPath = "");
    void closeProject(VulkanContext* context);
    void ensureDefaultScene(VulkanContext* context);
    bool hasProjectPath() const { return !m_projectPath.empty(); }
    bool openStartupProjectDialog(VulkanContext* context);
    
    // Set UI panels for interaction
    void setOutlinerPanel(OutlinerPanel* panel) { m_outlinePanel = panel; }
    void setPropertiesPanel(PropertiesPanel* panel) { m_propertiesPanel = panel; }
    
    // Engine events handling
    void notifySceneChanged(const std::string& sceneName);
    
    // Friend declaration to allow SceneTabViewHelper to access private methods
    friend class SceneTabViewHelper;

private:
    struct SceneTab {
        std::string name;
        bool isActive = false;
        bool isModified = false;
        std::string filePath; // Path to this scene's file
    };

    struct RecentProject {
        std::string name;
        std::string path;
        std::string lastOpened; // Timestamp for sorting
    };

    // Viewport state 
    ImVec2 m_viewportSize{1280, 720};
    bool m_isHovered{false};
    bool m_isFocused{false};
    
    // Project management
    std::string m_projectPath;
    std::string m_projectDir;
    std::string m_projectName;
    std::vector<RecentProject> m_recentProjects;
    const size_t MAX_RECENT_PROJECTS = 10;
    bool m_showStartupDialog = true;
    bool m_projectModified = false;
    
    // Scene tabs 
    std::vector<SceneTab> m_sceneTabs;
    std::unordered_map<std::string, std::shared_ptr<Scene>> m_cachedScenes; // Cache to prevent scenes from disappearing
    int m_activeTabIndex = -1;
    bool m_creatingNewTab = false;
    bool m_renamingTab = false;
    char m_newSceneName[256] = "";
    bool m_defaultSceneInitialized = false;
    
    // View helpers - modularizing the code
    std::unique_ptr<ProjectViewHelper> m_projectHelper;
    std::unique_ptr<SceneTabViewHelper> m_tabHelper;
    
    // UI panels
    OutlinerPanel* m_outlinePanel = nullptr;
    PropertiesPanel* m_propertiesPanel = nullptr;
    
    // Save popup state
    bool m_showSavePopup = false;
    FileAction m_pendingActionAfterSave = FileAction::None;
    std::string m_pendingPath;
    
    // Store the current context
    VulkanContext* m_context = nullptr;
    
    // Scene tab management
    void renderSceneTabs(VulkanContext* context);
    void activateTab(VulkanContext* context, int index);
    void closeTab(VulkanContext* context, int index);
    void createNewScene(VulkanContext* context, const std::string& name);
    bool saveCurrentScene(VulkanContext* context);
    void renameCurrentScene(VulkanContext* context, const std::string& newName);
    void refreshTabsFromContext(VulkanContext* context);
    
    // Scene caching
    bool cacheActiveScene(VulkanContext* context);
    bool restoreSceneFromCache(VulkanContext* context, const std::string& sceneName);
    void removeSceneFromCache(const std::string& sceneName);
    
    // Project management
    void loadRecentProjects();
    void saveRecentProjects();
    void addToRecentProjects(const std::string& projectPath);
    void addRecentProject(const std::string& name, const std::string& path);
    bool renderStartupDialog(VulkanContext* context);
    bool createNewProject(VulkanContext* context);
    std::string getEngineConfigPath() const;

    // Popup and dialog management
    void handlePopups(VulkanContext* context);
    bool handleSaveChangesPopup(VulkanContext* context);
    bool loadProjectFile(VulkanContext* context, const std::string& projectPath);
};

} // namespace ohao 