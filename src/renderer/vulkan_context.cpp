#include "vulkan_context.hpp"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <GLFW/glfw3.h>
#include <alloca.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
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
#include "ui/components/console_widget.hpp"
#include "ui/system/ui_manager.hpp"
#include "ui/window/window.hpp"

#define OHAO_ENABLE_VALIDATION_LAYER true

namespace ohao{
VulkanContext* VulkanContext::contextInstance = nullptr;

VulkanContext::VulkanContext(Window* windowHandle): window(windowHandle){
    contextInstance = this;
    int w, h;
    glfwGetFramebufferSize(window->getGLFWWindow(), &w, &h);
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    camera.setPosition(glm::vec3(0.3f, 0.0f, 3.0f));
    camera.setRotation(0.0f, -90.0f);  // Look at origin
    camera.setPerspectiveProjection(45.0f, float(width)/float(height), 0.1f, 100.0f);
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

    if (!shaderModules->createShaderModule( "vert", "shaders/shader.vert.spv",OhaoVkShaderModule::ShaderType::VERTEX) ||
        !shaderModules->createShaderModule("frag", "shaders/shader.frag.spv", OhaoVkShaderModule::ShaderType::FRAGMENT) ||
        !shaderModules->createShaderModule("gizmo_vert", "shaders/gizmo.vert.spv", OhaoVkShaderModule::ShaderType::VERTEX) ||
        !shaderModules->createShaderModule("gizmo_frag", "shaders/gizmo.frag.spv", OhaoVkShaderModule::ShaderType::FRAGMENT)
     ) {
        throw std::runtime_error("failed to create shader modules!");
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

    sceneRenderer = std::make_unique<SceneRenderer>();
    if(!sceneRenderer->initialize(this)){
        throw std::runtime_error("engine scene renderer initializatin failed");
    }
}
void
VulkanContext::cleanup(){
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
    sceneGizmoPipeline.reset();
    scenePipeline.reset();
    renderPass.reset();
    shaderModules.reset();
    swapchain.reset();
    device.reset();
    physicalDevice.reset();
    surface.reset();
    instance.reset();
}

void VulkanContext::initializeSceneRenderer() {
    if (!uiManager) {
        throw std::runtime_error("UIManager must be set before initializing scene renderer");
    }

    auto viewportSize = uiManager->getSceneViewportSize();
    if (!sceneRenderer->initializeRenderTarget(
            static_cast<uint32_t>(viewportSize.width),
            static_cast<uint32_t>(viewportSize.height))) {
        throw std::runtime_error("Failed to initialize scene render target");
    }

    // Create scene-specific pipelines
    scenePipeline = std::make_unique<OhaoVkPipeline>();
    if (!scenePipeline->initialize(
            device.get(),
            sceneRenderer->getRenderTarget()->getRenderPass(),
            shaderModules.get(),
            VkExtent2D{viewportSize.width, viewportSize.height},
            descriptor->getLayout(),
            OhaoVkPipeline::RenderMode::SOLID)) {
        throw std::runtime_error("Failed to initialize scene pipeline!");
    }

    sceneGizmoPipeline = std::make_unique<OhaoVkPipeline>();
    if (!sceneGizmoPipeline->initialize(
            device.get(),
            sceneRenderer->getRenderTarget()->getRenderPass(),
            shaderModules.get(),
            VkExtent2D{viewportSize.width, viewportSize.height},
            descriptor->getLayout(),
            OhaoVkPipeline::RenderMode::GIZMO)) {
        throw std::runtime_error("Failed to initialize scene gizmo pipeline!");
    }

    // Update the scene renderer to use both pipelines
    sceneRenderer->setPipelines(scenePipeline.get(), sceneGizmoPipeline.get());
}

void VulkanContext::drawFrame() {
    if (!uiManager) {
        throw std::runtime_error("UI Manager not set!");
    }
    if(window->wasResized()){
        recreateSwapChain();
        return;
    }

    // Wait for previous frame
    syncObjects->waitForFence(currentFrame);
    // Get next image
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device->getDevice(),
        swapchain->getSwapChain(),
        1000000000,
        syncObjects->getImageAvailableSemaphore(currentFrame),
        VK_NULL_HANDLE,
        &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || window->wasResized()) {
        recreateSwapChain();
        return;
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    syncObjects->resetFence(currentFrame);


    // Reset and record command buffer
    commandManager->resetCommandBuffer(currentFrame);
    auto commandBuffer = commandManager->getCommandBuffer(currentFrame);

    // Begin command buffer recording
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer!");
    }

    uniformBuffer->updateFromCamera(currentFrame, camera);
    // First pass: Render scene to scene render target

    // Always initialize and render scene
    if (!sceneRenderer->hasValidRenderTarget()) {
        initializeSceneRenderer();
    }
    sceneRenderer->render(uniformBuffer.get(), currentFrame);

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

    VkSemaphore waitSemaphores[] = {syncObjects->getImageAvailableSemaphore(currentFrame)};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = commandManager->getCommandBufferPtr(currentFrame);

    VkSemaphore signalSemaphores[] = {syncObjects->getRenderFinishedSemaphore(currentFrame)};
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

    // Render model
    renderModel(commandBuffer);

    // Render gizmos
    renderGizmos(commandBuffer);

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
    if (vertexBuffer && indexBuffer && scene) {
        auto sceneObjects = scene->getObjects();
        if (!sceneObjects.empty()) {
            auto mainObject = sceneObjects.begin()->second;
            if (mainObject && mainObject->getModel()) {
                // Select appropriate pipeline
                auto& currentPipeline = wireframeMode ? wireframePipeline : modelPipeline;
                currentPipeline->bind(commandBuffer);

                // Bind vertex and index buffers
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

                // Draw model
                vkCmdDrawIndexed(commandBuffer,
                               static_cast<uint32_t>(mainObject->getModel()->indices.size()),
                               1, 0, 0, 0);
            }
        }
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

bool VulkanContext::loadModel(const std::string& filename) {
    cleanupCurrentModel();

    try {
        scene = std::make_unique<Scene>();
        if (!scene->loadFromFile(filename)) {
            OHAO_LOG_ERROR("Scene::loadFromFile failed for: " + filename);
            return false;
        }

        auto sceneObjects = scene->getObjects();
        if (sceneObjects.empty()) {
            OHAO_LOG_ERROR("No objects loaded in scene!");
            return false;
        }

        auto mainObject = sceneObjects.begin()->second;
        if (!mainObject || !mainObject->getModel()) {
            OHAO_LOG_ERROR("Invalid main object or model!");
            return false;
        }

        const auto& modelVertices = mainObject->getModel()->vertices;
        const auto& modelIndices = mainObject->getModel()->indices;

        OHAO_LOG_DEBUG("Model data: " + std::to_string(modelVertices.size()) + " vertices, "
                      + std::to_string(modelIndices.size()) + " indices");

        if (modelVertices.empty() || modelIndices.empty()) {
            OHAO_LOG_ERROR("Model has no geometry data!");
            return false;
        }

        // Create vertex and index buffers
        try {
            createVertexBuffer(modelVertices);
            OHAO_LOG_DEBUG("Vertex buffer created successfully");
            createIndexBuffer(modelIndices);
            OHAO_LOG_DEBUG("Index buffer created successfully");
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Failed to create buffers: " + std::string(e.what()));
            cleanupCurrentModel();
            return false;
        }

        OHAO_LOG("Model loaded successfully: " + filename);
        return true;

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Exception during model loading: " + std::string(e.what()));
        cleanupCurrentModel();
        return false;
    }
}

void VulkanContext::cleanupCurrentModel() {
    vertexBuffer.reset();
    indexBuffer.reset();
    scene.reset();
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

}//namespace ohao
