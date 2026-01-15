#include "vulkan_context.hpp"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "ui/panels/viewport/viewport_toolbar.hpp"
#include "engine/scene/default_scene_factory.hpp"
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include "renderer/material/material.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/components/light_component.hpp"
#include <glm/trigonometric.hpp>
#include <memory>
#include <stdexcept>
#include <sys/types.h>
#include <vector>
#include <rhi/vk/ohao_vk_command_manager.hpp>
#include <rhi/vk/ohao_vk_descriptor.hpp>
#include <rhi/vk/ohao_vk_device.hpp>
#include <rhi/vk/ohao_vk_framebuffer.hpp>
#include <rhi/vk/ohao_vk_image.hpp>
#include <rhi/vk/ohao_vk_physical_device.hpp>
#include <rhi/vk/ohao_vk_pipeline.hpp>
#include <rhi/vk/ohao_vk_render_pass.hpp>
#include <rhi/vk/ohao_vk_surface.hpp>
#include <rhi/vk/ohao_vk_swapchain.hpp>
#include <rhi/vk/ohao_vk_instance.hpp>
#include <rhi/vk/ohao_vk_shader_module.hpp>
#include <rhi/vk/ohao_vk_sync_objects.hpp>
#include <rhi/vk/ohao_vk_uniform_buffer.hpp>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>
#include <iostream>
#include "subsystems/scene/scene_renderer.hpp"
#include "subsystems/scene/scene_render_target.hpp"
#include "subsystems/shadow/shadow_renderer.hpp"
#include "lighting/lighting_system.hpp"
#include "lighting/shadow_map_pool.hpp"
#include "ui/components/console_widget.hpp"
#include "ui/system/ui_manager.hpp"
#include "ui/window/window.hpp"
#include <utils/common_types.hpp>
#include <filesystem>
#include <limits>


#define OHAO_ENABLE_VALIDATION_LAYER true

namespace ohao{
VulkanContext* VulkanContext::contextInstance = nullptr;

VulkanContext::VulkanContext(Window* windowHandle): window(windowHandle){
    contextInstance = this;
    int w, h;
    glfwGetFramebufferSize(window->getGLFWWindow(), &w, &h);
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    
    // Position camera to see objects at origin from a reasonable distance
    camera.setPosition(glm::vec3(0.0f, 2.0f, 5.0f));
    camera.setRotation(-15.0f, -90.0f);  // Look down slightly at origin
    camera.setPerspectiveProjection(60.0f, float(width)/float(height), 0.1f, 1000.0f); // Wider FOV
}

VulkanContext::~VulkanContext(){
    if(contextInstance == this){
        contextInstance = nullptr;
    }
    cleanup();
}

void
VulkanContext::initializeVulkan(){
    //vulkan setup
    instance = std::make_unique<OhaoVkInstance>();
    if (!instance->initialize("OHAO Engine", OHAO_ENABLE_VALIDATION_LAYER)) {
        throw std::runtime_error("engine instance initialization failed!");
    }
    surface = std::make_unique<OhaoVkSurface>();
    if (!surface->initialize(instance.get(), window->getGLFWWindow())){
        throw std::runtime_error("engine surface initialization failed!");
    }

    physicalDevice = std::make_unique<OhaoVkPhysicalDevice>();
    if (!physicalDevice->initialize(instance.get(), surface.get(),
        OhaoVkPhysicalDevice::PreferredVendor::NVIDIA)) {
        throw std::runtime_error("engine physical device initialization failed!");
    }

    device = std::make_unique<OhaoVkDevice>();
    if(!device->initialize(physicalDevice.get(), instance->getValidationLayers())){
        throw std::runtime_error("engine logical device initialization failed!");
    }
    graphicsQueue = device->getGraphicsQueue();
    presentQueue = device->getPresentQueue();

    swapchain = std::make_unique<OhaoVkSwapChain>();
    if(!swapchain->initialize(device.get(), surface.get(), width, height)){
        throw std::runtime_error("engine swapchain initialization failed!");
    }

    shaderModules = std::make_unique<OhaoVkShaderModule>();
    if(!shaderModules->initialize(device.get())) {
        throw std::runtime_error("Failed to initialize shader modules!");
    }

    renderPass = std::make_unique<OhaoVkRenderPass>();
    if(!renderPass->initialize(device.get(), swapchain.get())){
        throw std::runtime_error("engine render pass initialization failed!");
    }

    if (!shaderModules->createShaderModule(
            "vert", "shaders/shader.vert.spv",
            OhaoVkShaderModule::ShaderType::VERTEX) ||
        !shaderModules->createShaderModule(
            "frag", "shaders/shader.frag.spv",
            OhaoVkShaderModule::ShaderType::FRAGMENT) ||
        !shaderModules->createShaderModule(
            "gizmo_vert", "shaders/gizmo.vert.spv",
            OhaoVkShaderModule::ShaderType::VERTEX) ||
        !shaderModules->createShaderModule(
            "gizmo_frag", "shaders/gizmo.frag.spv",
            OhaoVkShaderModule::ShaderType::FRAGMENT) ||
        !shaderModules->createShaderModule(
            "selection_vert", "shaders/selection.vert.spv",
            OhaoVkShaderModule::ShaderType::VERTEX) ||
        !shaderModules->createShaderModule(
            "selection_frag", "shaders/selection.frag.spv",
            OhaoVkShaderModule::ShaderType::FRAGMENT)
    ) {
        throw std::runtime_error("Failed to create shader modules!");
    }


    commandManager = std::make_unique<OhaoVkCommandManager>();
    if(!commandManager->initialize(device.get(), physicalDevice->getQueueFamilyIndices().graphicsFamily.value())){
        throw std::runtime_error("engine command manager initialization failed!");
    }

    if(!commandManager->allocateCommandBuffers(MAX_FRAMES_IN_FLIGHT)){
        throw std::runtime_error("failed to allocate command buffers!");
    }

    depthImage = std::make_unique<OhaoVkImage>();
    if (!depthImage->initialize(device.get())) {
        throw std::runtime_error("engine depth image initialization failed!");
    }
    if (!depthImage->createDepthResources(swapchain->getExtent(), msaaSamples)) {
        throw std::runtime_error("Failed to create depth resources!");
    }

    framebufferManager = std::make_unique<OhaoVkFramebuffer>();
    if(!framebufferManager->initialize(device.get(), swapchain.get(), renderPass.get(), depthImage.get())){
        throw std::runtime_error("engine framebuffer manager initialization failed!");
    }

    uniformBuffer = std::make_unique<OhaoVkUniformBuffer>();
    if(!uniformBuffer->initialize(device.get(), MAX_FRAMES_IN_FLIGHT, sizeof(UniformBufferObject))){
        throw std::runtime_error("engine uniform buffer initialization failed!");
    }

    descriptor = std::make_unique<OhaoVkDescriptor>();
    if(!descriptor->initialize(device.get(), MAX_FRAMES_IN_FLIGHT)) {
        throw std::runtime_error("engine descriptor system initialization failed!");
    }

    // Create descriptor sets for uniform buffers
    if (!descriptor->createDescriptorSets(uniformBuffer->getBuffers(), sizeof(UniformBufferObject))) {
        throw std::runtime_error("failed to create descriptor sets!");
    }

    axisGizmo = std::make_unique<AxisGizmo>();
    if (!axisGizmo->initialize(this)) {
        throw std::runtime_error("Failed to initialize axis gizmo!");
    }

    // Initialize pipelines
    modelPipeline = std::make_unique<OhaoVkPipeline>();
    if (!modelPipeline->initialize(device.get(), renderPass.get(), shaderModules.get(),
                                 swapchain->getExtent(), descriptor->getLayout(),
                                 OhaoVkPipeline::RenderMode::SOLID)) {
        throw std::runtime_error("Failed to create model pipeline!");
    }

    wireframePipeline = std::make_unique<OhaoVkPipeline>();
    if (!wireframePipeline->initialize(device.get(), renderPass.get(), shaderModules.get(),
                                     swapchain->getExtent(), descriptor->getLayout(),
                                     OhaoVkPipeline::RenderMode::WIREFRAME)) {
        throw std::runtime_error("Failed to create wireframe pipeline!");
    }

    // Initialize the push constant pipeline for model rendering
    modelPushConstantPipeline = std::make_unique<OhaoVkPipeline>();
    if (!modelPushConstantPipeline->initialize(device.get(), renderPass.get(), shaderModules.get(),
                                 swapchain->getExtent(), descriptor->getLayout(),
                                 OhaoVkPipeline::RenderMode::PUSH_CONSTANT_MODEL)) {
        throw std::runtime_error("Failed to create model push constant pipeline!");
    }

    gizmoPipeline = std::make_unique<OhaoVkPipeline>();
    if (!gizmoPipeline->initialize(device.get(), renderPass.get(), shaderModules.get(),
                                 swapchain->getExtent(), descriptor->getLayout(),
                                 OhaoVkPipeline::RenderMode::GIZMO)) {
        throw std::runtime_error("Failed to create gizmo pipeline!");
    }


    syncObjects = std::make_unique<OhaoVkSyncObjects>();
    if(!syncObjects->initialize(device.get(), MAX_FRAMES_IN_FLIGHT)){
        throw std::runtime_error("engine sync objects initialization failed!");
    }
    
    // Initialize swapchain-specific semaphores
    if(!syncObjects->initializeSwapchainSemaphores(swapchain->getImages().size())){
        throw std::runtime_error("engine swapchain sync objects initialization failed!");
    }

    sceneRenderer = std::make_unique<SceneRenderer>();
    if(!sceneRenderer->initialize(this)){
        throw std::runtime_error("engine scene renderer initializatin failed");
    }

    // Initialize unified lighting system
    lightingSystem = std::make_unique<LightingSystem>();
    if (!lightingSystem->initialize(device.get(), MAX_FRAMES_IN_FLIGHT)) {
        std::cerr << "Warning: Failed to initialize lighting system" << std::endl;
        lightingSystem.reset();
    } else {
        std::cout << "LightingSystem initialized successfully" << std::endl;
    }

    // Initialize shadow map pool (unified shadow system)
    shadowMapPool = std::make_unique<ShadowMapPool>();
    if (!shadowMapPool->initialize(this)) {
        std::cerr << "Warning: Failed to initialize shadow map pool" << std::endl;
        shadowMapPool.reset();
    } else {
        std::cout << "ShadowMapPool initialized successfully" << std::endl;

        // Update shadow map array descriptor for all frames
        if (descriptor) {
            auto shadowMapViews = shadowMapPool->getAllImageViews();
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
                descriptor->updateShadowMapArrayDescriptor(i,
                    shadowMapViews,
                    shadowMapPool->getSampler());
            }
            std::cout << "Shadow map array descriptors initialized for all frames" << std::endl;
        }
    }

    // Initialize legacy shadow renderer (for backward compatibility during migration)
    shadowRenderer = std::make_unique<ShadowRenderer>();
    if(!shadowRenderer->initialize(this)){
        std::cerr << "Warning: Failed to initialize shadow renderer, shadows will be disabled" << std::endl;
        shadowRenderer.reset();
    }

    initializeDefaultScene();
}

void VulkanContext::initializeDefaultScene() {
    // Use DefaultSceneFactory for clean, modular scene creation
    scene = DefaultSceneFactory::createBlenderLikeScene();
    
    if (!scene) {
        OHAO_LOG_ERROR("Failed to create default scene");
        throw std::runtime_error("Failed to initialize default scene");
    }
    OHAO_LOG("Default scene created with ComponentFactory");

    // Create minimal default buffers for legacy support
    std::vector<Vertex> defaultVertex = {
        {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}}
    };
    std::vector<uint32_t> defaultIndex = {0, 1, 2, 2, 3, 0};

    try {
        // Clean up any existing buffers
        device->waitIdle();
        vertexBuffer.reset();
        indexBuffer.reset();
        
        // Create new buffers
        createVertexBuffer(defaultVertex);
        createIndexBuffer(defaultIndex);
        
        // Verify buffers were created successfully
        if (!vertexBuffer || vertexBuffer->getBuffer() == VK_NULL_HANDLE) {
            throw std::runtime_error("Failed to create valid vertex buffer");
        }
        
        if (!indexBuffer || indexBuffer->getBuffer() == VK_NULL_HANDLE) {
            throw std::runtime_error("Failed to create valid index buffer");
        }
        
        OHAO_LOG("Default buffers created successfully");
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to create default buffers: " + std::string(e.what()));
        // Try one more time with even simpler data
        try {
            std::vector<Vertex> singleVertex = {
                {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}}
            };
            std::vector<uint32_t> singleIndex = {0};
            
            vertexBuffer.reset();
            indexBuffer.reset();
            
            createVertexBuffer(singleVertex);
            createIndexBuffer(singleIndex);
            
            OHAO_LOG("Created fallback minimal buffers");
        } catch (const std::exception& e2) {
            OHAO_LOG_ERROR("Critical failure creating minimal buffers: " + std::string(e2.what()));
        }
    }

    // Connect UI panels to the scene
    if (uiManager) {
        if (auto outlinerPanel = uiManager->getOutlinerPanel()) {
            outlinerPanel->setScene(scene.get());
        }
        if (auto propertiesPanel = uiManager->getPropertiesPanel()) {
            propertiesPanel->setScene(scene.get());
        }
        if (auto sceneSettingsPanel = uiManager->getSceneSettingsPanel()) {
            sceneSettingsPanel->setScene(scene.get());
        }
    }
    
    // Update scene buffers to include the scene geometry
    updateSceneBuffers();
    
    OHAO_LOG("Default scene initialization complete with ComponentFactory");
}

void VulkanContext::cleanup(){
    if(device){device->waitIdle();}
    if(uiManager){uiManager.reset();}
    sceneRenderer.reset();
    descriptor.reset();
    depthImage.reset();
    cleanupCurrentModel();
    uniformBuffer.reset();
    syncObjects.reset();
    commandManager.reset();
    framebufferManager.reset();
    axisGizmo.reset();
    wireframePipeline.reset();
    gizmoPipeline.reset();
    modelPushConstantPipeline.reset();
    sceneGizmoPipeline.reset();
    scenePipeline.reset();
    modelPipeline.reset();
    pipeline.reset();
    renderPass.reset();
    shaderModules.reset();
    swapchain.reset();
    device.reset();
    physicalDevice.reset();
    surface.reset();
    instance.reset();
}

void VulkanContext::cleanupSceneBuffers() {
    device->waitIdle();
    vertexBuffer.reset();
    indexBuffer.reset();
    meshBufferMap.clear();
}


void VulkanContext::initializeSceneRenderer() {
    if (!uiManager) {
        throw std::runtime_error("UIManager must be set before initializing scene renderer");
    }

    auto viewportSize = uiManager->getSceneViewportSize();
    uint32_t targetWidth = static_cast<uint32_t>(viewportSize.width);
    uint32_t targetHeight = static_cast<uint32_t>(viewportSize.height);
    // Fallback to swapchain extent if UI hasn't reported a size yet
    if (targetWidth == 0 || targetHeight == 0) {
        VkExtent2D extent = swapchain->getExtent();
        targetWidth = extent.width;
        targetHeight = extent.height;
    }

    if (!sceneRenderer->initializeRenderTarget(targetWidth, targetHeight)) {
        throw std::runtime_error("Failed to initialize scene render target");
    }

    VkDescriptorSetLayout descriptorSetLayout = descriptor->getLayout();

    // Create scene-specific pipelines
    scenePipeline = std::make_unique<OhaoVkPipeline>();
    if (!scenePipeline->initialize(
            device.get(),
            sceneRenderer->getRenderTarget()->getRenderPass(),
            shaderModules.get(),
            VkExtent2D{targetWidth, targetHeight},
            descriptorSetLayout,
            OhaoVkPipeline::RenderMode::SOLID)) {
        throw std::runtime_error("Failed to initialize scene pipeline!");
    }

    sceneGizmoPipeline = std::make_unique<OhaoVkPipeline>();
    if (!sceneGizmoPipeline->initialize(
            device.get(),
            sceneRenderer->getRenderTarget()->getRenderPass(),
            shaderModules.get(),
            VkExtent2D{targetWidth, targetHeight},
            descriptorSetLayout,
            OhaoVkPipeline::RenderMode::GIZMO)) {
        throw std::runtime_error("Failed to initialize scene gizmo pipeline!");
    }

    // Update the scene renderer to use both pipelines
    sceneRenderer->setPipelinesWithWireframe(scenePipeline.get(), wireframePipeline.get(), sceneGizmoPipeline.get());

    // Ensure camera aspect matches initial viewport
    if (targetWidth > 0 && targetHeight > 0) {
        camera.setAspectRatio(static_cast<float>(targetWidth) / static_cast<float>(targetHeight));
    }

    // Initialize picking system for viewport selection
    pickingSystem = std::make_unique<PickingSystem>();
    std::cout << "[VulkanContext] Picking system initialized" << std::endl;
}

void VulkanContext::updateScene(float deltaTime) {
    if (scene && uiManager) {
        auto* physicsPanel = uiManager->getPhysicsPanel();
        if (physicsPanel) {
            // Update physics world simulation state from the physics panel
            auto* physicsWorld = scene->getPhysicsWorld();
            if (physicsWorld) {
                auto currentState = physicsWorld->getSimulationState();
                auto newState = physicsPanel->getPhysicsState();
                
                // Only log state changes, not every frame
                if (currentState != newState) {
                    printf("PHYSICS PANEL SYNC: Physics world state %d -> %d\n", static_cast<int>(currentState), static_cast<int>(newState));
                }
                physicsWorld->setSimulationState(newState);
            }
            
            // Check if we should run physics
            if (physicsPanel->getPhysicsState() == physics::SimulationState::RUNNING && physicsPanel->isPhysicsEnabled()) {
                // Apply simulation speed multiplier
                float scaledDeltaTime = deltaTime * physicsPanel->getSimulationSpeed();

                // Update physics simulation
                scene->updatePhysics(scaledDeltaTime);

                // Increment frame counter for tracking
                physicsPanel->incrementFrame();
            }
        }
        
        // Always update scene components for proper sync
        scene->update(deltaTime);
    }
}

void VulkanContext::drawFrame() {
    if (!uiManager) {
        throw std::runtime_error("UI Manager not set!");
    }
    // Handle scene viewport resize requested by UI
    if (needsResize && lastWidth > 0 && lastHeight > 0) {
        device->waitIdle();

        // Resize scene render target
        if (sceneRenderer) {
            sceneRenderer->resize(lastWidth, lastHeight);
        }

        // Recreate scene pipelines with new extent and render pass
        VkDescriptorSetLayout descriptorSetLayout = descriptor->getLayout();
        OhaoVkRenderPass* rp = sceneRenderer && sceneRenderer->getRenderTarget() ? sceneRenderer->getRenderTarget()->getRenderPass() : nullptr;
        VkExtent2D newExtent{ lastWidth, lastHeight };

        scenePipeline = std::make_unique<OhaoVkPipeline>();
        if (!scenePipeline->initialize(device.get(), rp, shaderModules.get(), newExtent, descriptorSetLayout, OhaoVkPipeline::RenderMode::SOLID)) {
            throw std::runtime_error("Failed to recreate scene pipeline on resize!");
        }

        sceneGizmoPipeline = std::make_unique<OhaoVkPipeline>();
        if (!sceneGizmoPipeline->initialize(device.get(), rp, shaderModules.get(), newExtent, descriptorSetLayout, OhaoVkPipeline::RenderMode::GIZMO)) {
            throw std::runtime_error("Failed to recreate scene gizmo pipeline on resize!");
        }

        // Rebind pipelines into renderer
        sceneRenderer->setPipelinesWithWireframe(scenePipeline.get(), wireframePipeline.get(), sceneGizmoPipeline.get());

        float newAspect = static_cast<float>(lastWidth) / static_cast<float>(lastHeight);
        // Preserve perceived scale: if only height changed, keep horizontal FOV constant
        static uint32_t prevW = 0, prevH = 0;
        float prevAspect = (prevH > 0) ? static_cast<float>(prevW) / static_cast<float>(prevH) : newAspect;
        float fovY = camera.getFov();
        // Detect predominant dimension change
        bool widthChanged = (prevW != lastWidth);
        bool heightChanged = (prevH != lastHeight);
        if (!widthChanged && heightChanged) {
            // Keep horizontal FOV constant: hFOV0 = 2*atan(prevAspect*tan(fovY/2))
            float fovYRad = glm::radians(fovY);
            float hFOV0 = 2.0f * atanf(prevAspect * tanf(fovYRad * 0.5f));
            float newFovY = 2.0f * atanf(tanf(hFOV0 * 0.5f) / newAspect);
            camera.setFov(glm::degrees(newFovY));
        }
        camera.setAspectRatio(newAspect);
        prevW = lastWidth; prevH = lastHeight;

        needsResize = false;
        // Avoid recording/submitting while resources just changed this tick
        return;
    }
    if(window->wasResized()){
        recreateSwapChain();
        return;
    }

    // Wait for previous frame
    syncObjects->waitForFence(currentFrame);
    syncObjects->resetFence(currentFrame);
    
    // Reset acquire fence before using it (it must be unsignaled)
    VkFence acquireFence = syncObjects->getAcquireFence(currentFrame);
    vkResetFences(device->getDevice(), 1, &acquireFence);
    
    // Get next image - Use a separate fence for synchronization 
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device->getDevice(),
        swapchain->getSwapChain(),
        UINT64_MAX,
        VK_NULL_HANDLE,
        acquireFence,
        &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || window->wasResized()) {
        recreateSwapChain();
        return;
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }
    
    // Wait for the acquire fence to be signaled, then reset it
    vkWaitForFences(device->getDevice(), 1, &acquireFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device->getDevice(), 1, &acquireFence);

    // Reset and record command buffer
    commandManager->resetCommandBuffer(currentFrame);
    auto commandBuffer = commandManager->getCommandBuffer(currentFrame);

    // Begin command buffer recording
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer!");
    }

    // Update uniform buffer with latest camera information
    if (uniformBuffer) {
        // Collect lights from the scene using UnifiedLight
        std::vector<UnifiedLight> sceneLights;
        if (scene) {
            for (const auto& [actorId, actor] : scene->getAllActors()) {
                if (auto lightComponent = actor->getComponent<LightComponent>()) {
                    UnifiedLight light{};
                    light.position = actor->getTransform()->getPosition();
                    light.type = static_cast<float>(lightComponent->getLightType());
                    light.color = lightComponent->getColor();
                    light.intensity = lightComponent->getIntensity();
                    light.range = lightComponent->getRange();
                    // Always normalize direction to prevent shadow artifacts
                    glm::vec3 dir = lightComponent->getDirection();
                    light.direction = glm::length(dir) > 0.001f ? glm::normalize(dir) : glm::vec3(0.0f, -1.0f, 0.0f);
                    light.innerCone = lightComponent->getInnerConeAngle();
                    light.outerCone = lightComponent->getOuterConeAngle();
                    light.shadowMapIndex = -1;  // No shadow by default
                    light.lightSpaceMatrix = glm::mat4(1.0f);

                    sceneLights.push_back(light);
                }
            }
        }

        // Update lights in uniform buffer (using new unified system)
        auto& ubo = uniformBuffer->getCachedUBO();
        ubo.numLights = static_cast<int>(std::min(sceneLights.size(), static_cast<size_t>(MAX_UNIFIED_LIGHTS)));
        for (size_t i = 0; i < sceneLights.size() && i < MAX_UNIFIED_LIGHTS; ++i) {
            ubo.lights[i] = sceneLights[i];
        }

        // Find the first shadow-casting light (prioritize: directional > spot > point)
        const UnifiedLight* shadowLight = nullptr;
        int shadowLightIndex = -1;

        // First pass: look for directional light
        for (size_t i = 0; i < sceneLights.size(); ++i) {
            if (sceneLights[i].isDirectional()) {
                shadowLight = &sceneLights[i];
                shadowLightIndex = static_cast<int>(i);
                break;
            }
        }

        // Second pass: if no directional, look for spot light
        if (!shadowLight) {
            for (size_t i = 0; i < sceneLights.size(); ++i) {
                if (sceneLights[i].isSpot()) {
                    shadowLight = &sceneLights[i];
                    shadowLightIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        // Third pass: if no spot, look for point light
        if (!shadowLight) {
            for (size_t i = 0; i < sceneLights.size(); ++i) {
                if (sceneLights[i].isPoint()) {
                    shadowLight = &sceneLights[i];
                    shadowLightIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        // Update shadow parameters for any shadow-casting light
        if (shadowRenderer && shadowRenderer->isEnabled() && shadowLight) {
            // Calculate scene bounds for proper shadow frustum fitting
            glm::vec3 sceneMin(std::numeric_limits<float>::max());
            glm::vec3 sceneMax(std::numeric_limits<float>::lowest());
            bool hasObjects = false;

            if (scene) {
                for (const auto& [actorId, actor] : scene->getAllActors()) {
                    auto meshComp = actor->getComponent<MeshComponent>();
                    if (meshComp && meshComp->isVisible()) {
                        auto transform = actor->getTransform();
                        if (transform) {
                            glm::vec3 pos = transform->getPosition();
                            glm::vec3 scale = transform->getScale();
                            // Estimate object bounds (assume unit cube scaled)
                            float maxScale = std::max({scale.x, scale.y, scale.z});
                            sceneMin = glm::min(sceneMin, pos - glm::vec3(maxScale));
                            sceneMax = glm::max(sceneMax, pos + glm::vec3(maxScale));
                            hasObjects = true;
                        }
                    }
                }
            }

            // Calculate scene center from bounds (fallback to origin if no objects)
            glm::vec3 sceneCenter = hasObjects ? (sceneMin + sceneMax) * 0.5f : glm::vec3(0.0f);

            // Adjust ortho size based on scene bounds (only for directional lights)
            if (hasObjects && shadowLight->isDirectional()) {
                float sceneDiagonal = glm::length(sceneMax - sceneMin);
                float newOrthoSize = std::max(sceneDiagonal * 0.6f, 10.0f);
                shadowRenderer->setOrthoSize(newOrthoSize);

                // Debug: Log scene bounds for shadow frustum
                static bool loggedBounds = false;
                if (!loggedBounds) {
                    std::cout << "[Shadow Debug] Scene bounds: min=("
                              << sceneMin.x << "," << sceneMin.y << "," << sceneMin.z << ") max=("
                              << sceneMax.x << "," << sceneMax.y << "," << sceneMax.z << ")"
                              << " center=(" << sceneCenter.x << "," << sceneCenter.y << "," << sceneCenter.z << ")"
                              << " orthoSize=" << newOrthoSize << std::endl;
                    loggedBounds = true;
                }
            }

            glm::mat4 lightSpaceMatrix = shadowRenderer->calculateLightSpaceMatrix(*shadowLight, sceneCenter);

            // CRITICAL: Update shadow renderer's uniform buffer for shadow pass
            shadowRenderer->updateShadowUniforms(currentFrame, lightSpaceMatrix);

            // Store light space matrix in the shadow-casting light
            if (shadowLightIndex >= 0 && shadowLightIndex < MAX_UNIFIED_LIGHTS) {
                ubo.lights[shadowLightIndex].lightSpaceMatrix = lightSpaceMatrix;
                ubo.lights[shadowLightIndex].shadowMapIndex = 0;  // Use first shadow map

                // Debug logging (once per scene load)
                static bool s_debugPrintedOnce = false;
                if (!s_debugPrintedOnce) {
                    const char* lightTypeName = shadowLight->isDirectional() ? "Directional" :
                                               shadowLight->isSpot() ? "Spot" : "Point";
                    std::cout << "[Shadow Debug] " << lightTypeName << " Light " << shadowLightIndex << " configured for shadows:" << std::endl;
                    std::cout << "  Position: (" << shadowLight->position.x << ", "
                              << shadowLight->position.y << ", " << shadowLight->position.z << ")" << std::endl;
                    std::cout << "  Direction: (" << shadowLight->direction.x << ", "
                              << shadowLight->direction.y << ", " << shadowLight->direction.z << ")" << std::endl;
                    std::cout << "  ShadowMapIndex: " << ubo.lights[shadowLightIndex].shadowMapIndex << std::endl;
                    std::cout << "  ShadowBias: " << shadowRenderer->getShadowBias() << std::endl;
                    std::cout << "  ShadowStrength: " << shadowRenderer->getShadowStrength() << std::endl;

                    // Print light space matrix
                    std::cout << "  LightSpaceMatrix:" << std::endl;
                    for (int row = 0; row < 4; row++) {
                        std::cout << "    [";
                        for (int col = 0; col < 4; col++) {
                            std::cout << lightSpaceMatrix[col][row];
                            if (col < 3) std::cout << ", ";
                        }
                        std::cout << "]" << std::endl;
                    }

                    // Test transform of origin vertex
                    glm::vec4 testVertex(0.0f, 0.0f, 0.0f, 1.0f);
                    glm::vec4 transformed = lightSpaceMatrix * testVertex;
                    std::cout << "  Origin (0,0,0) transforms to: (" << transformed.x << ", "
                              << transformed.y << ", " << transformed.z << ", " << transformed.w << ")" << std::endl;
                    glm::vec3 ndc = glm::vec3(transformed) / transformed.w;
                    std::cout << "  Origin NDC: (" << ndc.x << ", " << ndc.y << ", " << ndc.z << ")" << std::endl;

                    s_debugPrintedOnce = true;
                }
            }

            // Also store in legacy single light space matrix for backward compatibility
            ubo.lightSpaceMatrix = lightSpaceMatrix;
            ubo.shadowBias = shadowRenderer->getShadowBias();
            ubo.shadowStrength = shadowRenderer->getShadowStrength();
        } else {
            // No shadows - set identity matrix and zero strength
            auto& ubo = uniformBuffer->getCachedUBO();
            ubo.lightSpaceMatrix = glm::mat4(1.0f);
            ubo.shadowBias = 0.005f;
            ubo.shadowStrength = 0.0f;
        }

        // Then update camera and write everything to GPU
        uniformBuffer->updateFromCamera(currentFrame, camera);
    }

    // Shadow pass: Render scene from light's perspective
    // Use old shadowRenderer (has working pipeline) but update to array descriptor
    if (shadowRenderer && shadowRenderer->isEnabled() && vertexBuffer && indexBuffer) {
        shadowRenderer->beginShadowPass(commandBuffer);
        shadowRenderer->renderShadowMap(commandBuffer, currentFrame);
        shadowRenderer->endShadowPass(commandBuffer);

        // Update shadow map array descriptor - put the shadow map at index 0
        if (descriptor && shadowRenderer->getShadowMapImageView() && shadowRenderer->getShadowMapSampler()) {
            // Create array with shadow map at index 0, placeholders for rest
            std::array<VkImageView, 4> shadowMapViews;

            // Use shadow renderer's shadow map for index 0
            shadowMapViews[0] = shadowRenderer->getShadowMapImageView();

            // Use placeholder for indices 1-3 (or shadow map pool's placeholders if available)
            if (shadowMapPool && shadowMapPool->isInitialized()) {
                auto poolViews = shadowMapPool->getAllImageViews();
                shadowMapViews[1] = poolViews[1];
                shadowMapViews[2] = poolViews[2];
                shadowMapViews[3] = poolViews[3];
            } else {
                // Fall back to using the same shadow map for all slots
                shadowMapViews[1] = shadowRenderer->getShadowMapImageView();
                shadowMapViews[2] = shadowRenderer->getShadowMapImageView();
                shadowMapViews[3] = shadowRenderer->getShadowMapImageView();
            }

            descriptor->updateShadowMapArrayDescriptor(currentFrame,
                shadowMapViews,
                shadowRenderer->getShadowMapSampler());
        }
    }

    // Explicitly begin the scene rendering pass (clear even when empty)
    if (sceneRenderer && sceneRenderer->hasValidRenderTarget()) {
        // Check if scene has any geometry to render
        static bool s_loggedEmptyOnce = false;
        bool hasGeometry = (vertexBuffer && indexBuffer);
        if (!hasGeometry) {
            if (!s_loggedEmptyOnce) {
                OHAO_LOG_DEBUG("Scene is empty - no geometry to render");
                s_loggedEmptyOnce = true;
            }
        } else {
            s_loggedEmptyOnce = false;
        }

        // Always run a scene pass to clear the viewport
        sceneRenderer->beginFrame();
        sceneRenderer->setWireframeMode(wireframeMode);
        // Always render; SceneRenderer handles empty scenes by drawing gizmo/grid
        sceneRenderer->render(uniformBuffer.get(), currentFrame);
        sceneRenderer->endFrame();
    } else {
        OHAO_LOG("Warning: Scene renderer not initialized or render target invalid");
        
        // Try to initialize scene renderer if needed
        if (sceneRenderer && !sceneRenderer->hasValidRenderTarget()) {
            initializeSceneRenderer();
        }
    }

    // Second pass: Main render pass with UI
    renderPass->begin(
        commandBuffer,
        framebufferManager->getFramebuffer(imageIndex),
        swapchain->getExtent(),
        {0.2f, 0.2f, 0.2f, 1.0f},
        1.0f,
        0
    );

    // Render ImGui
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);

    renderPass->end(commandBuffer);

    // End command buffer recording
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer!");
    }

    // Submit command buffer
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    
    // No wait semaphores since we're using fence-based synchronization
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = nullptr;
    submitInfo.pWaitDstStageMask = nullptr;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = commandManager->getCommandBufferPtr(currentFrame);

    VkSemaphore signalSemaphores[] = {syncObjects->getSwapchainRenderFinishedSemaphore(imageIndex)};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, syncObjects->getInFlightFence(currentFrame)) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {swapchain->getSwapChain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || window->wasResized()) {
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }
    // Advance to next frame
    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    renderPass->begin(commandBuffer,
                     framebufferManager->getFramebuffer(imageIndex),
                     swapchain->getExtent(),
                     {0.2f, 0.2f, 0.2f, 1.0f},
                     1.0f,
                     0);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchain->getExtent().width);
    viewport.height = static_cast<float>(swapchain->getExtent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain->getExtent();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // Legacy model rendering disabled - now using SceneRenderer
    // renderModel(commandBuffer);

    // Gizmo rendering temporarily disabled to debug outline issue
    // renderGizmos(commandBuffer);

    // Render ImGui
    if (ImGui::GetDrawData()) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    renderPass->end(commandBuffer);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer!");
    }
}

void VulkanContext::renderModel(VkCommandBuffer commandBuffer) {
    if (!vertexBuffer || !indexBuffer || !scene) {
        return;
    }
    
    // Get actors from scene
    const auto& actorsMap = scene->getAllActors();
    if (actorsMap.empty()) return;
    
    // Create a sorted list of actors to ensure predictable drawing order
    std::vector<Actor*> drawOrder;
    drawOrder.reserve(actorsMap.size());
    
    // First, collect actors with mesh components
    for (const auto& [actorId, actor] : actorsMap) {
        if (actor->getComponent<MeshComponent>()) {
            drawOrder.push_back(actor.get());
        }
    }

    // Sort by Z position to help with predictable rendering
    std::sort(drawOrder.begin(), drawOrder.end(), [](Actor* a, Actor* b) {
        // Sort back-to-front by Z position
        return a->getTransform()->getPosition().z < b->getTransform()->getPosition().z;
    });
    
    // Select appropriate pipeline
    auto& currentPipeline = wireframeMode ? wireframePipeline : modelPipeline;
    currentPipeline->bind(commandBuffer);

    // Bind vertex and index buffers only once - will use offsets for each draw
    VkBuffer vertexBuffers[] = {vertexBuffer->getBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);

    // Bind descriptor sets
    vkCmdBindDescriptorSets(commandBuffer,
                         VK_PIPELINE_BIND_POINT_GRAPHICS,
                         currentPipeline->getPipelineLayout(),
                         0, 1, &descriptor->getSet(currentFrame),
                         0, nullptr);

    // Iterate through actors in our draw order
    for (const auto* actor : drawOrder) {
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (!meshComponent || !meshComponent->getModel()) continue;
        
        // Find buffer info for this actor using ID
        auto it = meshBufferMap.find(actor->getID());
        if (it == meshBufferMap.end()) {
            OHAO_LOG("Actor not found in mesh buffer map: " + actor->getName());
            continue;
        }

        const auto& bufferInfo = it->second;
        
        // Set up model push constants
        OhaoVkPipeline::ModelPushConstants pushConstants{};
        pushConstants.model = actor->getTransform()->getWorldMatrix();
        
        // Get material properties from material component
        auto materialComponent = actor->getComponent<MaterialComponent>();
        if (materialComponent) {
            const auto& material = materialComponent->getMaterial();
            pushConstants.baseColor = material.baseColor;
            pushConstants.metallic = material.metallic;
            pushConstants.roughness = material.roughness;
            pushConstants.ao = material.ao;
        } else {
            // Default material if no material component
            pushConstants.baseColor = glm::vec3(0.8f, 0.8f, 0.8f);
            pushConstants.metallic = 0.0f;
            pushConstants.roughness = 0.5f;
            pushConstants.ao = 1.0f;
        }
        
        // Update push constants for this actor
        vkCmdPushConstants(
            commandBuffer,
            currentPipeline->getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(OhaoVkPipeline::ModelPushConstants),
            &pushConstants
        );
        
        // Draw using the actual indices - note we're using indexOffset and not vertexOffset here
        vkCmdDrawIndexed(
            commandBuffer,
            bufferInfo.indexCount,
            1,
            bufferInfo.indexOffset,
            0,
            0
        );
        
        // Debug log our draw command
        OHAO_LOG_DEBUG("Drawing actor '" + actor->getName() + 
                       "' with " + std::to_string(bufferInfo.indexCount) + 
                       " indices at offset " + std::to_string(bufferInfo.indexOffset));
    }
}

void VulkanContext::renderGizmos(VkCommandBuffer commandBuffer) {
    if (axisGizmo) {
        gizmoPipeline->bind(commandBuffer);

        // Set dynamic states after binding pipeline
        vkCmdSetLineWidth(commandBuffer, 2.0f);

        // Bind buffers and render
        VkBuffer vertexBuffers[] = {axisGizmo->getVertexBuffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, axisGizmo->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commandBuffer,
                               VK_PIPELINE_BIND_POINT_GRAPHICS,
                               gizmoPipeline->getPipelineLayout(),
                               0, 1, &descriptor->getSet(currentFrame),
                               0, nullptr);
        axisGizmo->render(commandBuffer, camera.getViewProjectionMatrix());
    }
}

void VulkanContext::createVertexBuffer(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        throw std::runtime_error("Attempting to create vertex buffer with empty vertices");
    }

    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

    vertexBuffer = std::make_unique<OhaoVkBuffer>();
    if (!vertexBuffer) {
        throw std::runtime_error("Failed to allocate vertex buffer");
    }

    vertexBuffer->initialize(device.get());

    if (!OhaoVkBuffer::createWithStaging(
        device.get(),
        commandManager->getCommandPool(),
        vertices.data(),
        bufferSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        *vertexBuffer)) {
        throw std::runtime_error("Failed to create vertex buffer!");
    }
}

void VulkanContext::createIndexBuffer(const std::vector<uint32_t>& indices) {
    if (indices.empty()) {
        throw std::runtime_error("Attempting to create index buffer with empty indices");
    }

    VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

    indexBuffer = std::make_unique<OhaoVkBuffer>();
    if (!indexBuffer) {
        throw std::runtime_error("Failed to allocate index buffer");
    }

    indexBuffer->initialize(device.get());

    if (!OhaoVkBuffer::createWithStaging(
        device.get(),
        commandManager->getCommandPool(),
        indices.data(),
        bufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        *indexBuffer)) {
        throw std::runtime_error("Failed to create index buffer!");
    }
}

bool VulkanContext::importModel(const std::string& filename) {
    if (!scene) {
        OHAO_LOG_ERROR("No active scene!");
        return false;
    }

    // Delegate to scene's new component pack-based import system
    bool success = scene->importModel(filename);
    
    if (success) {
        // Update scene buffers after successful import
        if (!updateSceneBuffers()) {
            OHAO_LOG_ERROR("Failed to update scene buffers after model import");
            return false;
        }
        OHAO_LOG("Successfully imported model via new component pack system: " + filename);
    }
    
    return success;
}

void VulkanContext::cleanupCurrentModel() {
    vertexBuffer.reset();
    indexBuffer.reset();
}

bool VulkanContext::hasLoadScene(){
    return scene != nullptr && vertexBuffer != nullptr && indexBuffer != nullptr;
}

void VulkanContext::updateViewport(uint32_t width, uint32_t height){
    if(width == 0 || height == 0)return ;

    if (width != this->width || height != this->height) {
        this->width = width;
        this->height = height;

        device->waitIdle();

        if (framebufferManager) {
            framebufferManager->cleanup();
        }

        if (!swapchain->recreate(width, height)) {
            throw std::runtime_error("Failed to recreate swapchain!");
        }

        // Recreate dependent resources
        if (!framebufferManager->initialize(device.get(), swapchain.get(), renderPass.get(), depthImage.get())) {
            throw std::runtime_error("Failed to recreate framebuffers!");
        }
    }
}

void VulkanContext::setViewportSize(uint32_t width, uint32_t height){
    if (width != lastWidth || height != lastHeight) {
        lastWidth = width;
        lastHeight = height;
        needsResize = true;
    }
}

void VulkanContext::recreateSwapChain(){
    int width = 0, height = 0;
    glfwGetFramebufferSize(window->getGLFWWindow(), &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window->getGLFWWindow(), &width, &height);
        glfwWaitEvents();
    }
    device->waitIdle();
    cleanupSwapChain();

    if (!swapchain->recreate(width, height)) {
        throw std::runtime_error("Failed to recreate swap chain!");
    }

    // Reinitialize swapchain-specific semaphores with new image count
    if(!syncObjects->initializeSwapchainSemaphores(swapchain->getImages().size())){
        throw std::runtime_error("Failed to recreate swapchain sync objects!");
    }

    if (!renderPass->initialize(device.get(), swapchain.get())) {
        throw std::runtime_error("Failed to recreate render pass!");
    }

    if (!depthImage->createDepthResources(swapchain->getExtent(), msaaSamples)) {
        throw std::runtime_error("Failed to recreate depth resources!");
    }

    if (!framebufferManager->initialize(device.get(), swapchain.get(), renderPass.get(), depthImage.get())) {
        throw std::runtime_error("Failed to recreate framebuffers!");
    }
}

void VulkanContext::cleanupSwapChain() {
    device->waitIdle();

    // Wait for all fences
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        syncObjects->waitForFence(i);
    }
    framebufferManager->cleanup();
    depthImage->cleanup();
    renderPass->cleanup();
    swapchain->cleanup();
}

bool VulkanContext::updateModelBuffers(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.empty()) {
        OHAO_LOG_ERROR("Cannot update buffers with empty data");
        return false;
    }

    try {
        device->waitIdle();
        createVertexBuffer(vertices);
        createIndexBuffer(indices);
        return true;
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to update model buffers: " + std::string(e.what()));
        return false;
    }
}
bool VulkanContext::updateSceneBuffers() {
    if (!scene) {
        OHAO_LOG("Cannot update buffers - no scene exists");
        return false;
    }
    
    device->waitIdle();

    // Clear old mappings first
    meshBufferMap.clear();

    // Debug output for initial state
    OHAO_LOG("Starting scene buffer update");
    OHAO_LOG("Number of actors: " + std::to_string(scene->getAllActors().size()));

    // First pass: Count actors with valid meshes
    size_t actorsWithMeshes = 0;
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    
    // Get actors with valid mesh components for processing
    std::vector<std::pair<Actor*, std::shared_ptr<Model>>> actorsWithModels;
    
    // Iterate through all actors with mesh components
    for (const auto& [actorId, actor] : scene->getAllActors()) {
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (meshComponent && meshComponent->getModel()) {
            auto model = meshComponent->getModel();
            totalVertices += model->vertices.size();
            totalIndices += model->indices.size();
            actorsWithMeshes++;
            
            // Store for second pass
            actorsWithModels.push_back({actor.get(), model});
            
            OHAO_LOG("Actor '" + actor->getName() + 
                     "' at position " + 
                     std::to_string(actor->getTransform()->getPosition().x) + ", " +
                     std::to_string(actor->getTransform()->getPosition().y) + ", " +
                     std::to_string(actor->getTransform()->getPosition().z) +
                     " requires " + std::to_string(model->vertices.size()) + 
                     " vertices and " + std::to_string(model->indices.size()) + " indices");
        }
    }
    
    // Check if we have anything to render
    if (actorsWithMeshes == 0) {
        OHAO_LOG("No actors with mesh components found in scene");
        cleanupCurrentModel(); // Clean up any old buffers
        return false;
    }

    // Pre-allocate buffers with exact size
    std::vector<Vertex> combinedVertices;
    std::vector<uint32_t> combinedIndices;
    combinedVertices.reserve(totalVertices);
    combinedIndices.reserve(totalIndices);

    // Second pass: Build combined buffers in Z-order (optional sort by Z position)
    OHAO_LOG("Building combined vertex and index buffers for " + std::to_string(actorsWithModels.size()) + " models");
    
    for (const auto& [actor, model] : actorsWithModels) {
        // Store vertex and index counts for this model before adding
        const uint32_t vertexOffset = static_cast<uint32_t>(combinedVertices.size());
        const uint32_t indexOffset = static_cast<uint32_t>(combinedIndices.size());
        const uint32_t indexCount = static_cast<uint32_t>(model->indices.size());
        
        // Create buffer info and store in our map with actor as key
        MeshBufferInfo bufferInfo{};
        bufferInfo.vertexOffset = vertexOffset;
        bufferInfo.indexOffset = indexOffset;
        bufferInfo.indexCount = indexCount;
        meshBufferMap[actor->getID()] = bufferInfo;
        
        // Add vertices directly without modification
        combinedVertices.insert(combinedVertices.end(), model->vertices.begin(), model->vertices.end());
        
        // Add indices with correct offset
        for (uint32_t index : model->indices) {
            // Each index needs to be offset by the cumulative vertex count
            combinedIndices.push_back(index + vertexOffset);
        }
        
        OHAO_LOG("Added actor '" + actor->getName() + "' to buffer at vertex offset " +
                 std::to_string(bufferInfo.vertexOffset) + ", index offset " +
                 std::to_string(bufferInfo.indexOffset) + " with " +
                 std::to_string(bufferInfo.indexCount) + " indices");
    }

    // Extra verification step
    if (combinedVertices.size() != totalVertices || combinedIndices.size() != totalIndices) {
        OHAO_LOG("Buffer size mismatch! Expected " + std::to_string(totalVertices) + 
                " vertices and " + std::to_string(totalIndices) + " indices, but got " +
                std::to_string(combinedVertices.size()) + " vertices and " + 
                std::to_string(combinedIndices.size()) + " indices");
    }

    // Verify we have data to upload
    if (combinedVertices.empty() || combinedIndices.empty()) {
        OHAO_LOG("No geometry to update - empty buffers");
        cleanupCurrentModel();
        return false;
    }

    try {
        // Clean up old buffers before creating new ones
        cleanupCurrentModel();

        // Create new buffers
        createVertexBuffer(combinedVertices);
        createIndexBuffer(combinedIndices);

        OHAO_LOG("Successfully updated scene buffers with " +
                 std::to_string(combinedVertices.size()) + " vertices and " +
                 std::to_string(combinedIndices.size()) + " indices across " +
                 std::to_string(actorsWithMeshes) + " actors");
        
        // Mark scene as modified
        markSceneModified();
        return true;

    } catch (const std::exception& e) {
        OHAO_LOG("Failed to update scene buffers: " + std::string(e.what()));
        // Try to ensure clean state on failure
        cleanupCurrentModel();
        return false;
    }
}

bool VulkanContext::createNewScene(const std::string& name) {
    device->waitIdle();

    scene = std::make_unique<Scene>();
    scene->setName(name);

    // Reset scene-related resources
    cleanupCurrentModel();
    initializeDefaultScene();

    return true;
}

bool VulkanContext::saveScene(const std::string& filename) {
    if (!scene) return false;

    if (scene->saveToFile(filename)) {
        sceneModified = false;
        return true;
    }
    return false;
}

bool VulkanContext::loadScene(const std::string& filename) {
    if (!scene) scene = std::make_unique<Scene>();

    return scene->loadFromFile(filename);
}

std::shared_ptr<Model> VulkanContext::generateSphereMesh() {
    auto model = std::make_shared<Model>();
    
    const float radius = 1.0f;
    const int sectors = 36;  // longitude
    const int stacks = 18;   // latitude
    
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    
    // Generate vertices
    for (int i = 0; i <= stacks; ++i) {
        float phi = glm::pi<float>() * float(i) / float(stacks);
        float sinPhi = sin(phi);
        float cosPhi = cos(phi);
        
        for (int j = 0; j <= sectors; ++j) {
            float theta = 2.0f * glm::pi<float>() * float(j) / float(sectors);
            float sinTheta = sin(theta);
            float cosTheta = cos(theta);
            
            float x = cosTheta * sinPhi;
            float y = cosPhi;
            float z = sinTheta * sinPhi;
            
            Vertex vertex;
            vertex.position = {x * radius, y * radius, z * radius};
            vertex.normal = {x, y, z};  // Normalized position = normal for sphere
            vertex.color = {1.0f, 1.0f, 1.0f};
            vertex.texCoord = {float(j) / sectors, float(i) / stacks};
            
            vertices.push_back(vertex);
        }
    }
    
    // Generate indices
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < sectors; ++j) {
            int first = i * (sectors + 1) + j;
            int second = first + sectors + 1;
            
            indices.push_back(first);
            indices.push_back(second);
            indices.push_back(first + 1);
            
            indices.push_back(second);
            indices.push_back(second + 1);
            indices.push_back(first + 1);
        }
    }
    
    model->vertices = vertices;
    model->indices = indices;
    
    // Setup default material
    MaterialData defaultMaterial;
    defaultMaterial.name = "Default";
    defaultMaterial.ambient = glm::vec3(0.2f);
    defaultMaterial.diffuse = glm::vec3(0.8f);
    defaultMaterial.specular = glm::vec3(0.5f);
    defaultMaterial.shininess = 32.0f;
    model->materials["default"] = defaultMaterial;
    
    return model;
}

}//namespace ohao


