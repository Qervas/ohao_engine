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
#include <vk/ohao_vk_device.hpp>
#include <vk/ohao_vk_physical_device.hpp>
#include <vk/ohao_vk_render_pass.hpp>
#include <vk/ohao_vk_surface.hpp>
#include <vk/ohao_vk_swapchain.hpp>
#include <vk/ohao_vk_instance.hpp>
#include <vk/ohao_vk_shader_module.hpp>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#define OHAO_ENABLE_VALIDATION_LAYER true

namespace ohao{

VulkanContext::VulkanContext(){}

VulkanContext::VulkanContext(GLFWwindow* windowHandle): window(windowHandle){}

VulkanContext::~VulkanContext(){
    cleanup();
}

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

    createDescriptorSetLayout();
    createPipelineLayout();
    createCommandPool();
    createDepthResources();

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

    //pipeline
    createGraphicsPipeline();

    //resources
    createFramebuffers();
    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();
    createCommandBuffers();
    createSyncObjects();

    // Look for light material and apply its properties
    for (const auto& [name, light] : scene->getLights()) {
        updateLight(light.position, light.color, light.intensity);
    }
    updateMaterial(mainObject->material);
    return true;
}

void
VulkanContext::cleanup(){
    vkDestroyImageView(device->getDevice(), depthImageView, nullptr);
    vkDestroyImage(device->getDevice(), depthImage, nullptr);
    vkFreeMemory(device->getDevice(), depthImageMemory, nullptr);

    vkDestroyBuffer(device->getDevice(), indexBuffer, nullptr);
    vkFreeMemory(device->getDevice(), indexBufferMemory, nullptr);

    vkDestroyBuffer(device->getDevice(), vertexBuffer, nullptr);
    vkFreeMemory(device->getDevice(), vertexBufferMemory, nullptr);

    for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++){
        vkDestroyBuffer(device->getDevice(), uniformBuffers[i], nullptr);
        vkFreeMemory(device->getDevice(), uniformBuffersMemory[i], nullptr);
    }
    vkDestroyDescriptorPool(device->getDevice(), descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device->getDevice(), descriptorSetLayout, nullptr);


    for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++){
        vkDestroySemaphore(device->getDevice(), renderFinishedSemaphores[i], nullptr);
        vkDestroySemaphore(device->getDevice(), imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device->getDevice(), inFlightFences[i], nullptr);
    }

    vkDestroyCommandPool(device->getDevice(), commandPool, nullptr);

    for(auto framebuffer : swapChainFrameBuffers){
        vkDestroyFramebuffer(device->getDevice(), framebuffer, nullptr);
    }

    shaderModules.reset();

    if(graphicsPipeline){
        vkDestroyPipeline(device->getDevice(), graphicsPipeline, nullptr);
    }

    if(pipelineLayout){
        vkDestroyPipelineLayout(device->getDevice(), pipelineLayout, nullptr);
    }

    renderPass.reset();
    swapchain.reset();
    device.reset();
    physicalDevice.reset();
    surface.reset();
    instance.reset();

}

void
VulkanContext::createPipelineLayout(){
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;

    if(vkCreatePipelineLayout(device->getDevice(),&pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS){
        throw std::runtime_error("failed to create pipeline layout!");
    }
}

void VulkanContext::createGraphicsPipeline(){
    //load shaders
    if (!shaderModules->createShaderModule("vert", "shaders/shader.vert.spv", OhaoVkShaderModule::ShaderType::VERTEX)
        ||!shaderModules->createShaderModule("frag", "shaders/shader.frag.spv", OhaoVkShaderModule::ShaderType::FRAGMENT)) {
        throw std::runtime_error("graphics pipeline shader modules failed to create!");
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo = shaderModules->getShaderStageInfo("vert");
    VkPipelineShaderStageCreateInfo fragShaderStageInfo = shaderModules->getShaderStageInfo("frag");

    VkPipelineShaderStageCreateInfo shaderStages[]{vertShaderStageInfo, fragShaderStageInfo};

    //vertex input
    auto bindingDescription = Vertex::getBindingDescriptions();
    auto attributeDescription = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescription.size());
    vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescription.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescription.data();

    //Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    //viewport
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchain->getExtent().width);
    viewport.height = static_cast<float>(swapchain->getExtent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain->getExtent();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    //Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    //Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    //color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT; ;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    //graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass->getRenderPass();
    pipelineInfo.subpass = 0;
    pipelineInfo.pDepthStencilState = &depthStencil;

    if(vkCreateGraphicsPipelines(device->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS){
        throw std::runtime_error("failed to create graphics pipeline!");
    }
}

void
VulkanContext::createFramebuffers(){
    swapChainFrameBuffers.resize(swapchain->getImageViews().size());

    for(size_t i = 0; i < swapchain->getImageViews().size(); ++i){
        std::array<VkImageView, 2> attachments = {
                    swapchain->getImageViews().at(i),
                    depthImageView
                };
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass->getRenderPass();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchain->getExtent().width;
        framebufferInfo.height = swapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if(vkCreateFramebuffer(device->getDevice(), &framebufferInfo, nullptr, &swapChainFrameBuffers[i]) != VK_SUCCESS){
            throw std::runtime_error("failed to create framebuffer!");
        }
    }
}

void
VulkanContext::createCommandPool(){
    QueueFamilyIndices queueFamilyIndices = physicalDevice->getQueueFamilyIndices();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if(vkCreateCommandPool(device->getDevice(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS){
        throw std::runtime_error("failed to create command pool!");
    }
}

void
VulkanContext::createCommandBuffers(){
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

    if (vkAllocateCommandBuffers(device->getDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate command buffers!");
    }
}


void
VulkanContext::createSyncObjects(){
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++){
        if(vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS
            || vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS
            || vkCreateFence(device->getDevice(), &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS
        ){
            throw std::runtime_error("failed to create synchronization objects!");
        }
    }
}

void
VulkanContext::drawFrame(){
    vkWaitForFences(device->getDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(device->getDevice(), swapchain->getSwapChain(), UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

    if(result != VK_SUCCESS){
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    vkResetFences(device->getDevice(), 1, &inFlightFences[currentFrame]);
    vkResetCommandBuffer(commandBuffers[currentFrame], 0);
    recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

    updateUniformBuffer(currentFrame);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[]{imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

    VkSemaphore signalSemaphores[]{renderFinishedSemaphores[currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if(vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS){
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

    renderPass->begin(commandBuffer, swapChainFrameBuffers[imageIndex], swapchain->getExtent(),
                    {0.2f, 0.2f, 0.2f, 1.0f},  // clear color
                    1.0f,                       // clear depth
                    0);                          // clear stencil)

    // Bind pipeline and set viewport/scissor
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Bind descriptor sets
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0, 1,
        &descriptorSets[currentFrame],
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
VulkanContext::createDescriptorSetLayout(){
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboLayoutBinding;

    if(vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS){
        throw std::runtime_error("failed to create descriptor set layout!");
    }
}

void
VulkanContext::createUniformBuffers(){
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);

    uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    uniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
    uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);

    for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++){
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if(vkCreateBuffer(device->getDevice(), &bufferInfo, nullptr, &uniformBuffers[i]) != VK_SUCCESS){
            throw std::runtime_error("failed to create uniform buffer!");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device->getDevice(), uniformBuffers[i], &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if(vkAllocateMemory(device->getDevice(), &allocInfo, nullptr, &uniformBuffersMemory[i]) != VK_SUCCESS){
            throw std::runtime_error("failed to allocate uniform buffer memory!");
        }

        vkBindBufferMemory(device->getDevice(), uniformBuffers[i], uniformBuffersMemory[i], 0);
        vkMapMemory(device->getDevice(), uniformBuffersMemory[i], 0, bufferSize, 0, &uniformBuffersMapped[i]);
    }
}
void
VulkanContext::createDescriptorPool(){
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    if(vkCreateDescriptorPool(device->getDevice(), &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS){
        throw std::runtime_error("failed to create descriptor pool!");
    }
}

void
VulkanContext::createDescriptorSets(){
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocInfo.pSetLayouts = layouts.data();

    descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    if(vkAllocateDescriptorSets(device->getDevice(), &allocInfo, descriptorSets.data()) != VK_SUCCESS){
        throw std::runtime_error("failed to allocate descriptor sets!");
    }

    for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++){
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device->getDevice(), 1, &descriptorWrite, 0, nullptr);
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

    memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
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
VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
            VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory){

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(device->getDevice(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS){
        throw std::runtime_error("failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device->getDevice(), buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if(vkAllocateMemory(device->getDevice(), &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS){
        throw std::runtime_error("failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device->getDevice(), buffer, bufferMemory, 0);
}

void
VulkanContext::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size){
    if (commandPool == VK_NULL_HANDLE) {
        throw std::runtime_error("Attempting to copy buffer before command pool creation!");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(device->getDevice(), &allocInfo, &commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate command buffer for copy operation!");
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device->getDevice(), commandPool, 1, &commandBuffer);
}

void
VulkanContext::createVertexBuffer(const std::vector<Vertex>& vertices){
    VkDeviceSize bufferSize = sizeof(Vertex) * vertices.size();

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(device->getDevice(), stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(device->getDevice(), stagingBufferMemory);

    createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertexBuffer, vertexBufferMemory);

    copyBuffer(stagingBuffer, vertexBuffer, bufferSize);

    vkDestroyBuffer(device->getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(device->getDevice(), stagingBufferMemory, nullptr);
}

void
VulkanContext::createIndexBuffer(const std::vector<uint32_t>& indices){
    VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(device->getDevice(), stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(device->getDevice(), stagingBufferMemory);

    createBuffer(bufferSize,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                indexBuffer, indexBufferMemory);

    copyBuffer(stagingBuffer, indexBuffer, bufferSize);

    vkDestroyBuffer(device->getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(device->getDevice(), stagingBufferMemory, nullptr);
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
    VkFormat depthFormat = findDepthFormat();

    createImage(swapchain->getExtent().width, swapchain->getExtent().height, depthFormat,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory, msaaSamples);

    depthImageView = createImageView(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
}

VkFormat
VulkanContext::findDepthFormat(){
    std::vector<VkFormat> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physicalDevice->getDevice(), format, &props);

        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }

    throw std::runtime_error("failed to find supported depth format!");
}

void
VulkanContext::createImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageTiling tiling,
    VkImageUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkImage& image,
    VkDeviceMemory& imageMemory,
    VkSampleCountFlagBits numSampels = VK_SAMPLE_COUNT_1_BIT) {

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = numSampels;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device->getDevice(), &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device->getDevice(), image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device->getDevice(), &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device->getDevice(), image, imageMemory, 0);
}

VkImageView
VulkanContext::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(device->getDevice(), &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture image view!");
    }

    return imageView;
}

}//namespace ohao
