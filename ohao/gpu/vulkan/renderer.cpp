#include "renderer_impl.hpp"
#include <algorithm>
#include <cstring>
#include "render/camera/camera.hpp"
#include "render/rt/denoise/oidn_denoise.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "render/deferred/deferred_renderer.hpp"
#include "render/scene/render_scene.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "core/log.hpp"

namespace ohao {

namespace {
// IEEE 754 binary16 → binary32 (for R16F AOV readback).
inline float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            exp = 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 0x1f) {
        f = (sign << 31) | (0xff << 23) | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}
}  // namespace

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
    m_camera = std::make_unique<Camera>();
    m_camera->setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
    m_camera->setRotation(0.0f, -90.0f);
    // The camera's aspect is the target's from the start: a window opened at its final
    // size never resizes, and resize() is the only other place this is set.
    if (width > 0 && height > 0)
        m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));

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
        std::ifstream test(path + "core_deferred_lighting.frag.spv");
        if (test.good()) {
            m_shaderBasePath = path;
            break;
        }
    }
}

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::initialize(const PresentTarget& target) {
    m_presentTarget = target;
    return initialize();
}

bool VulkanRenderer::initialize() {
    if (m_initialized) return true;

    try {
        if (!createInstance()) {
            OHAO_LOG(Error, gpu, "Failed to create Vulkan instance");
            return false;
        }
        if (m_presentTarget) {
            m_surface = m_presentTarget->createSurface ? m_presentTarget->createSurface(m_instance)
                                                       : VK_NULL_HANDLE;
            if (m_surface == VK_NULL_HANDLE) {
                OHAO_LOG(Error, gpu, "Failed to create window surface");
                return false;
            }
        }
        if (!pickPhysicalDevice()) {
            OHAO_LOG(Error, gpu, "Failed to pick physical device");
            return false;
        }
        if (!createLogicalDevice()) {
            OHAO_LOG(Error, gpu, "Failed to create logical device");
            return false;
        }
        if (!createCommandPool()) {
            OHAO_LOG(Error, gpu, "Failed to create command pool");
            return false;
        }

// Bindless texture manager: the deferred G-buffer pass binds its set
        m_textureManager = std::make_unique<BindlessTextureManager>();
        if (!m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,
                                           m_graphicsQueueFamily, m_graphicsQueue)) {
            OHAO_LOG(Warn, gpu, "Failed to initialize BindlessTextureManager (textures unavailable)");
            m_textureManager.reset();
        } else {
            OHAO_LOG(Debug, gpu, "BindlessTextureManager initialized");
        }

if (!createLightBuffer()) {
            OHAO_LOG(Error, gpu, "Failed to create light buffer");
            return false;
        }

        // Initialize multi-frame resources for pipelined rendering
        if (!initializeFrameResources()) {
            OHAO_LOG(Error, gpu, "Failed to initialize frame resources");
            return false;
        }
        // Per-pass GPU timing: one query pool per frame slot. Without timestamps the
        // renderer still renders; it just reports no timings.
        (void)m_passTimer.init(m_device, m_physicalDevice, m_graphicsQueueFamily, MAX_FRAMES_IN_FLIGHT);

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
            OHAO_LOG(Error, gpu, "Failed to initialize deferred renderer");
            return false;
        }

        // Persistent GPU render world: needs the device and command pool. It owns the
        // acceleration structures (one BLAS per mesh, the TLAS) when the device has
        // ray tracing; it decides that itself. The realtime/offline PathTracers are
        // created lazily on setRenderMode() (eager dual init at 4K can OOM).
        m_renderScene = std::make_unique<render::RenderScene>();
        if (!m_renderScene->initialize(render::RenderScene::InitInfo{
                m_instance, m_physicalDevice, m_device, m_graphicsQueue, m_graphicsQueueFamily,
                m_commandPool, /*rayTracing=*/true, m_rayQueryEnabled})) {
            OHAO_LOG(Error, gpu, "Failed to initialize RenderScene");
            m_renderScene.reset();
            return false;
        }

        if (m_presentTarget && !createPresentation()) {
            OHAO_LOG(Error, gpu, "Failed to create swapchain");
            return false;
        }

        m_initialized = true;

        OHAO_LOG(Info, gpu, "VulkanRenderer initialized: {}x{}", m_width, m_height);
        OHAO_LOG(Info, gpu, "Ray tracing: {}", isRTSupported() ? "enabled" : "disabled");
        return true;

    } catch (const std::exception& e) {
        OHAO_LOG(Error, gpu, "VulkanRenderer initialization failed: {}", e.what());
        return false;
    }
}

void VulkanRenderer::shutdown() {
    if (!m_initialized) return;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        // The render world's GPU resources (and its acceleration structures) go
        // first. The object itself stays, inert, until the renderer is destroyed:
        // a World's render extract may still hold a reference and release its
        // slots into it after an explicit shutdown().
        if (m_renderScene) m_renderScene->shutdown();

        // Cleanup RT resources BEFORE device destruction
        if (m_rtRealtimeRenderer) { m_rtRealtimeRenderer->destroy(); m_rtRealtimeRenderer.reset(); }
        if (m_rtOfflineRenderer) { m_rtOfflineRenderer->destroy(); m_rtOfflineRenderer.reset(); }

        // The env map's view is owned by the texture manager: release it first.
        destroyEnvMapImage();
        destroyEnvCDFBuffers();
        if (m_envSampler) { vkDestroySampler(m_device, m_envSampler, nullptr); m_envSampler = VK_NULL_HANDLE; }

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

        // Presentation: semaphores and swapchain belong to the device.
        vkDeviceWaitIdle(m_device);
        for (auto& sem : m_imageAcquired) {
            if (sem != VK_NULL_HANDLE) vkDestroySemaphore(m_device, sem, nullptr);
            sem = VK_NULL_HANDLE;
        }
        m_swapchain.reset();

        // Cleanup multi-frame resources
        m_passTimer.destroy();
        m_frameResources.shutdown();

        destroyLightBuffers();

        // Cleanup command pool
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }

        // Cleanup device
        vkDestroyDevice(m_device, nullptr);
    }

    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }

    // Cleanup instance
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }

    m_initialized = false;
}

void VulkanRenderer::render() {
    if (!m_initialized) return;

    // The environment map (and the dummy CDF buffers without one), once per path. A
    // new HDRI joins the bindless table, so frames in flight must finish first.
    if (!m_envMapChecked || m_envMapPath != m_envMapAttemptedPath) {
        if (!m_envMapPath.empty() && m_envMapPath != m_envMapLoadedPath) vkDeviceWaitIdle(m_device);
        m_envMapChecked = true;
        m_envMapAttemptedPath = m_envMapPath;
        uploadEnvironmentMap();
    }

    // Check render mode
    if (const auto* rtPipeline = getRTPipeline(m_renderMode);
        rtPipeline && getRTRenderer(m_renderMode) && isRTSupported()) {
        renderRTPipeline(*rtPipeline);
        m_denoiseCacheValid = false;
        return;
    }

    renderDeferred();
    m_denoiseCacheValid = false;
}

bool VulkanRenderer::isRTSupported() const {
    return m_renderScene && m_renderScene->rtSupported();
}

bool VulkanRenderer::ensureRTRenderer(RenderMode mode) {
    if (!isRTSupported()) {
        return false;
    }
    if (getRTRenderer(mode)) {
        return true;
    }

    const auto initProfile = [&](auto& slot, const char* label) -> bool {
        slot = std::make_unique<std::remove_reference_t<decltype(*slot)>>();
        // Binding 12 covers the whole texture table: materials store its slot indices.
        slot->setBindlessCapacity(m_textureManager ? m_textureManager->getMaxTextures()
                                                   : PathTracer::kDefaultBindlessCapacity);
        if (!slot->init(m_device, m_physicalDevice, m_width, m_height,
                        m_instance, m_graphicsQueueFamily,
                        m_enabledInstanceExtensions, m_enabledDeviceExtensions)) {
            OHAO_LOG(Error, gpu, "RT {} renderer: init failed", label);
            slot.reset();
            return false;
        }
        slot->setRenderSeed(m_renderSeed);   // a seed set before this renderer existed
        OHAO_LOG(Info, gpu, "RT {} renderer: ready", label);
        return true;
    };

    bool created = false;
    switch (mode) {
    case RenderMode::RTRealtime:
        created = initProfile(m_rtRealtimeRenderer, "realtime");
        break;
    case RenderMode::RTOffline:
        created = initProfile(m_rtOfflineRenderer, "offline");
        break;
    default:
        return false;
    }

    // Scene buffers, textures and lights are bound every RT frame from the
    // RenderScene (prepareRTSceneForFrame / renderRTPipeline): nothing to re-upload.
    return created;
}

void VulkanRenderer::setRenderMode(RenderMode mode) {
    const IRTRenderPipeline* rtPipeline = getRTPipeline(mode);
    if (rtPipeline) {
        if (!ensureRTRenderer(mode)) {
            OHAO_LOG(Warn, gpu, "Path tracing not available, staying in current mode");
            return;
        }
        m_rtSettings = rtPipeline->getDefaultSettings();
        applyRTRenderSettings();
    }

    m_renderMode = mode;
    const char* names[] = {"Deferred", "RTRealtime", "RTOffline"};
    OHAO_LOG(Info, gpu, "Render mode set to: {}", names[static_cast<int>(mode)]);
}

void VulkanRenderer::setRTRenderSettings(const RTRenderSettings& settings) {
    m_rtSettings = settings;
    applyRTRenderSettings();

    const char* profileName = (m_rtSettings.profile == RTRenderProfile::Realtime) ? "Realtime" : "Offline";
    OHAO_LOG(Info, gpu,
             "RT profile set to: {} (maxBounces={}, accumulation={}, AOVs={}, externalDenoiser={})",
             profileName, m_rtSettings.maxBounces,
             m_rtSettings.preferAccumulation ? "preferred" : "reduced",
             m_rtSettings.enableAuxiliaryAOVs ? "on" : "off",
             m_rtSettings.allowExternalDenoiser ? "allowed" : "off");
}

void VulkanRenderer::setRTRenderProfile(RTRenderProfile profile) {
    setRTRenderSettings(profile == RTRenderProfile::Realtime ? kRealtimeRTSettings : kOfflineRTSettings);
}

void VulkanRenderer::applyRTRenderSettings() {
    // Atrous, DLSS-RR and NRD are GPU-side denoisers the PathTracer dispatches
    // itself, so it must see their mode to run them (NRD: the chain that writes
    // the binding-30 image the NRD readback copies). The profile default (reset
    // each frame) doesn't carry the user's --denoise override, so inject it here.
    // None and OIDN (CPU, in readback) keep the profile default.
    if (m_denoiseModeOverridden &&
        (m_denoiseMode == DenoiseMode::Atrous || m_denoiseMode == DenoiseMode::DLSSRR ||
         m_denoiseMode == DenoiseMode::NRD)) {
        m_rtSettings.denoiseMode = m_denoiseMode;
    }
    // Re-inject the realtime per-frame sample count: prepareRTSceneForFrame
    // resets m_rtSettings to the pipeline default (samplesPerFrame=1) each frame,
    // so restore the user's choice here (same pattern as aniso/SSS preservation).
    m_rtSettings.samplesPerFrame = m_realtimeSamplesPerFrame;
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

void VulkanRenderer::prepareRTSceneForFrame(const IRTRenderPipeline& pipeline) {
    auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer) return;

    // Keep the active RT pipeline authoritative over path tracer behavior.
    // Preserve user-set anisotropy overrides (4.K) across the per-frame reset —
    // pipeline defaults always have aniso=0, so a non-zero strength means the
    // caller explicitly opted in via setRTRenderSettings.
    float preservedAnisoStrength = m_rtSettings.anisotropyStrength;
    float preservedAnisoRotation = m_rtSettings.anisotropyRotation;
    float preservedSssStrength   = m_rtSettings.subsurfaceStrength;  // 4.L
    m_rtSettings = pipeline.getDefaultSettings();
    m_rtSettings.anisotropyStrength = preservedAnisoStrength;
    m_rtSettings.anisotropyRotation = preservedAnisoRotation;
    m_rtSettings.subsurfaceStrength = preservedSssStrength;
    applyRTRenderSettings();

    // No rebuilds: the RenderScene's tables and TLAS are current once recordUpdates
    // ran (renderRTPipeline binds them after it). Textures, env map and env scale:
    bindRTTextures(*rtRenderer);
    rtRenderer->setEnvCDFBuffers(m_envMarginalCDFBuffer, m_envConditionalCDFBuffer,
                                 envMapLoaded() ? m_envMapWidth : 0u, envMapLoaded() ? m_envMapHeight : 0u,
                                 envMapLoaded() ? m_envMapIntegral : 0.0f);
    rtRenderer->setEnvMapResource(envMapLoaded() ? m_envMapImageView : VK_NULL_HANDLE,
                                  envMapLoaded() ? m_envSampler : VK_NULL_HANDLE);
    m_renderScene->setEnvIntensity(m_envIntensityScale);
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
    if (m_renderMode == RenderMode::Deferred) {
        // The deferred path accumulates too (RT GI and TAA history, a clock, the frame
        // slot that indexes TAA's jitter). Idle, then start over as a fresh renderer does,
        // the way resize() restarts the slots.
        if (!m_deferredRenderer || !m_initialized) return;
        vkDeviceWaitIdle(m_device);
        // Every slot's frame has finished: read their timings before the slots restart, or
        // an older frame would enter the ring after a newer one.
        m_passTimer.collectAll([](uint32_t) { return true; });
        m_currentFrame = 0;
        m_deferredRenderer->resetTemporalState();
        return;
    }
    if (auto* renderer = getRTRenderer(m_renderMode)) {
        renderer->resetAccumulation();
    }
}

void VulkanRenderer::setRenderSeed(uint32_t seed) {
    m_renderSeed = seed;
    forEachRTRenderer([&](IRTRendererProfile& r) { r.setRenderSeed(seed); });
    if (m_deferredRenderer) {
        m_deferredRenderer->setRenderSeed(seed);   // RT shadows and GI
        if (auto* pp = m_deferredRenderer->getPostProcessing()) pp->setNoiseSeed(seed);
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

VkImageView VulkanRenderer::getDepthAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getDepthAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getRoughnessAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getRoughnessAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getDiffuseRadianceAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getDiffuseRadianceAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getSpecularRadianceAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getSpecularRadianceAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getDiffAlbedoAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getDiffAlbedoAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getSpecColorAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getSpecColorAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getNormalRoughnessAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getNormalRoughnessAOV();
    }
    return VK_NULL_HANDLE;
}

bool VulkanRenderer::createPresentation() {
    // The deferred final image must be a valid blit source: R8G8B8A8_UNORM from
    // post-processing, or R16G16B16A16_SFLOAT from lighting when post is skipped.
    for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
            OHAO_LOG(Error, gpu, "present: deferred output format {} cannot be a blit source",
                     static_cast<int>(f));
            return false;
        }
    }
    m_swapchain = std::make_unique<Swapchain>();
    if (!m_swapchain->create(m_physicalDevice, m_device, m_surface, m_presentTarget->extent,
                             m_presentTarget->vsync)) {
        return false;
    }
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& sem : m_imageAcquired)
        if (vkCreateSemaphore(m_device, &si, nullptr, &sem) != VK_SUCCESS) return false;
    OHAO_LOG(Info, gpu, "present: swapchain {}x{}, {} images", m_swapchain->extent().width,
             m_swapchain->extent().height, m_swapchain->imageCount());
    return true;
}

bool VulkanRenderer::recreatePresentation() {
    const VkExtent2D want = m_presentTarget->extent;
    if (want.width == 0 || want.height == 0) return false;   // minimized
    if (!m_swapchain->recreate(want)) return false;
    m_swapchainDirty = false;
    resize(m_swapchain->extent().width, m_swapchain->extent().height);
    return true;
}

void VulkanRenderer::setPresentExtent(VkExtent2D extent) {
    if (!m_presentTarget) return;
    m_presentTarget->extent = extent;
    m_swapchainDirty = true;
}

float VulkanRenderer::gpuFrameMs() const noexcept {
    const render::FrameTiming* f = m_passTimer.latest();
    return f ? f->gpuMs : 0.0f;
}

std::vector<render::FrameTiming> VulkanRenderer::frameTimings(std::size_t n) {
    if (m_device != VK_NULL_HANDLE && m_frameResources.isInitialized()) {
        m_passTimer.collectAll([&](uint32_t slot) {
            return vkGetFenceStatus(m_device, m_frameResources.getFrame(slot).renderFence) == VK_SUCCESS;
        });
    }
    return m_passTimer.recent(n);
}

bool VulkanRenderer::initializeDeferredRenderer() {
    OHAO_LOG(Debug, gpu, "VulkanRenderer: Creating DeferredRenderer...");
    if (!m_deferredRenderer) {
        m_deferredRenderer = std::make_unique<DeferredRenderer>();
    }
    OHAO_LOG(Debug, gpu, "VulkanRenderer: Calling DeferredRenderer::initialize()...");
    if (!m_deferredRenderer->initialize(m_device, m_physicalDevice)) {
        OHAO_LOG(Error, gpu, "VulkanRenderer: DeferredRenderer::initialize() failed!");
        m_deferredRenderer.reset();
        return false;
    }
    OHAO_LOG(Info, gpu, "VulkanRenderer: DeferredRenderer initialized, resizing to {}x{}", m_width, m_height);
    m_deferredRenderer->setPassTimer(&m_passTimer);
    m_deferredRenderer->setRenderSeed(m_renderSeed);
    m_deferredRenderer->onResize(m_width, m_height);
    if (auto* pp = m_deferredRenderer->getPostProcessing()) {
        pp->setExposure(0.6f);
        pp->setNoiseSeed(m_renderSeed);
    }
    return true;
}

void VulkanRenderer::setDenoiseMode(DenoiseMode mode) {
    m_denoiseModeOverridden = true;
    if (mode != m_denoiseMode) {
        m_denoiseMode = mode;
        m_denoiseCacheValid = false;
    }
}

void VulkanRenderer::setRealtimeSamplesPerFrame(uint32_t n) {
    m_realtimeSamplesPerFrame = std::clamp(n, 1u, 64u);
    // Push immediately so the value takes effect even outside the per-frame
    // prepareRTSceneForFrame path; the next frame re-injects it anyway.
    m_rtSettings.samplesPerFrame = m_realtimeSamplesPerFrame;
    if (auto* renderer = getRTRenderer(m_renderMode)) {
        renderer->setRenderSettings(m_rtSettings);
    }
}

bool VulkanRenderer::denoiseRequestedFrame() {
    if (m_denoiseCacheValid) return true;
    // NRD denoises on the GPU: read its tonemapped output (binding 30).
    if (m_denoiseMode == DenoiseMode::NRD) {
        std::vector<uint8_t> rgba;
        uint32_t rw = 0, rh = 0;
        if (!readbackNrdTonemapped(rgba, rw, rh)) {
            OHAO_LOG(Error, gpu, "Denoise: NRD readback failed; returning the noisy frame");
            return false;
        }
        m_denoisedPixelBuffer = std::move(rgba);
        m_denoiseCacheValid = true;
        return true;
    }
    // OIDN: the frame's beauty, albedo and normal AOVs, denoised on the CPU.
    std::vector<float> beautyRGBA, albedoRGBA, normalRGBA;
    uint32_t rw = 0, rh = 0;
    if (!readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {
        OHAO_LOG(Error, gpu, "Denoise: readbackHDRBuffers failed; returning the noisy frame");
        return false;
    }
    auto beauty3 = ohao::rgba32fToFloat3(beautyRGBA.data(), rw, rh);
    auto albedo3 = ohao::rgba32fToFloat3(albedoRGBA.data(), rw, rh);
    auto normal3 = ohao::rgba32fToFloat3(normalRGBA.data(), rw, rh);
    if (!ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(), rw, rh, /*hdr*/ true)) {
        OHAO_LOG(Error, gpu, "Denoise: OIDN failed; returning the noisy frame");
        return false;
    }
    m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, /*exposure*/ 0.5f);
    m_denoiseCacheValid = true;
    return true;
}

VkImage VulkanRenderer::getMotionVectorImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getMotionVectorImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getMotionVectorImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getDepthAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDepthAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDepthAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getRoughnessAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getRoughnessAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getRoughnessAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getDiffuseRadianceAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDiffuseRadianceAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDiffuseRadianceAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getSpecularRadianceAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getSpecularRadianceAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getSpecularRadianceAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getDiffAlbedoAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDiffAlbedoAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDiffAlbedoAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getSpecColorAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getSpecColorAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getSpecColorAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getNormalRoughnessAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getNormalRoughnessAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getNormalRoughnessAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getOutDiffRadianceAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getOutDiffRadianceAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getOutDiffRadianceAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getOutSpecRadianceAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getOutSpecRadianceAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getOutSpecRadianceAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getNrdComposedAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getNrdComposedAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getNrdComposedAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getNrdTonemappedAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getNrdTonemappedAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getNrdTonemappedAOVImage();
    }
    return VK_NULL_HANDLE;
}

bool VulkanRenderer::readbackMotionVector(std::vector<uint16_t>& mvRaw, uint32_t& width, uint32_t& height) {
    VkImage mvImage = getMotionVectorImage();
    if (mvImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RG16F = 4 bytes/pixel
    mvRaw.resize(static_cast<size_t>(width) * height * 2);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition MV image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = mvImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition MV image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = mvImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(mvRaw.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackDepthAOV(std::vector<float>& depthData, uint32_t& width, uint32_t& height) {
    VkImage depthImage = getDepthAOVImage();
    if (depthImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // R32F = 4 bytes/pixel
    depthData.resize(static_cast<size_t>(width) * height);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition depth image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = depthImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition depth image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = depthImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(depthData.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackRoughnessAOV(std::vector<float>& roughData, uint32_t& width, uint32_t& height) {
    VkImage roughImage = getRoughnessAOVImage();
    if (roughImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 2; // R16F = 2 bytes/pixel
    roughData.resize(static_cast<size_t>(width) * height);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition roughness image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = roughImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, roughImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition roughness image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = roughImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    const uint16_t* halfs = reinterpret_cast<const uint16_t*>(mapped);
    for (uint32_t i = 0; i < width * height; i++) {
        roughData[i] = half_to_float(halfs[i]);
    }
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackDiffuseRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getDiffuseRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition diffuse radiance image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition diffuse radiance image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackSpecularRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getSpecularRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition specular radiance image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition specular radiance image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackDenoisedDiffuse(std::vector<float>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getOutDiffRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition denoised diffuse image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition denoised diffuse image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackDenoisedSpecular(std::vector<float>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getOutSpecRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition denoised specular image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition denoised specular image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackNrdComposed(std::vector<float>& data,
                                          uint32_t& width, uint32_t& height) {
    VkImage srcImage = getNrdComposedAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes
    data.resize(static_cast<size_t>(width) * height * 4);

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = byteCount;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = m_commandPool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image            = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            stagingBuf, 1, &region);

    VkImageMemoryBarrier toGen{};
    toGen.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image            = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackNrdTonemapped(std::vector<uint8_t>& data,
                                            uint32_t& width, uint32_t& height) {
    VkImage srcImage = getNrdTonemappedAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RGBA8 = 4 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = byteCount;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = m_commandPool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image            = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            stagingBuf, 1, &region);

    VkImageMemoryBarrier toGen{};
    toGen.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image            = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getDiffAlbedoAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RGBA8 = 4 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition diff albedo image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition diff albedo image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getSpecColorAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RGBA8 = 4 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition spec color image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition spec color image back to GENERAL
    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

} // namespace ohao
