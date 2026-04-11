#include "offscreen_renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "scene/component/mesh_component.hpp"
#include "scene/asset/model.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "scene/scene.hpp"

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

        // Initialize bindless texture manager BEFORE pipeline creation
        // so the forward pipeline layout can include the bindless descriptor set
        m_textureManager = std::make_unique<BindlessTextureManager>();
        if (!m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,
                                           m_graphicsQueueFamily, m_graphicsQueue)) {
            std::cerr << "Failed to initialize BindlessTextureManager (textures unavailable)" << std::endl;
            m_textureManager.reset();
        } else {
            std::cout << "BindlessTextureManager initialized" << std::endl;
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
        // Set texture manager BEFORE init so GBufferPass pipeline includes bindless layout
        if (m_textureManager) {
            // Will be stored and passed to GBufferPass during initialize()
            if (!m_deferredRenderer) {
                m_deferredRenderer = std::make_unique<DeferredRenderer>();
            }
            m_deferredRenderer->setTextureManager(m_textureManager.get());
        }
        if (!initializeDeferredRenderer()) {
            std::cerr << "Failed to initialize deferred renderer (deferred mode unavailable)" << std::endl;
            // Not fatal - forward rendering still works
        }

        // Initialize RT acceleration structure manager
        m_rtAccel = std::make_unique<RTAccelerationStructure>();
        if (m_rtAccel->init(m_device, m_physicalDevice, m_graphicsQueue, m_graphicsQueueFamily, m_commandPool)) {
            std::cout << "Ray tracing: available" << std::endl;

            // Initialize path tracer
            m_pathTracer = std::make_unique<PathTracer>();
            if (m_pathTracer->init(m_device, m_physicalDevice, m_width, m_height)) {
                std::cout << "Path tracer: available" << std::endl;
            } else {
                std::cout << "Path tracer: init failed" << std::endl;
                m_pathTracer.reset();
            }
        } else {
            std::cout << "Ray tracing: not available (continuing without RT)" << std::endl;
            m_rtAccel.reset();
        }

        m_initialized = true;
        std::cout << "OffscreenRenderer initialized: " << m_width << "x" << m_height << std::endl;
        std::cout << "Shadow mapping: " << (m_shadowsEnabled ? "enabled" : "disabled") << std::endl;
        std::cout << "Multi-frame rendering: " << (m_frameResources.isInitialized() ? "enabled" : "disabled") << std::endl;
        std::cout << "Deferred rendering: " << (m_deferredRenderer ? "available" : "unavailable") << std::endl;
        std::cout << "Ray tracing: " << (m_rtAccel ? "enabled" : "disabled") << std::endl;
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

        // Cleanup RT resources BEFORE device destruction
        if (m_pathTracer) {
            m_pathTracer->destroy();
            m_pathTracer.reset();
        }
        if (m_rtAccel) {
            m_rtAccel->destroy();
            m_rtAccel.reset();
        }

        // Cleanup deferred renderer
        if (m_deferredRenderer) {
            m_deferredRenderer->cleanup();
            m_deferredRenderer.reset();
        }

        // Cleanup texture manager
        if (m_textureManager) {
            m_textureManager->cleanup();
            m_textureManager.reset();
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
    if (m_renderMode == RenderMode::PathTraced && m_pathTracer && m_rtAccel) {
        renderPathTraced();
        return;
    }

    if (m_renderMode == RenderMode::Deferred && m_deferredRenderer) {
        renderDeferred();
        return;
    }

    // Forward rendering path
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
    if (mode == RenderMode::PathTraced && (!m_pathTracer || !m_rtAccel)) {
        std::cerr << "Path tracing not available, staying in current mode" << std::endl;
        return;
    }
    m_renderMode = mode;
    const char* names[] = {"Forward", "Deferred", "PathTraced"};
    std::cout << "Render mode set to: " << names[static_cast<int>(mode)] << std::endl;
}

bool OffscreenRenderer::initializeDeferredRenderer() {
    std::cout << "OffscreenRenderer: Creating DeferredRenderer..." << std::endl;
    if (!m_deferredRenderer) {
        m_deferredRenderer = std::make_unique<DeferredRenderer>();
    }
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

    // Pass RT acceleration structure to deferred renderer for RT shadows
    if (m_rtAccel && m_rtAccel->isSupported()) {
        m_deferredRenderer->setAccelerationStructure(m_rtAccel.get());
    }

    // Pass geometry buffers to deferred renderer
    if (m_vertexBuffer != VK_NULL_HANDLE && m_indexBuffer != VK_NULL_HANDLE) {
        m_deferredRenderer->setGeometryBuffers(m_vertexBuffer, m_indexBuffer, &m_meshBufferMap);
    }

    // Update m_lightBuffer with current scene lights (correct numLights, default fallback if empty)
    updateLightBuffer();

    // Sync fallback directional light with sky sun direction so deferred lighting matches the sky
    if (m_lightBufferMapped && m_deferredRenderer) {
        LightUniformBuffer* ld = static_cast<LightUniformBuffer*>(m_lightBufferMapped);
        if (ld && ld->numLights == 1) {
            // No explicit scene lights — align the fallback sun with sky
            glm::vec3 skyLightDir = m_deferredRenderer->getLightDirection(); // points DOWN toward scene
            float nightFactor = m_deferredRenderer->getNightFactor();

            if (glm::length(skyLightDir) > 0.001f) {
                if (nightFactor > 0.01f) {
                    // Blend light direction from sun toward moon at night
                    glm::vec3 moonDir = -m_deferredRenderer->getMoonDirection();
                    glm::vec3 blendedDir = glm::normalize(glm::mix(
                        glm::normalize(skyLightDir), moonDir, nightFactor));
                    ld->lights[0].direction = glm::vec4(blendedDir, 100.0f);

                    // Blend color from warm white (sun) to cool blue (moon)
                    glm::vec3 sunColor  = glm::vec3(1.0f, 0.98f, 0.95f);
                    glm::vec3 moonColor = glm::vec3(0.6f, 0.7f, 0.85f);
                    glm::vec3 blendedColor = glm::mix(sunColor, moonColor, nightFactor);

                    // Blend intensity from full (PI) to ~8% moonlight
                    float sunIntensity  = glm::pi<float>();
                    float moonIntensity = glm::pi<float>() * 0.08f;
                    float blendedIntensity = glm::mix(sunIntensity, moonIntensity, nightFactor);

                    ld->lights[0].color = glm::vec4(blendedColor, blendedIntensity);
                } else {
                    ld->lights[0].direction = glm::vec4(glm::normalize(skyLightDir), 100.0f);
                }
                ld->lights[0].lightSpaceMatrix = calculateLightSpaceMatrix(ld->lights[0]);
            }
            // Ambient: moderate during day, lower at night
            ld->ambientIntensity = glm::mix(0.12f, 0.04f, nightFactor);
        }
    }

    // Pass actual light count to deferred renderer (avoids NaN from zero-init garbage entries)
    if (m_lightBuffer != VK_NULL_HANDLE && m_lightBufferMapped) {
        LightUniformBuffer* lightData = static_cast<LightUniformBuffer*>(m_lightBufferMapped);
        uint32_t lightCount = lightData ? static_cast<uint32_t>(std::max(1, lightData->numLights)) : 1u;
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

    // Copy final output to staging buffer for CPU readback
    copyDeferredOutputToPixelBuffer(cmd);

    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

void OffscreenRenderer::renderPathTraced() {
    if (!m_pathTracer || !m_rtAccel) return;

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    // Read back previous frame's pixels
    if (frame.stagingBufferMapped) {
        memcpy(m_pixelBuffer.data(), frame.stagingBufferMapped, m_width * m_height * 4);
    }

    m_frameResources.resetFrame(m_currentFrame);

    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Use the engine camera's view/projection matrices directly
    float aspect = float(m_width) / float(m_height);
    glm::mat4 ptView = m_camera->getViewMatrix();
    glm::mat4 ptProj = glm::perspectiveRH_ZO(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);

    // Sphere light: position, intensity, color, radius
    m_pathTracer->render(cmd, m_rtAccel.get(), ptView, ptProj,
                         glm::vec3(0.0f, 4.0f, 0.0f), 30.0f,
                         glm::vec3(1.0f, 0.98f, 0.92f), 1.0f);

    // Copy path tracer output to staging buffer for CPU readback
    VkImage ptOutput = m_pathTracer->getOutputImage();
    if (ptOutput != VK_NULL_HANDLE && frame.stagingBuffer != VK_NULL_HANDLE) {
        // Output is already in TRANSFER_SRC_OPTIMAL from render()
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {m_width, m_height, 1};

        vkCmdCopyImageToBuffer(cmd, ptOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               frame.stagingBuffer, 1, &copyRegion);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

void OffscreenRenderer::copyDeferredOutputToPixelBuffer(VkCommandBuffer cmd) {
    if (!m_deferredRenderer) return;

    VkImage finalImage = m_deferredRenderer->getFinalOutputImage();
    if (finalImage == VK_NULL_HANDLE) return;

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    if (frame.stagingBuffer == VK_NULL_HANDLE) return;

    // Transition final output: SHADER_READ_ONLY_OPTIMAL -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier toTransferSrc{};
    toTransferSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.image = finalImage;
    toTransferSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransferSrc.subresourceRange.baseMipLevel = 0;
    toTransferSrc.subresourceRange.levelCount = 1;
    toTransferSrc.subresourceRange.baseArrayLayer = 0;
    toTransferSrc.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransferSrc);

    // Copy image to staging buffer
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

    vkCmdCopyImageToBuffer(cmd, finalImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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

    // Bind this frame's descriptor set (set 0: camera/light/shadow)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);

    // Bind bindless texture descriptor set (set 1) if available
    if (m_textureManager) {
        VkDescriptorSet texSet = m_textureManager->getDescriptorSet();
        if (texSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 1, 1, &texSet, 0, nullptr);
        }
    }

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

        // Bind descriptor sets (set 0: camera/light/shadow)
        vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

        // Bind bindless texture descriptor set (set 1)
        if (m_textureManager) {
            VkDescriptorSet texSet = m_textureManager->getDescriptorSet();
            if (texSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_pipelineLayout, 1, 1, &texSet, 0, nullptr);
            }
        }

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

bool OffscreenRenderer::readTerrainHeights(std::vector<float>& outData, uint32_t& outRes) {
    return false; // Terrain pass disabled
}

bool OffscreenRenderer::readbackHDRBuffers(std::vector<float>& beauty, std::vector<float>& albedo,
                                            std::vector<float>& normal, uint32_t& w, uint32_t& h) {
    if (!m_pathTracer) return false;
    w = m_width; h = m_height;

    auto readbackImage = [&](VkImage image, std::vector<float>& outBuf) -> bool {
        if (image == VK_NULL_HANDLE) return false;
        VkDeviceSize size = m_width * m_height * 4 * sizeof(float);  // RGBA32F

        // Create staging buffer
        VkBuffer staging; VkDeviceMemory stagingMem;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &staging);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, staging, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &stagingMem);
        vkBindBufferMemory(m_device, staging, stagingMem, 0);

        // Command buffer
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = m_commandPool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &cmdInfo, &cmd);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Barrier: GENERAL → TRANSFER_SRC
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

        // Barrier back: TRANSFER_SRC → GENERAL
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);

        // Map and copy
        outBuf.resize(m_width * m_height * 4);
        void* mapped;
        vkMapMemory(m_device, stagingMem, 0, size, 0, &mapped);
        memcpy(outBuf.data(), mapped, size);
        vkUnmapMemory(m_device, stagingMem);

        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMem, nullptr);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        return true;
    };

    vkDeviceWaitIdle(m_device);
    bool ok = readbackImage(m_pathTracer->getAccumImage(), beauty);
    ok &= readbackImage(m_pathTracer->getAlbedoAOV(), albedo);
    ok &= readbackImage(m_pathTracer->getNormalAOV(), normal);
    return ok;
}

} // namespace ohao
