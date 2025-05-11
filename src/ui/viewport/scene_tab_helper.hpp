#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "imgui.h"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"

namespace ohao {

class SceneViewport;

class SceneTabViewHelper {
public:
    SceneTabViewHelper(SceneViewport* viewport);
    ~SceneTabViewHelper() = default;
    
    struct SceneTab {
        std::string name;
        bool isActive = false;
        bool isModified = false;
        std::string filePath; // Path to this scene's file
    };
    
    // Tab rendering and management
    void renderTabs(VulkanContext* context);
    void activateTab(VulkanContext* context, int index);
    void closeTab(VulkanContext* context, int index);
    
    // Scene operations
    bool createNewScene(VulkanContext* context, const std::string& name);
    void saveCurrentScene(VulkanContext* context);
    void loadScene(VulkanContext* context, const std::string& path);
    void renameCurrentScene(VulkanContext* context, const std::string& newName);
    void ensureDefaultScene(VulkanContext* context);
    void refreshTabsFromContext(VulkanContext* context);
    
    // Scene caching
    bool cacheActiveScene(VulkanContext* context);
    bool restoreSceneFromCache(VulkanContext* context, const std::string& sceneName);
    void removeSceneFromCache(const std::string& sceneName);
    
    // Added declaration for new method
    void handleSaveChangesPopup(VulkanContext* context, int destinationTabIndex);
    
    // Getters
    const std::vector<SceneTab>& getTabs() const { return m_sceneTabs; }
    int getActiveTabIndex() const { return m_activeTabIndex; }
    bool isCreatingNewTab() const { return m_creatingNewTab; }
    bool isRenamingTab() const { return m_renamingTab; }
    
    // Setters
    void setActiveTabIndex(int index) { m_activeTabIndex = index; }
    void setCreatingNewTab(bool value) { m_creatingNewTab = value; }
    void setRenamingTab(bool value) { m_renamingTab = value; }
    
private:
    SceneViewport* m_viewport; // Back-reference to the parent viewport
    
    std::vector<SceneTab> m_sceneTabs;
    std::unordered_map<std::string, std::shared_ptr<Scene>> m_cachedScenes; // Cache to prevent scenes from disappearing
    int m_activeTabIndex = -1;
    bool m_creatingNewTab = false;
    bool m_renamingTab = false;
    char m_newSceneName[256] = "";
    bool m_defaultSceneInitialized = false;
    
    // UI helpers
    void renderNewScenePopup(VulkanContext* context);
    void renderRenameScenePopup(VulkanContext* context);
};

} // namespace ohao 