#include "ui/system/ui_manager.hpp"
#include "console_widget.hpp"
#include "imgui.h"
#define GLFW_INCLUDE_VULKAN
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <renderer/rhi/vk/ohao_vk_texture_handle.hpp>
#include "layout_manager.hpp"
#include "window/window.hpp"
#include "components/file_dialog.hpp"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <vulkan/vulkan_core.h>
#include "imgui.h"
#include "imgui_vulkan_utils.hpp"

namespace ohao {

// initialize static instance pointer
UIManager* UIManager::instance = nullptr;


UIManager::UIManager(Window* window, VulkanContext* context)
    : window(window), vulkanContext(context) {

    instance = this;
    preferencesWindow = std::make_unique<PreferencesWindow>();
}

UIManager::~UIManager(){
    if (vulkanContext && vulkanContext->getLogicalDevice()) {
        vulkanContext->getLogicalDevice()->waitIdle();
    }
    if(instance == this){
        instance = nullptr;
    }
    shutdownImGui();
}

void UIManager::initialize() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Enable multi-viewport

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForVulkan(window->getGLFWWindow(), true);
    
    // Set up the Platform_CreateVkSurface callback for multi-viewport support
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_CreateVkSurface = [](ImGuiViewport* viewport, ImU64 vk_instance, const void* vk_allocator, ImU64* out_vk_surface) -> int {
        VkSurfaceKHR surface;
        VkResult err = glfwCreateWindowSurface(
            (VkInstance)vk_instance,
            (GLFWwindow*)viewport->PlatformHandle,
            (const VkAllocationCallbacks*)vk_allocator,
            &surface);
        if (err != VK_SUCCESS)
            return err;
        *out_vk_surface = (ImU64)surface;
        return VK_SUCCESS;
    };
    
    initializeVulkanBackend();

    // Load and apply preferences before setting up ImGui
    auto& prefs = Preferences::get();
    const auto& appearance = prefs.getAppearance();

    applyTheme(appearance.theme);
    io.FontGlobalScale = appearance.uiScale;

    setupImGuiStyle();
    setupPanels();

    OHAO_LOG_DEBUG("UI Manager initialized with preferences:");
    OHAO_LOG_DEBUG("Theme: " + appearance.theme);
    OHAO_LOG_DEBUG("UI Scale: " + std::to_string(appearance.uiScale));
    OHAO_LOG_DEBUG("Docking: " + std::string(appearance.enableDocking ? "enabled" : "disabled"));
    OHAO_LOG_DEBUG("Viewports: " + std::string(appearance.enableViewports ? "enabled" : "disabled"));
}

void UIManager::applyTheme(const std::string& theme) {
    if (theme == "Dark") {
        ImGui::StyleColorsDark();
        OHAO_LOG_DEBUG("Applied Dark theme");
    } else if (theme == "Light") {
        ImGui::StyleColorsLight();
        OHAO_LOG_DEBUG("Applied Light theme");
    } else if (theme == "Classic") {
        ImGui::StyleColorsClassic();
        OHAO_LOG_DEBUG("Applied Classic theme");
    }

    // After applying the base theme, apply any custom style modifications
    ImGuiStyle& style = ImGui::GetStyle();

    // Ensure colors are properly set based on the theme
    ImVec4* colors = style.Colors;
    if (theme == "Dark") {
        colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
        colors[ImGuiCol_Border] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    }
}

void UIManager::setupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Main styles
    style.WindowPadding            = ImVec2(8.00f, 8.00f);
    style.FramePadding            = ImVec2(5.00f, 2.00f);
    style.CellPadding             = ImVec2(6.00f, 6.00f);
    style.ItemSpacing             = ImVec2(6.00f, 6.00f);
    style.ItemInnerSpacing        = ImVec2(6.00f, 6.00f);
    style.TouchExtraPadding       = ImVec2(0.00f, 0.00f);
    style.IndentSpacing           = 25;
    style.ScrollbarSize           = 12;
    style.GrabMinSize            = 10;
    style.WindowBorderSize        = 1;
    style.ChildBorderSize         = 1;
    style.PopupBorderSize         = 1;
    style.FrameBorderSize         = 1;
    style.TabBorderSize           = 1;
    style.WindowRounding          = 7;
    style.ChildRounding          = 4;
    style.FrameRounding          = 3;
    style.PopupRounding          = 4;
    style.ScrollbarRounding      = 9;
    style.GrabRounding          = 3;
    style.LogSliderDeadzone      = 4;
    style.TabRounding            = 4;
}

void UIManager::initializeVulkanBackend(){
    ImGui_ImplVulkan_LoadFunctions(
        VK_API_VERSION_1_3,
        [](const char* function_name, void* user_data) -> PFN_vkVoidFunction {
            return vkGetInstanceProcAddr(static_cast<VkInstance>(user_data), function_name);
        },
        vulkanContext->getVkInstance()
    );

    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(vulkanContext->getVkDevice(), &pool_info, nullptr, &imguiPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create ImGui descriptor pool!");
    }

    // Initialize ImGui Vulkan implementation
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = vulkanContext->getVkInstance();
    init_info.PhysicalDevice = vulkanContext->getVkPhysicalDevice();
    init_info.Device = vulkanContext->getVkDevice();
    init_info.QueueFamily = vulkanContext->getLogicalDevice()->getQueueFamilyIndices().graphicsFamily.value();
    init_info.Queue = vulkanContext->getLogicalDevice()->getGraphicsQueue();
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = imguiPool;
    init_info.MinImageCount = 2;
    init_info.ImageCount = vulkanContext->getSwapChain()->getImages().size();
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = nullptr;
    init_info.RenderPass = vulkanContext->getVkRenderPass();

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("Failed to initialize ImGui Vulkan implementation!");
    }


    // Upload fonts - modern ImGui handles this automatically
    // The font atlas is automatically uploaded when needed
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Build();

    imguiInitialized = true;
}

void UIManager::shutdownImGui(){
    if (imguiInitialized) {
        // Wait for device to be idle before cleanup
        if (vulkanContext && vulkanContext->getLogicalDevice()) {
            vulkanContext->getLogicalDevice()->waitIdle();
        }

        // Cleanup ImGui
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();

        // Destroy descriptor pool after ImGui shutdown
        if (imguiPool != VK_NULL_HANDLE && vulkanContext) {
            vkDestroyDescriptorPool(vulkanContext->getVkDevice(), imguiPool, nullptr);
            imguiPool = VK_NULL_HANDLE;
        }

        ImGui::DestroyContext();
        imguiInitialized = false;
    }
}

void UIManager::render() {
    if (!imguiInitialized) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    // Begin dockspace with menubar
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    // Set window properties for dockspace
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // Begin dockspace window
    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    const ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));
    }
    if (!layoutInitialized) {
        initializeDockspace();
        layoutInitialized = true;
    }

    renderMainMenuBar();
    renderPanels();
    renderSceneViewport();
    ConsoleWidget::get().render();

    // Debug windows
    if (showStyleEditor) {
        ImGui::Begin("Style Editor", &showStyleEditor);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }

    if (showMetricsWindow) {
        ImGui::ShowMetricsWindow(&showMetricsWindow);
    }

    if (showAboutWindow) {
        ImGui::Begin("About OHAO Engine", &showAboutWindow);
        ImGui::Text("OHAO Engine v0.1");
        ImGui::Text("A modern game engine built with Vulkan");
        ImGui::Separator();
        ImGui::Text("Created by Qervas@github");
        ImGui::End();
    }

    ImGui::End(); // DockSpace

    if(preferencesWindow){
        preferencesWindow->render(nullptr);
    }

    ImGui::Render();

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);
    }
}


void UIManager::renderMainMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            renderFileMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            renderEditMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            renderViewMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Build")) {
            renderBuildMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            renderDebugMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            renderHelpMenu();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void UIManager::renderEditMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
    if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
    ImGui::Separator();
    if (ImGui::MenuItem("Cut", "Ctrl+X")) {}
    if (ImGui::MenuItem("Copy", "Ctrl+C")) {}
    if (ImGui::MenuItem("Paste", "Ctrl+V")) {}
    ImGui::Separator();
    if (ImGui::MenuItem("Preferences", "Ctrl+,")) {
        preferencesWindow->open();
    }
}

void UIManager::renderViewMenu() {
    if (ImGui::MenuItem("Scene View", nullptr, true)) {}
    if (ImGui::MenuItem("Game View", nullptr, false)) {}
    if (ImGui::MenuItem("Asset Browser", nullptr, true)) {}
    if (ImGui::MenuItem("Console", nullptr, true)) {}
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout")) {
        resetLayout();
    }
}

void UIManager::renderBuildMenu() {
    if (ImGui::MenuItem("Build Project")) {}
    if (ImGui::MenuItem("Build and Run", "F5")) {}
    ImGui::Separator();
    if (ImGui::MenuItem("Build Settings")) {}
}

void UIManager::renderDebugMenu() {
    if (ImGui::MenuItem("Style Editor", nullptr, &showStyleEditor)) {}
    if (ImGui::MenuItem("Metrics/Debugger", nullptr, &showMetricsWindow)) {}
    ImGui::Separator();
    if (ImGui::BeginMenu("Rendering")) {
        ImGui::MenuItem("Wireframe Mode", nullptr, false);
        ImGui::MenuItem("Show Normals", nullptr, false);
        ImGui::MenuItem("Show Collision", nullptr, false);
        ImGui::EndMenu();
    }
}

void UIManager::renderHelpMenu() {
    if (ImGui::MenuItem("Documentation")) {}
    if (ImGui::MenuItem("Report Bug")) {}
    ImGui::Separator();
    if (ImGui::MenuItem("About", nullptr, &showAboutWindow)) {}
}

void UIManager::renderFileMenu() {
    if (ImGui::MenuItem("New Project", "Ctrl+N")) {
        handleNewProject();
    }
    if (ImGui::MenuItem("Open Project", "Ctrl+O")) {
        handleOpenProject();
    }
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
        handleSaveProject();
    }
    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
        handleSaveAsProject();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Import Model", "Ctrl+I")) {
        handleModelImport();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit", "Alt+F4")) {
        handleExit();
    }
}

void UIManager::handleModelImport() {
    enableCursor(true);

    try {
        std::string filename = FileDialog::openFile(
            "Select OBJ File",
            "",
            std::vector<const char*>{"*.obj"},
            "Object Files (*.obj)"
        );

        if (!filename.empty()) {
            if (vulkanContext->importModel(filename)) {
                OHAO_LOG("Successfully loaded model: " + filename);
                
                // Get the scene after model import
                auto scene = vulkanContext->getScene();
                
                // Make sure all UI components are updated with the new model
                if (outlinerPanel) {
                    outlinerPanel->setScene(scene);
                }
                
                // Update SelectionManager to ensure it has the latest scene data
                SelectionManager::get().setScene(scene);
                
                // Ensure buffers are updated
                vulkanContext->updateSceneBuffers();
            } else {
                OHAO_LOG_ERROR("Failed to load model: " + filename);
            }
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error during model import: " + std::string(e.what()));
    }

    enableCursor(false);
}

bool UIManager::wantsInputCapture() const {
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void UIManager::enableCursor(bool enable) {
    window->enableCursor(enable);
}

bool UIManager::isSceneViewportHovered() const {
    return isSceneWindowHovered;
}

ViewportSize UIManager::getSceneViewportSize() const {
    return {
        static_cast<uint32_t>(sceneViewportSize.x),
        static_cast<uint32_t>(sceneViewportSize.y)
    };
}

void UIManager::renderSceneViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Scene Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    sceneViewportSize = ImGui::GetContentRegionAvail();
    isSceneWindowHovered = ImGui::IsWindowHovered();

    ImVec2 pos = ImGui::GetCursorScreenPos();

    auto sceneTexture = vulkanContext->getSceneRenderer()->getViewportTexture();
    if (sceneTexture) {  // Now we can use the bool operator
        ImTextureID imguiTexID = imgui::convertVulkanTextureToImGui(sceneTexture);
        ImGui::GetWindowDrawList()->AddImage(
            imguiTexID,
            pos,
            ImVec2(pos.x + sceneViewportSize.x, pos.y + sceneViewportSize.y),
            ImVec2(0, 0),
            ImVec2(1, 1)
        );
    }

    // Viewport resolution text at the bottom
    ImGui::SetCursorPos(ImVec2(10, sceneViewportSize.y - 30));
    ImGui::Text("Viewport: %dx%d", (int)sceneViewportSize.x, (int)sceneViewportSize.y);

    ImGui::End();
    ImGui::PopStyleVar();
}

void UIManager::setupPanels() {
    outlinerPanel = std::make_unique<OutlinerPanel>();
    propertiesPanel = std::make_unique<PropertiesPanel>();
    sceneSettingsPanel = std::make_unique<SceneSettingsPanel>();
    
    // Initialize UI panels with the current scene if available
    if (vulkanContext && vulkanContext->getScene()) {
        auto scene = vulkanContext->getScene();
        
        // Set the scene reference in the SelectionManager first
        SelectionManager::get().setScene(scene);
        
        // Then initialize the panels
        outlinerPanel->setScene(scene);
        propertiesPanel->setScene(scene);
        sceneSettingsPanel->setScene(scene);
        
        OHAO_LOG_DEBUG("UI Panels initialized with scene");
    }
}

void UIManager::renderPanels() {
    if (outlinerPanel) outlinerPanel->render();
    if (propertiesPanel) propertiesPanel->render();
    if (sceneSettingsPanel) sceneSettingsPanel->render();
}

void UIManager::initializeDockspace() {
    if (isDockspaceInitialized) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    if (dockspace_id == 0) return;

    // Use Layout Manager to setup the default layout
    LayoutManager::initializeLayout(dockspace_id);

    isDockspaceInitialized = true;
}

void UIManager::resetLayout() {
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    LayoutManager::resetLayout(dockspace_id);
}

OutlinerPanel* UIManager::getOutlinerPanel() const { return outlinerPanel.get(); }
PropertiesPanel* UIManager::getPropertiesPanel() const { return propertiesPanel.get(); }
SceneSettingsPanel* UIManager::getSceneSettingsPanel() const { return sceneSettingsPanel.get(); }

bool UIManager::showNewProjectDialog() {
    static char nameBuffer[256] = "";
    bool confirmed = false;

    ImGui::OpenPopup("New Project");
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Project Name:");
        if (ImGui::InputText("##ProjectName", nameBuffer, sizeof(nameBuffer))) {
            newProjectName = nameBuffer;
        }

        if (ImGui::Button("Create", ImVec2(120, 0))) {
            if (strlen(nameBuffer) > 0) {
                newProjectName = nameBuffer;
                confirmed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return confirmed;
}

void UIManager::handleNewProject() {
    // Enable cursor for dialog interaction
    enableCursor(true);

    if (showNewProjectDialog()) {
        if (vulkanContext->createNewScene(newProjectName)) {
            currentProjectPath = "";
            OHAO_LOG("Created new project: " + newProjectName);

            // Get the reference to the newly created scene
            auto scene = vulkanContext->getScene();
            
            // Update SelectionManager first
            SelectionManager::get().setScene(scene);
            
            // Then update UI panels
            if (outlinerPanel) outlinerPanel->setScene(scene);
            if (propertiesPanel) propertiesPanel->setScene(scene);
            if (sceneSettingsPanel) sceneSettingsPanel->setScene(scene);
        } else {
            OHAO_LOG_ERROR("Failed to create new project");
        }
    }

    enableCursor(false);
}

void UIManager::handleOpenProject() {
    enableCursor(true);

    std::string filename = FileDialog::openFile(
        "Open Project",
        "",
        std::vector<const char*>{".ohao"},
        "OHAO Project Files (*.ohao)"
    );

    if (!filename.empty()) {
        if (vulkanContext->loadScene(filename)) {
            currentProjectPath = filename;
            OHAO_LOG("Successfully opened project: " + filename);

            // Get the loaded scene
            auto scene = vulkanContext->getScene();
            
            // Update SelectionManager first
            SelectionManager::get().setScene(scene);
            
            // Then update UI panels
            if (outlinerPanel) outlinerPanel->setScene(scene);
            if (propertiesPanel) propertiesPanel->setScene(scene);
            if (sceneSettingsPanel) sceneSettingsPanel->setScene(scene);
        } else {
            OHAO_LOG_ERROR("Failed to open project: " + filename);
        }
    }

    enableCursor(false);
}

bool UIManager::handleSaveProject() {
    if (currentProjectPath.empty()) {
         return handleSaveAsProject();
     }

     if (vulkanContext->saveScene(currentProjectPath)) {
         OHAO_LOG("Project saved successfully: " + currentProjectPath);
         return true;
     } else {
         OHAO_LOG_ERROR("Failed to save project: " + currentProjectPath);
         return false;
     }
}

bool UIManager::handleSaveAsProject() {
    enableCursor(true);

    std::string filename = FileDialog::saveFile(
        "Save Project As",
        "",
        std::vector<const char*>{".ohao"},
        "OHAO Project Files (*.ohao)"
    );

    if (!filename.empty()) {
        // Add extension if not present
        if (filename.substr(filename.find_last_of(".") + 1) != "ohao") {
            filename += ".ohao";
        }

        if (vulkanContext->saveScene(filename)) {
            currentProjectPath = filename;
            OHAO_LOG("Project saved successfully: " + filename);
            enableCursor(false);
            return true;
        } else {
            OHAO_LOG_ERROR("Failed to save project: " + filename);
        }
    }

    enableCursor(false);
    return false;
}

void UIManager::handleExit() {
    // Check for unsaved changes
    if (vulkanContext->hasUnsavedChanges()) {
        ImGui::OpenPopup("Unsaved Changes");
    } else {
        glfwSetWindowShouldClose(window->getGLFWWindow(), GLFW_TRUE);
        return;
    }

    // Show confirmation dialog for unsaved changes
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You have unsaved changes. Do you want to save before exiting?");

        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (handleSaveProject()) {
                glfwSetWindowShouldClose(window->getGLFWWindow(), GLFW_TRUE);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            glfwSetWindowShouldClose(window->getGLFWWindow(), GLFW_TRUE);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace ohao
