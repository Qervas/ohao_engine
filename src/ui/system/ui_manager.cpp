#include "ui/system/ui_manager.hpp"
#include "console_widget.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "ohao_vk_texture_handle.hpp"
#include "window/window.hpp"
#include "components/file_dialog.hpp"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <vulkan/vulkan_core.h>
#include "imgui.h"
#include "imgui_vulkan_utils.hpp"

namespace ohao {

UIManager::UIManager(Window* window, VulkanContext* context)
    : window(window), vulkanContext(context) {
}

UIManager::~UIManager(){
    if (vulkanContext && vulkanContext->getLogicalDevice()) {
        vulkanContext->getLogicalDevice()->waitIdle();
    }
    shutdownImGui();
}

void UIManager::initialize() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Enable docking
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable Multi-Viewport

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        io.ConfigViewportsNoAutoMerge = true;
        io.ConfigViewportsNoTaskBarIcon = true;
    }

    setupImGuiStyle();

    // Initialize ImGui implementation
    ImGui_ImplGlfw_InitForVulkan(window->getGLFWWindow(), true);
    initializeVulkanBackend();
}

void UIManager::setupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Color scheme
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_Border]                 = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.11f, 0.64f, 0.92f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.11f, 0.64f, 0.92f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.08f, 0.50f, 0.72f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    colors[ImGuiCol_Header]                 = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    colors[ImGuiCol_Separator]              = style.Colors[ImGuiCol_Border];
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.41f, 0.42f, 0.44f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    colors[ImGuiCol_Tab]                    = ImLerp(colors[ImGuiCol_Header], colors[ImGuiCol_TitleBgActive], 0.80f);
    colors[ImGuiCol_TabHovered]             = colors[ImGuiCol_HeaderHovered];
    colors[ImGuiCol_TabActive]              = ImLerp(colors[ImGuiCol_HeaderActive], colors[ImGuiCol_TitleBgActive], 0.60f);
    colors[ImGuiCol_TabUnfocused]           = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImLerp(colors[ImGuiCol_TabActive], colors[ImGuiCol_TitleBg], 0.40f);
    ImVec4 headerCol = colors[ImGuiCol_Header];
    colors[ImGuiCol_DockingPreview]         = ImVec4(headerCol.x, headerCol.y, headerCol.z, headerCol.w * 0.7f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

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
    ImGui_ImplVulkan_LoadFunctions([](const char* function_name, void* vulkan_instance) {
        return vkGetInstanceProcAddr(static_cast<VkInstance>(vulkan_instance), function_name);
    }, vulkanContext->getVkInstance());

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


    // Upload fonts
    VkCommandBuffer commandBuffer = vulkanContext->getCommandManager()->beginSingleTime();
    ImGui_ImplVulkan_CreateFontsTexture();
    vulkanContext->getCommandManager()->endSingleTime(commandBuffer);
    ImGui_ImplVulkan_DestroyFontsTexture();

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

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    // Draw the main menu bar
    renderMainMenuBar();

    // DockSpace
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));

    // Setup default layout after first frame
    if (!layoutInitialized) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

        auto dock_id_main = dockspace_id;
        auto dock_id_right = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Right, 0.2f, nullptr, &dock_id_main);
        auto dock_id_bottom = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Down, 0.25f, nullptr, &dock_id_main);

        ImGui::DockBuilderDockWindow("Scene Viewport", dock_id_main);
        ImGui::DockBuilderDockWindow("Properties", dock_id_right);
        ImGui::DockBuilderDockWindow("Console", dock_id_bottom);

        ImGui::DockBuilderFinish(dockspace_id);
        layoutInitialized = true;
    }

    renderSceneViewport();

    // Properties Panel
    ImGui::Begin("Properties");
    if (ImGui::CollapsingHeader("Scene Info")) {
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

        // Get actual vertex count from scene
        size_t vertexCount = 0;
        if (vulkanContext->hasLoadScene()) {
            auto scene = vulkanContext->getScene();
            auto mainObject = scene->getObjects().begin()->second;
            vertexCount = mainObject->model->vertices.size();
        }

        ImGui::Text("Vertices: %zu", vertexCount);
    }
    ImGui::End();

    // Console
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
        ImGui::Text("Created by [Your Name]");
        ImGui::End();
    }

    ImGui::End(); // DockSpace

    // End the frame
    ImGui::EndFrame();

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
    if (ImGui::MenuItem("Project Settings")) {}
}

void UIManager::renderViewMenu() {
    if (ImGui::MenuItem("Scene View", nullptr, true)) {}
    if (ImGui::MenuItem("Game View", nullptr, false)) {}
    if (ImGui::MenuItem("Asset Browser", nullptr, true)) {}
    if (ImGui::MenuItem("Console", nullptr, true)) {}
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout")) {}
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
    if (ImGui::MenuItem("New Project", "Ctrl+N")) {}
    if (ImGui::MenuItem("Open Project", "Ctrl+O")) {}
    if (ImGui::MenuItem("Save", "Ctrl+S")) {}
    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {}
    ImGui::Separator();
    if (ImGui::MenuItem("Import Model", "Ctrl+I")) {
        handleModelImport();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit", "Alt+F4")) {
        // TODO: Handle exit
    }
}

void UIManager::handleModelImport() {
    enableCursor(true);

    std::string filename = FileDialog::openFile(
        "Select OBJ File",
        "",
        std::vector<const char*>{"*.obj"},
        "Object Files (*.obj)"
    );

    if (!filename.empty()) {
        if (vulkanContext->loadModel(filename)) {
            OHAO_LOG("Successfully loaded model: " + filename);
            window->enableCursor(true);  // Keep cursor enabled after loading
        } else {
            OHAO_LOG_ERROR("Failed to load model: " + filename);
        }
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

void UIManager::setupDefaultLayout(){
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

    // Setup default docking layout
    ImGui::DockBuilderRemoveNode(dockspace_id); // Clear any previous layout
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    // Split the docking space into sections
    auto dock_id_main = dockspace_id; // Main dock space ID
    auto dock_id_right = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Right, 0.2f, nullptr, &dock_id_main);
    auto dock_id_bottom = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Down, 0.25f, nullptr, &dock_id_main);

    // Dock windows
    ImGui::DockBuilderDockWindow("Scene Viewport", dock_id_main);
    ImGui::DockBuilderDockWindow("Properties", dock_id_right);
    ImGui::DockBuilderDockWindow("Console", dock_id_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
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

} // namespace ohao
