#pragma once

#include <vector>
#include <string>
#include <memory>
#include "imgui.h"
#include "renderer/vulkan_context.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/imgui/imgui_vulkan_utils.hpp"

namespace ohao {

class SceneViewport {
public:
    SceneViewport();
    ~SceneViewport() = default;

    void render(VulkanContext* context);
    void handleKeyboardShortcuts(VulkanContext* context);
    
    // Viewport size and interaction state
    ImVec2 getViewportSize() const { return m_viewportSize; }
    bool isHovered() const { return m_isHovered; }
    bool isFocused() const { return m_isFocused; }
    
    // Initialize default scene if needed
    void ensureDefaultScene(VulkanContext* context);

private:
    struct SceneTab {
        std::string name;
        bool isActive = false;
        bool isModified = false;
    };

    ImVec2 m_viewportSize{1280, 720};
    bool m_isHovered{false};
    bool m_isFocused{false};
    
    // Scene tabs 
    std::vector<SceneTab> m_sceneTabs;
    int m_activeTabIndex = -1;
    bool m_creatingNewTab = false;
    bool m_renamingTab = false;
    char m_newSceneName[256] = "";
    std::string m_projectPath;
    bool m_defaultSceneInitialized = false;
    
    // Scene tab management
    void renderSceneTabs(VulkanContext* context);
    void activateTab(VulkanContext* context, int index);
    void closeTab(VulkanContext* context, int index);
    void createNewScene(VulkanContext* context, const std::string& name);
    void saveCurrentScene(VulkanContext* context);
    void renameCurrentScene(VulkanContext* context, const std::string& newName);
    void refreshTabsFromContext(VulkanContext* context);
};

} // namespace ohao 