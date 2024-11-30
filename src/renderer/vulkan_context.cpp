#include "vulkan_context.hpp"
#include <GLFW/glfw3.h>
#include <alloca.h>
#include <chrono>
#include <cmath>
#include <cstddef>
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
#include <vk/ohao_vk_command_manager.hpp>
#include <vk/ohao_vk_descriptor.hpp>
#include <vk/ohao_vk_device.hpp>
#include <vk/ohao_vk_framebuffer.hpp>
#include <vk/ohao_vk_image.hpp>
#include <vk/ohao_vk_physical_device.hpp>
#include <vk/ohao_vk_pipeline.hpp>
#include <vk/ohao_vk_render_pass.hpp>
#include <vk/ohao_vk_surface.hpp>
#include <vk/ohao_vk_swapchain.hpp>
#include <vk/ohao_vk_instance.hpp>
#include <vk/ohao_vk_shader_module.hpp>
#include <vk/ohao_vk_sync_objects.hpp>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#define OHAO_ENABLE_VALIDATION_LAYER true

namespace ohao{

VulkanContext::VulkanContext(){}

VulkanContext::VulkanContext(GLFWwindow* windowHandle): window(windowHandle){}

VulkanContext::~VulkanContext(){cleanup();}

bool
VulkanContext::initialize(){
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

    createDepthResources();

    framebufferManager = std::make_unique<OhaoVkFramebuffer>();
    if(!framebufferManager->initialize(device.get(), swapchain.get(), renderPass.get(), depthImage.get())){
        throw std::runtime_error("engine framebuffer manager initialization failed!");
    }

    createUniformBuffers();

    descriptor = std::make_unique<OhaoVkDescriptor>();
    if(!descriptor->initialize(device.get(), MAX_FRAMES_IN_FLIGHT)) {
        throw std::runtime_error("engine descriptor system initialization failed!");
    }
    if(!descriptor->createDescriptorSets(uniformBuffers, sizeof(UniformBufferObject))){
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

    //load model
    scene = std::make_unique<Scene>();
    scene->loadFromFile("assets/models/cornell_box.obj");
    auto sceneObjects = scene->getObjects();
    if (sceneObjects.empty()) {
        throw std::runtime_error("No objects loaded in scene!");
    }
    auto mainObject = sceneObjects.begin()->second;

    createVertexBuffer(mainObject->model->vertices);
    createIndexBuffer(mainObject->model->indices);

    // Look for light material and apply its properties
    for (const auto& [name, light] : scene->getLights()) {
        updateLight(light.position, light.color, light.intensity);
    }
    updateMaterial(mainObject->material);
    return true;
}

void
VulkanContext::cleanup(){
    depthImage.reset();

    for(auto& uniformBuffer: uniformBuffers){
        uniformBuffer.reset();
    }
    indexBuffer.reset();
    vertexBuffer.reset();
    descriptor.reset();
    syncObjects.reset();
    commandManager.reset();
    framebufferManager.reset();
    pipeline.reset();
    renderPass.reset();
    shaderModules.reset();
    swapchain.reset();
    device.reset();
    physicalDevice.reset();
    surface.reset();
    instance.reset();
}

void
VulkanContext::drawFrame(){
    syncObjects->waitForFence(currentFrame);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(device->getDevice(), swapchain->getSwapChain(), UINT64_MAX,
                    syncObjects->getImageAvailableSemaphore(currentFrame), VK_NULL_HANDLE, &imageIndex);

    if(result != VK_SUCCESS){
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    updateUniformBuffer(currentFrame);
    syncObjects->resetFence(currentFrame);
    commandManager->resetCommandBuffer(currentFrame);
    recordCommandBuffer(commandManager->getCommandBuffer(currentFrame), imageIndex);


    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[]{syncObjects->getImageAvailableSemaphore(currentFrame)};
    VkPipelineStageFlags waitStages[] {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = commandManager->getCommandBufferPtr(currentFrame);

    VkSemaphore signalSemaphores[]{syncObjects->getRenderFinishedSemaphore(currentFrame)};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if(vkQueueSubmit(graphicsQueue, 1, &submitInfo, syncObjects->getInFlightFence(currentFrame)) != VK_SUCCESS){
        throw std::runtime_error("failed to submit draw command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {swapchain->getSwapChain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue, &presentInfo);
    if(result != VK_SUCCESS){
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1 ) % MAX_FRAMES_IN_FLIGHT;
}

void
VulkanContext::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if(vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    renderPass->begin(commandBuffer, framebufferManager->getFramebuffer(imageIndex), swapchain->getExtent(),
                    {0.2f, 0.2f, 0.2f, 1.0f},  // clear color
                    1.0f,                       // clear depth
                    0);                          // clear stencil

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

    vkCmdEndRenderPass(commandBuffer);



    if(vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer!");
    }
}


void
VulkanContext::createUniformBuffers(){
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        uniformBuffers[i] = std::make_unique<OhaoVkBuffer>();
        uniformBuffers[i]->initialize(device.get());

        if (!uniformBuffers[i]->create(
            bufferSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            throw std::runtime_error("Failed to create uniform buffer!");
        }

        // Map the memory for the entire lifetime
        if (!uniformBuffers[i]->map()) {
            throw std::runtime_error("Failed to map uniform buffer!");
        }
        uniformBuffersMapped[i] = uniformBuffers[i]->getMappedMemory();
    }
}



void
VulkanContext::updateUniformBuffer(uint32_t currentImage){
    static auto startTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

    UniformBufferObject ubo{};
    ubo.model = glm::mat4(1.0f);
    ubo.view = camera.getViewMatrix();
    ubo.proj = camera.getProjectionMatrix();
    ubo.viewPos = camera.getPosition();
    ubo.proj[1][1] *= -1; //flip Y coordinate

    if (glm::length(ubo.lightColor) == 0.0f) {
        ubo.lightPos = glm::vec3(0.0f, 2.5f, 0.0f);
        ubo.lightColor = glm::vec3(1.0f);
        ubo.lightIntensity = 10.0f;
        ubo.baseColor = glm::vec3(0.8f);
        ubo.metallic = 0.0f;
        ubo.roughness = 0.5f;
        ubo.ao = 1.0f;
    }
    uniformBuffers[currentImage]->writeToBuffer(&ubo, sizeof(ubo));

    descriptor->updateDescriptorSet(currentImage, *uniformBuffers[currentImage], sizeof(ubo));
}

uint32_t
VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties){
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice->getDevice(), &memProperties);

    for(uint32_t i = 0; i < memProperties.memoryTypeCount; i++){
        if((typeFilter & ( 1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties){
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
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

void
VulkanContext::updateMaterial(const Material& material){
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        UniformBufferObject* ubo = static_cast<UniformBufferObject*>(uniformBuffersMapped[i]);
        ubo->baseColor = material.baseColor;
        ubo->metallic = material.metallic;
        ubo->roughness = material.roughness;
        ubo->ao = material.ao;
    }
}

void
VulkanContext::updateLight(const glm::vec3& position, const glm::vec3& color, float intensity){
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        UniformBufferObject* ubo = static_cast<UniformBufferObject*>(uniformBuffersMapped[i]);
        ubo->lightPos = position;
        ubo->lightColor = color;
        ubo->lightIntensity = intensity;
    }
}

void
VulkanContext::createDepthResources(){
    VkFormat depthFormat = OhaoVkImage::findDepthFormat(device.get());
    depthImage = std::make_unique<OhaoVkImage>();
    depthImage->initialize(device.get());

    if(!depthImage->createImage(swapchain->getExtent().width, swapchain->getExtent().height,
        depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, msaaSamples)){

        throw std::runtime_error("failed to create depth image!");
    }

    if(!depthImage->createImageView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT)){
        throw std::runtime_error("failed to create depth image view!");
    }
}


}//namespace ohao
