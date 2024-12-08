#pragma once
#include "imgui.h"
#include "renderer/vulkan_context.hpp"
#include "ui/window/window.hpp"
#include <memory>
#include <string>

namespace ohao {


class UIManager {
public:
    UIManager(Window* window, VulkanContext* context);
    ~UIManager();

    void initialize();
    void render();
    bool wantsInputCapture() const;
    bool isSceneViewportHovered() const;
    ViewportSize getSceneViewportSize() const;

private:
    void setupImGuiStyle();
    void initializeVulkanBackend();
    void renderMainMenuBar();
    void renderFileMenu();
    void renderEditMenu();
    void renderViewMenu();
    void renderBuildMenu();
    void renderDebugMenu();
    void renderHelpMenu();
    void renderSceneViewport();
    void enableCursor(bool enable);
    void handleModelImport();
    void shutdownImGui();
    void setupDefaultLayout();

    // Temporary state variables for menu items
    bool showStyleEditor = false;
    bool showMetricsWindow = false;
    bool showAboutWindow = false;
    bool showFileDialog = false;
    bool imguiInitialized{false};
    bool showDemoWindow = true;

    Window* window;
    VulkanContext* vulkanContext;
    ImGuiStyle* style;

    VkDescriptorPool imguiPool{VK_NULL_HANDLE};

    ImVec2 sceneViewportSize{1280, 720};
    bool isSceneWindowHovered{false};
    bool layoutInitialized{false};


};

} // namespace ohao
