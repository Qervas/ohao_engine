#include "offscreen_renderer_impl.hpp"
#include "renderer/camera/camera.hpp"
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
        std::ifstream test(path + "offscreen_simple.vert.spv");
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

        m_initialized = true;
        std::cout << "OffscreenRenderer initialized: " << m_width << "x" << m_height << std::endl;
        std::cout << "Shadow mapping: " << (m_shadowsEnabled ? "enabled" : "disabled") << std::endl;
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

        // Cleanup sync objects
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

    // Update uniform buffers
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

    // Shadow pass: Render scene from light's perspective to shadow map
    renderShadowPass();

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
        renderSceneObjects();
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
    vkQueueWaitIdle(m_graphicsQueue);

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

        // Cleanup old staging buffer
        if (m_stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        }
        if (m_stagingBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_stagingBufferMemory, nullptr);
        }

        createOffscreenFramebuffer();
    }
}

} // namespace ohao
