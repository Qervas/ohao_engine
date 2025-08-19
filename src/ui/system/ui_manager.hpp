#pragma once
#define GLFW_INCLUDE_VULKAN
#include "imgui.h"
#include "renderer/vulkan_context.hpp"
#include "ui/components/preferences_window.hpp"
#include "ui/panels/outliner/outliner_panel.hpp"
#include "ui/panels/properties/properties_panel.hpp"
#include "ui/panels/scene_settings/scene_settings_panel.hpp"
#include "ui/panels/viewport/viewport_toolbar.hpp"
#include "ui/panels/physics/physics_panel.hpp"
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
    static UIManager* getInstance() { return instance; }
    OutlinerPanel* getOutlinerPanel() const;
    PropertiesPanel* getPropertiesPanel() const;
    SceneSettingsPanel* getSceneSettingsPanel() const;
    ViewportToolbar* getViewportToolbar() const;
    PhysicsPanel* getPhysicsPanel() const;
    void applyTheme(const std::string& theme);

private:

    static constexpr const char* DOCKSPACE_NAME = "OHAO_Dockspace";

    void initializeDockspace();
    void initializeVulkanBackend();
    void setupImGuiStyle();
    void setupPanels();
    void renderPanels();
    void renderMainMenuBar();
    void renderFileMenu();
    void renderEditMenu();
    void renderViewMenu();
    void renderBuildMenu();
    void renderDebugMenu();
    void renderHelpMenu();
    void renderSceneViewport();
    void renderLightIndicators(ImVec2 viewportPos, ImVec2 viewportSize);
    void enableCursor(bool enable);
    void handleModelImport();
    void shutdownImGui();
    void resetLayout();
    void handleNewProject();
    void handleOpenProject();
    bool handleSaveProject();
    bool handleSaveAsProject();
    void handleExit();
    bool showNewProjectDialog();


    // statiac instance pointer
    static UIManager* instance;

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
    bool isDockspaceInitialized{false};

    std::unique_ptr<PreferencesWindow> preferencesWindow;

    std::unique_ptr<OutlinerPanel> outlinerPanel;
    std::unique_ptr<PropertiesPanel> propertiesPanel;
    std::unique_ptr<SceneSettingsPanel> sceneSettingsPanel;
    std::unique_ptr<ViewportToolbar> viewportToolbar;
    std::unique_ptr<PhysicsPanel> physicsPanel;

    std::string currentProjectPath;
    std::string newProjectName;




};

} // namespace ohao
