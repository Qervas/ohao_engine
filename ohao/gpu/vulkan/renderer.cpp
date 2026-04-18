#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include "render/rt/denoise/oidn_denoise.hpp"
#include "render/rt/denoise/optix_denoise.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "scene/component/mesh_component.hpp"
#include "animation/animation_component.hpp"
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

            m_rtRealtimeRenderer = std::make_unique<RTRealtimeRenderer>();
            if (!m_rtRealtimeRenderer->init(m_device, m_physicalDevice, m_width, m_height)) {
                std::cout << "RT realtime renderer: init failed" << std::endl;
                m_rtRealtimeRenderer.reset();
            }

            m_rtOfflineRenderer = std::make_unique<RTOfflineRenderer>();
            if (!m_rtOfflineRenderer->init(m_device, m_physicalDevice, m_width, m_height)) {
                std::cout << "RT offline renderer: init failed" << std::endl;
                m_rtOfflineRenderer.reset();
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
        if (m_rtRealtimeRenderer) { m_rtRealtimeRenderer->destroy(); m_rtRealtimeRenderer.reset(); }
        if (m_rtOfflineRenderer) { m_rtOfflineRenderer->destroy(); m_rtOfflineRenderer.reset(); }
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

        // Cleanup env CDF buffers
        if (m_envMarginalCDFBuffer)    { vkDestroyBuffer(m_device, m_envMarginalCDFBuffer, nullptr); m_envMarginalCDFBuffer = VK_NULL_HANDLE; }
        if (m_envMarginalCDFMemory)    { vkFreeMemory(m_device, m_envMarginalCDFMemory, nullptr);    m_envMarginalCDFMemory = VK_NULL_HANDLE; }
        if (m_envConditionalCDFBuffer) { vkDestroyBuffer(m_device, m_envConditionalCDFBuffer, nullptr); m_envConditionalCDFBuffer = VK_NULL_HANDLE; }
        if (m_envConditionalCDFMemory) { vkFreeMemory(m_device, m_envConditionalCDFMemory, nullptr);    m_envConditionalCDFMemory = VK_NULL_HANDLE; }

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
    if (const auto* rtPipeline = getRTPipeline(m_renderMode);
        rtPipeline && getRTRenderer(m_renderMode) && m_rtAccel) {
        renderRTPipeline(*rtPipeline);
        m_denoiseCacheValid = false;
        return;
    }

    if (m_renderMode == RenderMode::Deferred && m_deferredRenderer) {
        renderDeferred();
        m_denoiseCacheValid = false;
        return;
    }

    // Forward rendering path
    if (m_frameResources.isInitialized()) {
        renderMultiFrame();
    } else {
        renderLegacy();
    }
    m_denoiseCacheValid = false;
}

void VulkanRenderer::setRenderMode(RenderMode mode) {
    if (mode == RenderMode::Deferred && !m_deferredRenderer) {
        std::cerr << "Deferred rendering not available, staying in Forward mode" << std::endl;
        return;
    }
    const IRTRenderPipeline* rtPipeline = getRTPipeline(mode);
    if (rtPipeline && (!getRTRenderer(mode) || !m_rtAccel)) {
        std::cerr << "Path tracing not available, staying in current mode" << std::endl;
        return;
    }

    if (rtPipeline) {
        m_rtSettings = rtPipeline->getDefaultSettings();
        applyRTRenderSettings();
    }

    m_renderMode = mode;
    const char* names[] = {"Forward", "Deferred", "RTRealtime", "RTOffline"};
    std::cout << "Render mode set to: " << names[static_cast<int>(mode)] << std::endl;
}

void VulkanRenderer::setRTRenderSettings(const RTRenderSettings& settings) {
    m_rtSettings = settings;
    applyRTRenderSettings();

    const char* profileName = (m_rtSettings.profile == RTRenderProfile::Realtime) ? "Realtime" : "Offline";
    std::cout << "RT profile set to: " << profileName
              << " (maxBounces=" << m_rtSettings.maxBounces
              << ", accumulation=" << (m_rtSettings.preferAccumulation ? "preferred" : "reduced")
              << ", AOVs=" << (m_rtSettings.enableAuxiliaryAOVs ? "on" : "off")
              << ", externalDenoiser=" << (m_rtSettings.allowExternalDenoiser ? "allowed" : "off")
              << ")" << std::endl;
}

void VulkanRenderer::setRTRenderProfile(RTRenderProfile profile) {
    setRTRenderSettings(profile == RTRenderProfile::Realtime ? kRealtimeRTSettings : kOfflineRTSettings);
}

void VulkanRenderer::applyRTRenderSettings() {
    if (auto* renderer = getRTRenderer(m_renderMode)) {
        renderer->setRenderSettings(m_rtSettings);
    }
    // Sync denoiser mode from the active render profile settings,
    // but only when the caller hasn't issued an explicit setDenoiseMode() override.
    if (!m_denoiseModeOverridden && m_denoiseMode != m_rtSettings.denoiseMode) {
        m_denoiseMode = m_rtSettings.denoiseMode;
        m_denoiseCacheValid = false;
    }
}

bool VulkanRenderer::updateAnimatedActorsForRT() {
    bool hasAnimatedActors = false;
    if (!m_scene) return false;

    const float dt = 1.0f / 60.0f;
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto animComp = actor->getComponent<AnimationComponent>();
        if (animComp && animComp->isPlaying()) {
            animComp->update(dt);
            hasAnimatedActors = true;
        }
    }
    return hasAnimatedActors;
}

void VulkanRenderer::prepareRTSceneForFrame(const IRTRenderPipeline& pipeline, bool hasDynamicBLAS) {
    auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer) return;

    // Keep the active RT pipeline authoritative over path tracer behavior.
    m_rtSettings = pipeline.getDefaultSettings();
    applyRTRenderSettings();

    updateLightBuffer();
    if (m_rtLightBuffer != VK_NULL_HANDLE) {
        rtRenderer->setLightBuffer(m_rtLightBuffer, std::max(1u, m_rtLightCount));
    }

    if (hasDynamicBLAS && m_gpuSkinning) {
        VkBuffer skinnedNormalBuf = m_gpuSkinning->getGlobalSkinnedNormalBuffer();
        if (skinnedNormalBuf != VK_NULL_HANDLE && m_rtIndexBuffer != VK_NULL_HANDLE) {
            rtRenderer->setNormalBuffer(skinnedNormalBuf, m_rtIndexBuffer, m_vertexCount);
            return;
        }
    }

    if (m_rtNormalBuffer != VK_NULL_HANDLE && m_rtIndexBuffer != VK_NULL_HANDLE) {
        rtRenderer->setNormalBuffer(m_rtNormalBuffer, m_rtIndexBuffer, m_vertexCount);
    }
}

const IRTRenderPipeline* VulkanRenderer::getRTPipeline(RenderMode mode) const {
    switch (mode) {
    case RenderMode::RTRealtime:
        return &m_rtRealtimePipeline;
    case RenderMode::RTOffline:
        return &m_rtOfflinePipeline;
    default:
        return nullptr;
    }
}

IRTRendererProfile* VulkanRenderer::getRTRenderer(RenderMode mode) {
    switch (mode) {
    case RenderMode::RTRealtime:
        return m_rtRealtimeRenderer.get();
    case RenderMode::RTOffline:
        return m_rtOfflineRenderer.get();
    default:
        return nullptr;
    }
}

const IRTRendererProfile* VulkanRenderer::getRTRenderer(RenderMode mode) const {
    switch (mode) {
    case RenderMode::RTRealtime:
        return m_rtRealtimeRenderer.get();
    case RenderMode::RTOffline:
        return m_rtOfflineRenderer.get();
    default:
        return nullptr;
    }
}

void VulkanRenderer::forEachRTRenderer(const std::function<void(IRTRendererProfile&)>& fn) {
    if (m_rtOfflineRenderer) {
        fn(*m_rtOfflineRenderer);
    }
    if (m_rtRealtimeRenderer) {
        fn(*m_rtRealtimeRenderer);
    }
}

void VulkanRenderer::resetAccumulation() {
    if (auto* renderer = getRTRenderer(m_renderMode)) {
        renderer->resetAccumulation();
    }
}

void VulkanRenderer::notifyCameraChanged() {
    if (auto* renderer = getRTRenderer(m_renderMode)) {
        renderer->notifyViewChanged();
    }
}

uint32_t VulkanRenderer::getPathTracerFrameIndex() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getFrameIndex();
    }
    return 0;
}

VkImageView VulkanRenderer::getMotionVectorAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getMotionVectorAOV();
    }
    return VK_NULL_HANDLE;
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

void VulkanRenderer::setDenoiseMode(DenoiseMode mode) {
    m_denoiseModeOverridden = true;
    if (mode != m_denoiseMode) {
        m_denoiseMode = mode;
        m_denoiseCacheValid = false;
    }
}

const uint8_t* VulkanRenderer::getPixels() const {
    if (m_denoiseMode == DenoiseMode::None) {
        return m_pixelBuffer.data();
    }
    if (m_denoiseCacheValid) {
        return m_denoisedPixelBuffer.data();
    }

    // Shared readback + float3 conversion for any denoiser backend.
    // readbackHDRBuffers is non-const and has device-global side effects
    // (allocates a command buffer, submits it, waits idle). Functionally
    // safe after render() completes in a single-threaded game loop.
    // TODO: make readbackHDRBuffers const by factoring the staging-buffer
    // machinery behind a mutable cache.
    std::vector<float> beautyRGBA, albedoRGBA, normalRGBA;
    uint32_t rw = 0, rh = 0;
    auto* self = const_cast<VulkanRenderer*>(this);
    if (!self->readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {
        std::cerr << "[Denoise] readbackHDRBuffers failed — returning noisy pixels\n";
        return m_pixelBuffer.data();
    }
    auto beauty3 = ohao::rgba32fToFloat3(beautyRGBA.data(), rw, rh);
    auto albedo3 = ohao::rgba32fToFloat3(albedoRGBA.data(), rw, rh);
    auto normal3 = ohao::rgba32fToFloat3(normalRGBA.data(), rw, rh);

    bool denoised = false;

    // Primary denoiser attempt
    if (m_denoiseMode == DenoiseMode::OptiX) {
        denoised = ohao::optixDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                                       rw, rh, /*hdr*/ true);
        if (!denoised) {
            std::cerr << "[Denoise] OptiX unavailable or failed — falling back to OIDN\n";
        }
    }

    // Fallback to OIDN if OptiX failed, or if mode was OIDN to begin with
    if (!denoised) {
        denoised = ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                                      rw, rh, /*hdr*/ true);
        if (!denoised) {
            std::cerr << "[Denoise] OIDN failed — returning noisy pixels\n";
            return m_pixelBuffer.data();
        }
    }

    m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, /*exposure*/ 0.5f);
    m_denoiseCacheValid = true;
    return m_denoisedPixelBuffer.data();
}

} // namespace ohao
