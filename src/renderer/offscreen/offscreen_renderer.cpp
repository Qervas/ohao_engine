#include "offscreen_renderer_impl.hpp"
#include "renderer/camera/camera.hpp"
#include "renderer/passes/deferred_renderer.hpp"
#include "engine/scene/scene.hpp"

namespace ohao {

// Helper function implementation
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

OffscreenRenderer::OffscreenRenderer(uint32_t width, uint32_t height)
    : m_width(width), m_height(height)
{
    m_pixelBuffer.resize(width * height * 4); // RGBA
    m_camera = std::make_unique<Camera>();
    m_camera->setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
    m_camera->setRotation(0.0f, -90.0f);

    // Try to find shader base path
    std::vector<std::string> searchPaths = {
        "bin/shaders/",
        "build/shaders/",
        "../build/shaders/",
        "../../build/shaders/",
        "../../../build/shaders/",
        "shaders/",
        "../shaders/"
    };

    for (const auto& path : searchPaths) {
        std::ifstream test(path + "core_forward.vert.spv");
        if (test.good()) {
            m_shaderBasePath = path;
            break;
        }
    }
}

OffscreenRenderer::~OffscreenRenderer() {
    shutdown();
}

bool OffscreenRenderer::initialize() {
    if (m_initialized) return true;

    try {
        if (!createInstance()) {
            std::cerr << "Failed to create Vulkan instance" << std::endl;
            return false;
        }
        if (!pickPhysicalDevice()) {
            std::cerr << "Failed to pick physical device" << std::endl;
            return false;
        }
        if (!createLogicalDevice()) {
            std::cerr << "Failed to create logical device" << std::endl;
            return false;
        }
        if (!createCommandPool()) {
            std::cerr << "Failed to create command pool" << std::endl;
            return false;
        }
        if (!createRenderPass()) {
            std::cerr << "Failed to create render pass" << std::endl;
            return false;
        }
        if (!createOffscreenFramebuffer()) {
            std::cerr << "Failed to create offscreen framebuffer" << std::endl;
            return false;
        }

        // Shadow mapping setup (must be before descriptor sets)
        if (!createShadowRenderPass()) {
            std::cerr << "Failed to create shadow render pass" << std::endl;
            return false;
        }
        if (!createShadowResources()) {
            std::cerr << "Failed to create shadow resources" << std::endl;
            return false;
        }

        if (!createDescriptorSetLayout()) {
            std::cerr << "Failed to create descriptor set layout" << std::endl;
            return false;
        }
        if (!createPipeline()) {
            std::cerr << "Failed to create graphics pipeline" << std::endl;
            return false;
        }

        // Shadow pipeline (after main pipeline)
        if (!createShadowPipeline()) {
            std::cerr << "Failed to create shadow pipeline" << std::endl;
            return false;
        }

        if (!createUniformBuffer()) {
            std::cerr << "Failed to create uniform buffer" << std::endl;
            return false;
        }
        if (!createLightBuffer()) {
            std::cerr << "Failed to create light buffer" << std::endl;
            return false;
        }
        if (!createDescriptorPool()) {
            std::cerr << "Failed to create descriptor pool" << std::endl;
            return false;
        }
        if (!createDescriptorSets()) {
            std::cerr << "Failed to create descriptor sets" << std::endl;
            return false;
        }
        if (!createVertexBuffer()) {
            std::cerr << "Failed to create vertex buffer" << std::endl;
            return false;
        }
        if (!createSyncObjects()) {
            std::cerr << "Failed to create sync objects" << std::endl;
            return false;
        }

        // Initialize multi-frame resources for pipelined rendering
        if (!initializeFrameResources()) {
            std::cerr << "Failed to initialize frame resources (continuing with legacy mode)" << std::endl;
            // Not a fatal error - we can still use single-frame rendering
        }

        // Set shader base path for all render passes
        RenderPassBase::setShaderBasePath(m_shaderBasePath);

        // Initialize deferred renderer (AAA quality rendering)
        if (!initializeDeferredRenderer()) {
            std::cerr << "Failed to initialize deferred renderer (deferred mode unavailable)" << std::endl;
            // Not fatal - forward rendering still works
        }

        m_initialized = true;
        std::cout << "OffscreenRenderer initialized: " << m_width << "x" << m_height << std::endl;
        std::cout << "Shadow mapping: " << (m_shadowsEnabled ? "enabled" : "disabled") << std::endl;
        std::cout << "Multi-frame rendering: " << (m_frameResources.isInitialized() ? "enabled" : "disabled") << std::endl;
        std::cout << "Deferred rendering: " << (m_deferredRenderer ? "available" : "unavailable") << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "OffscreenRenderer initialization failed: " << e.what() << std::endl;
        return false;
    }
}

void OffscreenRenderer::shutdown() {
    if (!m_initialized) return;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        // Cleanup deferred renderer
        if (m_deferredRenderer) {
            m_deferredRenderer->cleanup();
            m_deferredRenderer.reset();
        }

        // Cleanup multi-frame resources
        m_frameResources.shutdown();

        // Cleanup legacy sync objects
        if (m_renderFence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, m_renderFence, nullptr);
        }

        // Cleanup vertex buffer
        if (m_vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        }
        if (m_vertexBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
        }

        // Cleanup index buffer
        if (m_indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        }
        if (m_indexBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
        }

        // Cleanup uniform buffer
        if (m_uniformBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_uniformBuffer, nullptr);
        }
        if (m_uniformBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_uniformBufferMemory, nullptr);
        }

        // Cleanup light buffer
        if (m_lightBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_lightBuffer, nullptr);
        }
        if (m_lightBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_lightBufferMemory, nullptr);
        }

        // Cleanup descriptor pool
        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        }

        // Cleanup descriptor set layout
        if (m_descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        }

        // Cleanup pipeline
        if (m_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        }

        // Cleanup shaders
        if (m_vertShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_vertShaderModule, nullptr);
        }
        if (m_fragShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_fragShaderModule, nullptr);
        }

        // Cleanup shadow resources
        cleanupShadowResources();

        // Cleanup staging buffer
        if (m_stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        }
        if (m_stagingBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_stagingBufferMemory, nullptr);
        }

        // Cleanup framebuffer
        cleanupFramebuffer();

        // Cleanup render pass
        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        }

        // Cleanup command pool
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }

        // Cleanup device
        vkDestroyDevice(m_device, nullptr);
    }

    // Cleanup instance
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }

    m_initialized = false;
}

void OffscreenRenderer::setScene(Scene* scene) {
    m_scene = scene;
    // Update scene buffers when scene is set
    if (m_initialized && m_scene) {
        updateSceneBuffers();
    }
}

void OffscreenRenderer::updatePhysics(float deltaTime) {
    if (m_scene) {
        m_scene->updatePhysics(deltaTime);
    }
}

void OffscreenRenderer::render() {
    if (!m_initialized) return;

    // Check render mode
    if (m_renderMode == RenderMode::Deferred && m_deferredRenderer) {
        renderDeferred();
        return;
    }

    // Forward rendering path
    // Use multi-frame rendering if available
    if (m_frameResources.isInitialized()) {
        renderMultiFrame();
    } else {
        renderLegacy();
    }
}

void OffscreenRenderer::setRenderMode(RenderMode mode) {
    if (mode == RenderMode::Deferred && !m_deferredRenderer) {
        std::cerr << "Deferred rendering not available, staying in Forward mode" << std::endl;
        return;
    }
    m_renderMode = mode;
    std::cout << "Render mode set to: " << (mode == RenderMode::Deferred ? "Deferred" : "Forward") << std::endl;
}

bool OffscreenRenderer::initializeDeferredRenderer() {
    std::cout << "OffscreenRenderer: Creating DeferredRenderer..." << std::endl;
    m_deferredRenderer = std::make_unique<DeferredRenderer>();
    std::cout << "OffscreenRenderer: Calling DeferredRenderer::initialize()..." << std::endl;
    if (!m_deferredRenderer->initialize(m_device, m_physicalDevice)) {
        std::cerr << "OffscreenRenderer: DeferredRenderer::initialize() failed!" << std::endl;
        m_deferredRenderer.reset();
        return false;
    }
    std::cout << "OffscreenRenderer: DeferredRenderer initialized, resizing to " << m_width << "x" << m_height << std::endl;
    m_deferredRenderer->onResize(m_width, m_height);
    return true;
}

void OffscreenRenderer::renderDeferred() {
    if (!m_deferredRenderer) return;

    // Get command buffer
    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    // Read pixels from THIS frame's staging buffer AFTER waiting
    // This data was written 3 frames ago and is now guaranteed complete
    if (frame.stagingBufferMapped) {
        memcpy(m_pixelBuffer.data(), frame.stagingBufferMapped, m_width * m_height * 4);
    }

    m_frameResources.resetFrame(m_currentFrame);

    // Update camera data
    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();
    glm::vec3 camPos = m_camera->getPosition();

    m_deferredRenderer->setScene(m_scene);
    m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);

    // Pass geometry buffers to deferred renderer
    if (m_vertexBuffer != VK_NULL_HANDLE && m_indexBuffer != VK_NULL_HANDLE) {
        m_deferredRenderer->setGeometryBuffers(m_vertexBuffer, m_indexBuffer, &m_meshBufferMap);
    }

    // Set light buffer if we have lights
    if (m_lightBuffer != VK_NULL_HANDLE) {
        // Count active lights from scene
        uint32_t lightCount = 0;
        if (m_scene) {
            // TODO: Get actual light count from scene
            lightCount = MAX_LIGHTS;
        }
        m_deferredRenderer->setLightBuffer(m_lightBuffer, lightCount);
    }

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);

    // Execute deferred rendering pipeline
    m_deferredRenderer->render(cmd, m_currentFrame);

    // Copy deferred output to staging buffer for pixel readback
    copyDeferredOutputToPixelBuffer();

    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

void OffscreenRenderer::copyDeferredOutputToPixelBuffer() {
    // For now, fall back to using forward renderer's output (m_colorImage)
    // The deferred pipeline writes to its own buffers which need proper integration
    // TODO: Properly integrate deferred output once the pipeline is fully tested

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    VkCommandBuffer cmd = frame.commandBuffer;

    // Copy from the forward renderer's color image (which is in a known good state)
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_width, m_height, 1};

    vkCmdCopyImageToBuffer(cmd, m_colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.stagingBuffer, 1, &region);
}

void OffscreenRenderer::renderMultiFrame() {
    // Get current frame resources
    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);

    // Wait for this frame's previous work to complete
    // This only stalls if we've gone around the ring buffer (3 frames ago)
    m_frameResources.waitForFrame(m_currentFrame);

    // Read pixels from THIS frame's staging buffer AFTER waiting
    // This data was written 3 frames ago and is now guaranteed complete
    if (frame.stagingBufferMapped) {
        memcpy(m_pixelBuffer.data(), frame.stagingBufferMapped, m_width * m_height * 4);
    }

    // Reset fence for new work
    m_frameResources.resetFrame(m_currentFrame);

    // Update uniform buffers for this frame (no stall - using per-frame buffers)
    updateUniformBuffer(m_currentFrame);
    updateLightBuffer(m_currentFrame);

    // Record command buffer
    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult beginResult = vkBeginCommandBuffer(cmd, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        std::cerr << "[MultiFrame] vkBeginCommandBuffer failed with result: " << beginResult << std::endl;
        return;
    }

    // Shadow pass: Render scene from light's perspective (use per-frame descriptor set!)
    renderShadowPass(cmd, frame.descriptorSet);

    // Main render pass
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_width, m_height};

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.1f, 0.1f, 0.12f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Bind this frame's descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);

    if (m_hasSceneMeshes) {
        // Bind pipeline and vertex/index buffers
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_width);
        viewport.height = static_cast<float>(m_height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {m_width, m_height};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, offsets);

        if (m_indexBuffer != VK_NULL_HANDLE && m_indexCount > 0) {
            vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        // Render scene objects
        renderSceneObjects(cmd);
    }

    vkCmdEndRenderPass(cmd);

    // Copy framebuffer to this frame's staging buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_width, m_height, 1};

    vkCmdCopyImageToBuffer(cmd, m_colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.stagingBuffer, 1, &region);

    vkEndCommandBuffer(cmd);

    // Submit work with fence signaling (no blocking wait!)
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    // Advance to next frame in ring buffer
    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

void OffscreenRenderer::renderLegacy() {
    // Legacy single-frame rendering (kept for compatibility)
    updateUniformBuffer();
    updateLightBuffer();

    // Wait for previous frame
    vkWaitForFences(m_device, 1, &m_renderFence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_renderFence);

    // Record command buffer
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

    // Shadow pass: Render scene from light's perspective to shadow map (use legacy descriptor set)
    renderShadowPass(m_commandBuffer, m_descriptorSet);

    // Main render pass
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_width, m_height};

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.1f, 0.1f, 0.12f, 1.0f}};  // Dark background
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Render scene objects if we have them
    if (m_hasSceneMeshes) {
        // Bind pipeline
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        // Set viewport
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_width);
        viewport.height = static_cast<float>(m_height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);

        // Set scissor
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {m_width, m_height};
        vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);

        // Bind descriptor sets
        vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

        // Bind vertex and index buffers
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, &m_vertexBuffer, offsets);
        if (m_indexBuffer != VK_NULL_HANDLE && m_indexCount > 0) {
            vkCmdBindIndexBuffer(m_commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        renderSceneObjects(m_commandBuffer);
    }
    // If no scene meshes, just show the clear color (empty viewport)

    vkCmdEndRenderPass(m_commandBuffer);

    // Copy framebuffer to staging buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_width, m_height, 1};

    vkCmdCopyImageToBuffer(m_commandBuffer, m_colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &region);

    vkEndCommandBuffer(m_commandBuffer);

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_renderFence);

    // Wait for completion before reading pixels (legacy blocking behavior)
    vkWaitForFences(m_device, 1, &m_renderFence, VK_TRUE, UINT64_MAX);

    // Copy pixels to CPU buffer
    void* data;
    vkMapMemory(m_device, m_stagingBufferMemory, 0, m_width * m_height * 4, 0, &data);
    memcpy(m_pixelBuffer.data(), data, m_width * m_height * 4);
    vkUnmapMemory(m_device, m_stagingBufferMemory);
}

void OffscreenRenderer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    m_pixelBuffer.resize(width * height * 4);

    if (m_initialized) {
        vkDeviceWaitIdle(m_device);
        cleanupFramebuffer();

        // Cleanup old legacy staging buffer
        if (m_stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
            m_stagingBuffer = VK_NULL_HANDLE;
        }
        if (m_stagingBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_stagingBufferMemory, nullptr);
            m_stagingBufferMemory = VK_NULL_HANDLE;
        }

        createOffscreenFramebuffer();

        // Resize multi-frame staging buffers (critical for correct pixel readback!)
        if (m_frameResources.isInitialized()) {
            m_frameResources.resizeStagingBuffers(width * height * 4);
            // Reset frame index to avoid reading from old-size staging buffers
            m_currentFrame = 0;
        }

        // Resize deferred renderer
        if (m_deferredRenderer) {
            m_deferredRenderer->onResize(width, height);
        }
    }
}

} // namespace ohao
