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
#include "ui/system/ui_manager.hpp"

#define OHAO_ENABLE_VALIDATION_LAYER true

namespace ohao{

VulkanContext::VulkanContext(GLFWwindow* windowHandle): window(windowHandle){
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
}

VulkanContext::~VulkanContext(){cleanup();}

void
VulkanContext::initializeVulkan(){
    //vulkan setup
    instance = std::make_unique<OhaoVkInstance>();
    if (!instance->initialize("OHAO Engine", OHAO_ENABLE_VALIDATION_LAYER)) {
        throw std::runtime_error("engine instance initialization failed!");
    }
    surface = std::make_unique<OhaoVkSurface>();
    if (!surface->initialize(instance.get(), window)){
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
            OhaoVkShaderModule::ShaderType::FRAGMENT)) {
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

    pipeline = std::make_unique<OhaoVkPipeline>();
    if(!pipeline->initialize(device.get(), renderPass.get(), shaderModules.get(), swapchain->getExtent(), descriptor->getLayout())){
        throw std::runtime_error("engine pipeline initialization failed!");
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
    scenePipeline.reset();
    pipeline.reset();
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

    // Create scene-specific pipeline
    scenePipeline = std::make_unique<OhaoVkPipeline>();
    if (!scenePipeline->initialize(
            device.get(),
            sceneRenderer->getRenderTarget()->getRenderPass(),
            shaderModules.get(),
            VkExtent2D{viewportSize.width, viewportSize.height},
            descriptor->getLayout())) {
        throw std::runtime_error("Failed to initialize scene pipeline!");
    }

    // Update the scene renderer to use the new pipeline
    sceneRenderer->setPipeline(scenePipeline.get());
}

void VulkanContext::drawFrame() {
    if (!uiManager) {
        throw std::runtime_error("UI Manager not set!");
    }

    // Wait for previous frame
    syncObjects->waitForFence(currentFrame);
    syncObjects->resetFence(currentFrame);

    // Get next image
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device->getDevice(),
        swapchain->getSwapChain(),
        UINT64_MAX,
        syncObjects->getImageAvailableSemaphore(currentFrame),
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }


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
    if (hasLoadScene()) {

        auto scene = getScene();
        if (scene && !scene->getLights().empty()) {
            const auto& light = scene->getLights().begin()->second;
            uniformBuffer->setLightProperties(
                light.position,
                light.color,
                light.intensity
            );
        }
        if (!sceneRenderer->hasValidRenderTarget()) {
            initializeSceneRenderer();
        }
        sceneRenderer->render(uniformBuffer.get(), currentFrame);
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
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swap chain image!");
    }

    // Advance to next frame
    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if(vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    renderPass->begin(commandBuffer,
                     framebufferManager->getFramebuffer(imageIndex),
                     swapchain->getExtent(),
                     {0.2f, 0.2f, 0.2f, 1.0f},
                     1.0f,
                     0);

    // Set viewport and scissor for both scene and ImGui
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

    // Only render scene if we have one loaded
    if (vertexBuffer && indexBuffer && scene) {
        // Bind pipeline and set viewport/scissor
        pipeline->bind(commandBuffer);

        // Bind vertex and index buffers
        VkBuffer vertexBuffers[] = {vertexBuffer->getBuffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);

        // Bind descriptor sets
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline->getPipelineLayout(),
            0, 1,
            &descriptor->getSet(currentFrame),
            0, nullptr
        );

        // Draw the scene
        auto sceneObjects = scene->getObjects();
        if(sceneObjects.empty()) {
            throw std::runtime_error("Scene object is empty!");
        }
        auto mainObject = sceneObjects.begin()->second;
        vkCmdDrawIndexed(
            commandBuffer,
            static_cast<uint32_t>(mainObject->model->indices.size()),
            1, 0, 0, 0
        );
    }

    // Render ImGui
    if (ImGui::GetDrawData()) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    renderPass->end(commandBuffer);
    if(vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer!");
    }
}

void
VulkanContext::createVertexBuffer(const std::vector<Vertex>& vertices){
    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

    vertexBuffer = std::make_unique<OhaoVkBuffer>();
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

void
VulkanContext::createIndexBuffer(const std::vector<uint32_t>& indices){
    VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

    indexBuffer = std::make_unique<OhaoVkBuffer>();
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
        scene->loadFromFile(filename);
        auto sceneObjects = scene->getObjects();
        if (sceneObjects.empty()) {
            throw std::runtime_error("No objects loaded in scene!");
        }
        auto mainObject = sceneObjects.begin()->second;
        createVertexBuffer(mainObject->model->vertices);
        createIndexBuffer(mainObject->model->indices);

        // Initialize uniform buffer with scene data
        UniformBufferObject initialUBO{};
        initialUBO.model = glm::mat4(1.0f);
        initialUBO.view = camera.getViewMatrix();
        initialUBO.proj = camera.getProjectionMatrix();
        initialUBO.viewPos = camera.getPosition();
        initialUBO.proj[1][1] *= -1;

        // Get light data from scene
        const auto& lights = scene->getLights();
        if (!lights.empty()) {
            const auto& light = lights.begin()->second;
            initialUBO.lightPos = light.position;
            initialUBO.lightColor = light.color;
            initialUBO.lightIntensity = light.intensity;
        }else{
            initialUBO.lightPos = glm::vec3(0.0f, 0.9f, 0.0f);
            initialUBO.lightColor = glm::vec3(1.0f);
            initialUBO.lightIntensity = 1.0f;
        }

        // Write initial values to all uniform buffers
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            uniformBuffer->writeToBuffer(i, &initialUBO, sizeof(UniformBufferObject));
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load model: " << e.what() << std::endl;
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

}//namespace ohao
