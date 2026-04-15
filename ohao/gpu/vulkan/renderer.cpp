#include "renderer_impl.hpp"
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

VulkanRenderer::VulkanRenderer(uint32_t width, uint32_t height)
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

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::initialize() {
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
        // Initialize GPU skinning for animated BLAS
        m_gpuSkinning = std::make_unique<GPUSkinning>();
        if (!m_gpuSkinning->initialize(m_device, m_physicalDevice, m_commandPool, m_graphicsQueue)) {
            std::cerr << "GPUSkinning init failed (non-fatal)" << std::endl;
            m_gpuSkinning.reset();
        }
        // Initialize animated RT manager (dependencies set later during scene setup)
        m_animatedRT = std::make_unique<AnimatedRTManager>();

        std::cout << "VulkanRenderer initialized: " << m_width << "x" << m_height << std::endl;
        std::cout << "Shadow mapping: " << (m_shadowsEnabled ? "enabled" : "disabled") << std::endl;
        std::cout << "Multi-frame rendering: " << (m_frameResources.isInitialized() ? "enabled" : "disabled") << std::endl;
        std::cout << "Deferred rendering: " << (m_deferredRenderer ? "available" : "unavailable") << std::endl;
        std::cout << "Ray tracing: " << (m_rtAccel ? "enabled" : "disabled") << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "VulkanRenderer initialization failed: " << e.what() << std::endl;
        return false;
    }
}

void VulkanRenderer::shutdown() {
    if (!m_initialized) return;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        // Cleanup RT resources BEFORE device destruction
        if (m_animatedRT) {
            m_animatedRT->cleanup();
            m_animatedRT.reset();
        }
        if (m_gpuSkinning) {
            m_gpuSkinning->cleanup();
            m_gpuSkinning.reset();
        }
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

void VulkanRenderer::setScene(Scene* scene) {
    m_scene = scene;
    // Update scene buffers when scene is set
    if (m_initialized && m_scene) {
        updateSceneBuffers();
    }
}

void VulkanRenderer::updatePhysics(float deltaTime) {
    if (m_scene) {
        m_scene->updatePhysics(deltaTime);
    }
}

void VulkanRenderer::render() {
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

void VulkanRenderer::setRenderMode(RenderMode mode) {
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

bool VulkanRenderer::initializeDeferredRenderer() {
    std::cout << "VulkanRenderer: Creating DeferredRenderer..." << std::endl;
    if (!m_deferredRenderer) {
        m_deferredRenderer = std::make_unique<DeferredRenderer>();
    }
    std::cout << "VulkanRenderer: Calling DeferredRenderer::initialize()..." << std::endl;
    if (!m_deferredRenderer->initialize(m_device, m_physicalDevice)) {
        std::cerr << "VulkanRenderer: DeferredRenderer::initialize() failed!" << std::endl;
        m_deferredRenderer.reset();
        return false;
    }
    std::cout << "VulkanRenderer: DeferredRenderer initialized, resizing to " << m_width << "x" << m_height << std::endl;
    m_deferredRenderer->onResize(m_width, m_height);
    if (m_deferredRenderer->getPostProcessing())
        m_deferredRenderer->getPostProcessing()->setExposure(0.6f);
    return true;
}


} // namespace ohao
