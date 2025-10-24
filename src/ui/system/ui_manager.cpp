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
#include "renderer/components/light_component.hpp"
#include "ui/selection/selection_manager.hpp"
#include "ui/icons/font_awesome_icons.hpp"
#include <glm/glm.hpp>

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
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    if (theme == "Dark") {
        // Modern Unreal Engine-inspired dark theme with better readability and contrast

        // Background colors - darker, more professional
        colors[ImGuiCol_WindowBg]               = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);  // Main window background
        colors[ImGuiCol_ChildBg]                = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);  // Child window background
        colors[ImGuiCol_PopupBg]                = ImVec4(0.11f, 0.11f, 0.12f, 0.98f);  // Popup background
        colors[ImGuiCol_MenuBarBg]              = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);  // Menu bar background

        // Borders and separators - subtle but visible
        colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.20f, 0.22f, 0.65f);
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.30f);
        colors[ImGuiCol_Separator]              = ImVec4(0.25f, 0.25f, 0.27f, 0.70f);
        colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.35f, 0.60f, 0.85f, 0.78f);
        colors[ImGuiCol_SeparatorActive]        = ImVec4(0.40f, 0.65f, 0.90f, 1.00f);

        // Title bars - slightly lighter than background with blue tint when active
        colors[ImGuiCol_TitleBg]                = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
        colors[ImGuiCol_TitleBgActive]          = ImVec4(0.10f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.07f, 0.07f, 0.08f, 0.75f);

        // Text - high contrast white for better readability
        colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.96f, 1.00f);
        colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.30f, 0.55f, 0.85f, 0.45f);

        // Frames (inputs, combos, etc.) - darker with blue accent when active
        colors[ImGuiCol_FrameBg]                = ImVec4(0.15f, 0.15f, 0.16f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.22f, 0.25f, 1.00f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);

        // Buttons - subtle blue tint with vibrant hover/active states
        colors[ImGuiCol_Button]                 = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.48f, 0.75f, 1.00f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(0.35f, 0.60f, 0.90f, 1.00f);

        // Headers (collapsing headers, tree nodes) - blue accent
        colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.35f, 0.55f, 0.80f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(0.28f, 0.48f, 0.75f, 0.90f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(0.35f, 0.60f, 0.90f, 1.00f);

        // Tabs - modern tab design with blue active state
        colors[ImGuiCol_Tab]                    = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
        colors[ImGuiCol_TabHovered]             = ImVec4(0.28f, 0.48f, 0.75f, 0.90f);
        colors[ImGuiCol_TabActive]              = ImVec4(0.20f, 0.35f, 0.55f, 1.00f);
        colors[ImGuiCol_TabUnfocused]           = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.15f, 0.20f, 0.30f, 1.00f);

        // Scrollbar - subtle and modern
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.40f, 0.40f, 0.42f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);

        // Checkboxes and sliders - blue accent
        colors[ImGuiCol_CheckMark]              = ImVec4(0.45f, 0.70f, 1.00f, 1.00f);
        colors[ImGuiCol_SliderGrab]             = ImVec4(0.40f, 0.65f, 0.95f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);

        // Resize grip
        colors[ImGuiCol_ResizeGrip]             = ImVec4(0.25f, 0.25f, 0.27f, 0.50f);
        colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.35f, 0.60f, 0.90f, 0.70f);
        colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.40f, 0.65f, 0.95f, 0.90f);

        // Docking
        colors[ImGuiCol_DockingPreview]         = ImVec4(0.35f, 0.60f, 0.90f, 0.40f);
        colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);

        // Plot colors (for graphs, histograms)
        colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

        // Table colors
        colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.25f, 0.25f, 0.27f, 1.00f);
        colors[ImGuiCol_TableBorderLight]       = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
        colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

        // Drag and drop
        colors[ImGuiCol_DragDropTarget]         = ImVec4(0.45f, 0.70f, 1.00f, 0.90f);

        // Navigation highlight
        colors[ImGuiCol_NavHighlight]           = ImVec4(0.45f, 0.70f, 1.00f, 1.00f);
        colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

        // Modal window dimming
        colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);

        OHAO_LOG_DEBUG("Applied enhanced Dark theme (Unreal Engine-inspired)");

    } else if (theme == "Light") {
        ImGui::StyleColorsLight();
        OHAO_LOG_DEBUG("Applied Light theme");
    } else if (theme == "Classic") {
        ImGui::StyleColorsClassic();
        OHAO_LOG_DEBUG("Applied Classic theme");
    }
}

void UIManager::setupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Spacing and padding - more generous for better readability
    style.WindowPadding            = ImVec2(10.00f, 10.00f);   // More padding for better breathing room
    style.FramePadding             = ImVec2(8.00f, 4.00f);     // Taller frames for better clickability
    style.CellPadding              = ImVec2(8.00f, 4.00f);     // Better table cell spacing
    style.ItemSpacing              = ImVec2(8.00f, 6.00f);     // More horizontal space between items
    style.ItemInnerSpacing         = ImVec2(6.00f, 4.00f);     // Space inside items
    style.TouchExtraPadding        = ImVec2(0.00f, 0.00f);
    style.IndentSpacing            = 22.0f;                    // Tree indentation
    style.ScrollbarSize            = 14.0f;                    // Slightly wider scrollbar
    style.GrabMinSize              = 12.0f;                    // Larger grab handles

    // Borders - clean and minimal
    style.WindowBorderSize         = 1.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupBorderSize          = 1.0f;
    style.FrameBorderSize          = 0.0f;                     // No frame borders for cleaner look
    style.TabBorderSize            = 0.0f;                     // No tab borders

    // Rounding - modern and smooth
    style.WindowRounding           = 6.0f;                     // Slightly rounded windows
    style.ChildRounding            = 4.0f;                     // Subtle child window rounding
    style.FrameRounding            = 4.0f;                     // Rounded inputs/frames
    style.PopupRounding            = 5.0f;                     // Rounded popups
    style.ScrollbarRounding        = 8.0f;                     // Smooth scrollbar
    style.GrabRounding             = 4.0f;                     // Rounded sliders
    style.LogSliderDeadzone        = 4.0f;
    style.TabRounding              = 4.0f;                     // Rounded tabs

    // Additional polish
    style.WindowTitleAlign         = ImVec2(0.00f, 0.50f);     // Left-aligned title
    style.WindowMenuButtonPosition = ImGuiDir_None;            // No collapse button
    style.ColorButtonPosition      = ImGuiDir_Right;           // Color picker on right
    style.ButtonTextAlign          = ImVec2(0.50f, 0.50f);     // Center button text
    style.SelectableTextAlign      = ImVec2(0.00f, 0.50f);     // Left-aligned selectable items
    style.DisplaySafeAreaPadding   = ImVec2(4.00f, 4.00f);     // Safe area padding
    style.AntiAliasedLines         = true;                     // Smooth lines
    style.AntiAliasedLinesUseTex   = true;                     // Better line quality
    style.AntiAliasedFill          = true;                     // Smooth shapes
    style.CurveTessellationTol     = 1.25f;                    // Smoother curves
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


    // Load fonts: Default font + FontAwesome icons
    ImGuiIO& io = ImGui::GetIO();

    // Load default font at 16px for better readability
    io.Fonts->AddFontDefault();

    // Merge FontAwesome icons into the default font
    static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.GlyphMinAdvanceX = 18.0f; // Make icons monospaced
    icons_config.GlyphOffset = ImVec2(0.0f, 2.0f); // Adjust vertical alignment

    const char* font_path = "assets/fonts/fa-solid-900.ttf";
    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, 16.0f, &icons_config, icons_ranges);

    if (!font) {
        OHAO_LOG_WARNING("Failed to load FontAwesome font from: " + std::string(font_path));
        OHAO_LOG_WARNING("Icon toolbar will use text placeholders. See assets/fonts/README.md");
    } else {
        OHAO_LOG_DEBUG("FontAwesome icons loaded successfully");
    }

    // Build the font atlas
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

    ImVec2 newSize = ImGui::GetContentRegionAvail();
    // Notify renderer if viewport size changed (avoid 0-dimensions)
    if ((newSize.x > 0 && newSize.y > 0) &&
        (newSize.x != sceneViewportSize.x || newSize.y != sceneViewportSize.y)) {
        sceneViewportSize = newSize;
        if (vulkanContext) {
            vulkanContext->setViewportSize(static_cast<uint32_t>(sceneViewportSize.x),
                                           static_cast<uint32_t>(sceneViewportSize.y));
        }
    } else {
        sceneViewportSize = newSize;
    }
    isSceneWindowHovered = ImGui::IsWindowHovered();

    ImVec2 pos = ImGui::GetCursorScreenPos();

    auto sceneTexture = vulkanContext->getSceneRenderer()->getViewportTexture();
    if (sceneTexture) {
        ImTextureID imguiTexID = imgui::convertVulkanTextureToImGui(sceneTexture);
        // Restore previous behavior: fill the entire panel; renderer resizes RT to panel
        ImGui::GetWindowDrawList()->AddImage(
            imguiTexID,
            pos,
            ImVec2(pos.x + sceneViewportSize.x, pos.y + sceneViewportSize.y),
            ImVec2(0, 0),
            ImVec2(1, 1)
        );
    }

    // Render light indicators for selected lights
    renderLightIndicators(pos, sceneViewportSize);

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
    renderSettingsPanel = std::make_unique<RenderSettingsPanel>();
    viewportToolbar = std::make_unique<ViewportToolbar>();
    physicsPanel = std::make_unique<PhysicsPanel>();

    // Create component-specific panels
    meshComponentPanel = std::make_unique<MeshComponentPanel>();
    materialComponentPanel = std::make_unique<MaterialComponentPanel>();
    physicsComponentPanel = std::make_unique<PhysicsComponentPanel>();
    lightComponentPanel = std::make_unique<LightComponentPanel>();

    // Create side panel manager for Blender-style tabbed interface
    sidePanelManager = std::make_unique<SidePanelManager>();

    // Register scene-level panels with side panel manager
    sidePanelManager->registerTab(SidePanelTab::Properties, ICON_FA_WRENCH,
                                  "Properties", propertiesPanel.get());
    sidePanelManager->registerTab(SidePanelTab::SceneSettings, ICON_FA_TREE,
                                  "Scene Settings", sceneSettingsPanel.get());
    sidePanelManager->registerTab(SidePanelTab::RenderSettings, ICON_FA_IMAGE,
                                  "Render Settings", renderSettingsPanel.get());
    sidePanelManager->registerTab(SidePanelTab::Physics, ICON_FA_ATOM,
                                  "Physics Simulation", physicsPanel.get());

    // Register component-specific panels (dynamic tabs)
    sidePanelManager->registerComponentTab(ComponentTab::Mesh, ICON_FA_CUBE,
                                          "Mesh Component", meshComponentPanel.get());
    sidePanelManager->registerComponentTab(ComponentTab::Material, ICON_FA_PALETTE,
                                          "Material Component", materialComponentPanel.get());
    sidePanelManager->registerComponentTab(ComponentTab::Physics, ICON_FA_COGS,
                                          "Physics Component", physicsComponentPanel.get());
    sidePanelManager->registerComponentTab(ComponentTab::Light, ICON_FA_LIGHTBULB,
                                          "Light Component", lightComponentPanel.get());

    // Set Properties as default active tab
    sidePanelManager->setActiveTab(SidePanelTab::Properties);

    // Set up selection change callback to update dynamic tabs
    SelectionManager::get().setSelectionChangedCallback([this](void* userData) {
        // Get the selected actor from SelectionManager
        Actor* selectedActor = SelectionManager::get().getSelectedActor();

        if (sidePanelManager) {
            sidePanelManager->updateDynamicTabs(selectedActor);
        }

        // Update component panels with selected actor
        if (meshComponentPanel) meshComponentPanel->setSelectedActor(selectedActor);
        if (materialComponentPanel) materialComponentPanel->setSelectedActor(selectedActor);
        if (physicsComponentPanel) {
            physicsComponentPanel->setSelectedActor(selectedActor);
            if (vulkanContext && vulkanContext->getScene()) {
                physicsComponentPanel->setScene(vulkanContext->getScene());
            }
        }
        if (lightComponentPanel) lightComponentPanel->setSelectedActor(selectedActor);
    });

    // Connect viewport toolbar to axis gizmo system
    if (vulkanContext && vulkanContext->getAxisGizmo()) {
        viewportToolbar->setAxisGizmo(vulkanContext->getAxisGizmo());
    }

    // Initialize UI panels with the current scene if available
    if (vulkanContext && vulkanContext->getScene()) {
        auto scene = vulkanContext->getScene();

        // Set the scene reference in the SelectionManager first
        SelectionManager::get().setScene(scene);

        // Then initialize the panels
        outlinerPanel->setScene(scene);
        propertiesPanel->setScene(scene);
        sceneSettingsPanel->setScene(scene);

        // Connect physics panel to physics world and scene
        physicsPanel->setPhysicsWorld(scene->getPhysicsWorld());
        physicsPanel->setScene(scene);

        OHAO_LOG_DEBUG("UI Panels initialized with scene and side panel manager");
    }
}

void UIManager::renderPanels() {
    // Render outliner separately (stays at top of right panel)
    if (outlinerPanel) outlinerPanel->render();

    // Render side panel manager (handles tabbed interface for other panels)
    if (sidePanelManager) {
        if (ImGui::Begin("Side Panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) {
            sidePanelManager->render();
        }
        ImGui::End();
    }

    // Render viewport toolbar (overlays on viewport)
    if (viewportToolbar) viewportToolbar->render();
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
ViewportToolbar* UIManager::getViewportToolbar() const { return viewportToolbar.get(); }

PhysicsPanel* UIManager::getPhysicsPanel() const { return physicsPanel.get(); }

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
        std::vector<const char*>{"*.ohao", "*.OHAO"},
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
        std::vector<const char*>{"*.ohao", "*.OHAO"},
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

void UIManager::renderLightIndicators(ImVec2 viewportPos, ImVec2 viewportSize) {
    // Get the current scene from the vulkan context
    auto scene = vulkanContext->getScene();
    if (!scene) return;
    
    // Get the current camera for world-to-screen projection
    auto camera = vulkanContext->getCamera();
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Get all actors with light components
    for (const auto& [actorId, actor] : scene->getAllActors()) {
        auto lightComponent = actor->getComponent<LightComponent>();
        if (!lightComponent) continue;
        
        // Check if this light actor is selected
        bool isSelected = SelectionManager::get().isSelected(actor.get());
        if (!isSelected) continue;  // Only show indicators for selected lights
        
        // Get light world position
        glm::vec3 worldPos = actor->getTransform()->getPosition();
        
        // Project world position to screen space
        glm::mat4 viewMatrix = camera.getViewMatrix();
        glm::mat4 projMatrix = camera.getProjectionMatrix();
        projMatrix[1][1] *= -1;  // Flip Y for Vulkan
        
        glm::mat4 viewProj = projMatrix * viewMatrix;
        glm::vec4 clipPos = viewProj * glm::vec4(worldPos, 1.0f);
        
        // Perspective divide
        if (clipPos.w <= 0.001f) continue;  // Behind camera
        
        glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        
        // Check if the light is in front of the camera and within NDC bounds
        if (ndc.z < 0.0f || ndc.z > 1.0f) continue;
        if (ndc.x < -1.0f || ndc.x > 1.0f) continue;
        if (ndc.y < -1.0f || ndc.y > 1.0f) continue;
        
        // Convert NDC to screen coordinates
        float screenX = (ndc.x * 0.5f + 0.5f) * viewportSize.x;
        float screenY = (ndc.y * 0.5f + 0.5f) * viewportSize.y;
        
        ImVec2 screenPos(viewportPos.x + screenX, viewportPos.y + screenY);
        
        // Draw light bulb icon (different for each type)
        float iconSize = 16.0f;
        ImU32 lightColor, outlineColor;
        
        // Different colors for different light types
        switch (lightComponent->getLightType()) {
            case LightType::Point:
                lightColor = IM_COL32(255, 255, 100, 255);  // Yellow for point light
                break;
            case LightType::Directional:
                lightColor = IM_COL32(255, 200, 100, 255);  // Orange for directional light
                break;
            case LightType::Spot:
                lightColor = IM_COL32(100, 255, 100, 255);  // Green for spot light
                break;
            default:
                lightColor = IM_COL32(255, 255, 255, 255);  // White fallback
                break;
        }
        outlineColor = IM_COL32(0, 0, 0, 255);      // Black outline
        
        // Draw bulb shape (circle for all types)
        drawList->AddCircleFilled(screenPos, iconSize * 0.4f, lightColor, 8);
        drawList->AddCircle(screenPos, iconSize * 0.4f, outlineColor, 8, 2.0f);
        
        // Draw different patterns for different light types
        if (lightComponent->getLightType() == LightType::Point) {
            // Draw light rays for point light
            for (int i = 0; i < 8; i++) {
                float angle = (i * 45.0f) * (3.14159f / 180.0f);
                float rayLength = iconSize * 0.3f;
                ImVec2 rayStart(
                    screenPos.x + cos(angle) * (iconSize * 0.5f),
                    screenPos.y + sin(angle) * (iconSize * 0.5f)
                );
                ImVec2 rayEnd(
                    screenPos.x + cos(angle) * (iconSize * 0.5f + rayLength),
                    screenPos.y + sin(angle) * (iconSize * 0.5f + rayLength)
                );
                drawList->AddLine(rayStart, rayEnd, lightColor, 2.0f);
            }
        } else if (lightComponent->getLightType() == LightType::Directional) {
            // Draw parallel arrows for directional light
            for (int i = 0; i < 3; i++) {
                float offset = (i - 1) * 6.0f;
                ImVec2 arrowStart(screenPos.x + offset, screenPos.y - iconSize * 0.6f);
                ImVec2 arrowEnd(screenPos.x + offset, screenPos.y + iconSize * 0.6f);
                drawList->AddLine(arrowStart, arrowEnd, lightColor, 2.0f);
                
                // Arrow head
                ImVec2 arrowTip1(screenPos.x + offset - 3.0f, screenPos.y + iconSize * 0.4f);
                ImVec2 arrowTip2(screenPos.x + offset + 3.0f, screenPos.y + iconSize * 0.4f);
                drawList->AddLine(arrowEnd, arrowTip1, lightColor, 2.0f);
                drawList->AddLine(arrowEnd, arrowTip2, lightColor, 2.0f);
            }
        } else if (lightComponent->getLightType() == LightType::Spot) {
            // Draw cone shape for spot light
            float coneHeight = iconSize * 0.8f;
            float coneWidth = iconSize * 0.6f;
            
            ImVec2 coneTop(screenPos.x, screenPos.y - coneHeight * 0.3f);
            ImVec2 coneLeft(screenPos.x - coneWidth * 0.5f, screenPos.y + coneHeight * 0.5f);
            ImVec2 coneRight(screenPos.x + coneWidth * 0.5f, screenPos.y + coneHeight * 0.5f);
            
            drawList->AddLine(coneTop, coneLeft, lightColor, 2.0f);
            drawList->AddLine(coneTop, coneRight, lightColor, 2.0f);
            drawList->AddLine(coneLeft, coneRight, lightColor, 2.0f);
        }
        
        // Draw light type text below the icon
        const char* lightTypeText = "";
        switch (lightComponent->getLightType()) {
            case LightType::Point: lightTypeText = "Point"; break;
            case LightType::Directional: lightTypeText = "Directional"; break;
            case LightType::Spot: lightTypeText = "Spot"; break;
        }
        
        ImVec2 textPos(screenPos.x - ImGui::CalcTextSize(lightTypeText).x * 0.5f, screenPos.y + iconSize * 0.6f);
        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), lightTypeText);
    }
}

} // namespace ohao
