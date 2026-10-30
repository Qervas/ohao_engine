#include "path_tracer.hpp"
#include "render/scene/render_scene.hpp"

#include "core/log.hpp"
#include <algorithm>
#include <cmath>

// Always include: nrd_denoise.hpp is header-only safe regardless of
// OHAO_NRD_ENABLED (just declares the class). PathTracer holds an
// unconditional std::unique_ptr<NrdDenoiser>, so the destructor needs
// NrdDenoiser to be a complete type in this TU for the unique_ptr
// instantiation to compile. Method calls on NrdDenoiser remain guarded
// by OHAO_NRD_ENABLED because definitions only exist in that mode.
#include "render/rt/denoise/nrd_denoise.hpp"
#include "render/rt/denoise/nrd_compose.hpp"
#include "render/rt/denoise/nrd_cinematic.hpp"
// Unconditional: PathTracer holds a std::unique_ptr<AtrousDenoiser>, so this
// TU (which defines the out-of-line dtor) needs the complete type. Not
// OHAO_NRD-guarded — the à-trous path is independent of NRD.
#include "render/rt/denoise/atrous_denoise.hpp"
// DLSS-RR wrapper is only a complete type in this TU (and path_tracer_render.cpp)
// where OHAO_DLSS_ENABLED is defined; the header only forward-declares DlssRR so
// the unique_ptr member's out-of-line dtor (below) needs the full type here.
#ifdef OHAO_DLSS_ENABLED
#include "render/rt/denoise/dlss_rr.hpp"
#endif

namespace ohao {

PathTracer::PathTracer() = default;

PathTracer::~PathTracer() {
    destroy();
}

// ─── Function pointer loading ─────────────────────────────────────────

bool PathTracer::loadFunctionPointers() {
    vkCreateRayTracingPipelinesKHR =
        (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR");
    vkGetRayTracingShaderGroupHandlesKHR =
        (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR");
    vkCmdTraceRaysKHR =
        (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(m_device, "vkCmdTraceRaysKHR");
    vkGetBufferDeviceAddressFn =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddress");

    return vkCreateRayTracingPipelinesKHR && vkGetRayTracingShaderGroupHandlesKHR &&
           vkCmdTraceRaysKHR && vkGetBufferDeviceAddressFn;
}

// ─── Helpers ──────────────────────────────────────────────────────────

uint32_t PathTracer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

VkDeviceAddress PathTracer::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddressFn(m_device, &info);
}


// ─── Initialization ──────────────────────────────────────────────────

bool PathTracer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                       uint32_t width, uint32_t height,
                       VkInstance instance,
                       uint32_t graphicsQueueFamilyIndex,
                       const std::vector<const char*>& instanceExtensions,
                       const std::vector<const char*>& deviceExtensions) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    // `width`/`height` from the caller are the OUTPUT/display resolution. The
    // internal render resolution is derived from it (== output unless a DLSS
    // upscaling preset is active — which it is NOT at init(), since the denoise
    // mode is still the profile default here; DLSSRR is applied later via
    // setRenderSettings(), which recomputes + reallocates if the scale changes).
    m_outW = width;
    m_outH = height;
    computeRenderResolution();   // sets m_width/m_height (render res)
    m_sampleIndex = 0;
    m_historyFrameCount = 0;

    if (!loadFunctionPointers()) {
        OHAO_LOG(Error, render, "PathTracer: failed to load RT function pointers");
        return false;
    }

    if (!createImages()) {
        OHAO_LOG(Error, render, "PathTracer: failed to create output images");
        return false;
    }

    if (!m_openpbrTables.create(m_device, m_physicalDevice)) {
        OHAO_LOG(Error, render, "PathTracer: failed to upload OpenPBR's energy tables");
        return false;
    }

    if (!createDescriptorResources()) {
        OHAO_LOG(Error, render, "PathTracer: failed to create descriptor resources");
        return false;
    }

    if (!createRTPipeline()) {
        OHAO_LOG(Error, render, "PathTracer: failed to create RT pipeline");
        return false;
    }

    if (!createShaderBindingTable()) {
        OHAO_LOG(Error, render, "PathTracer: failed to create SBT");
        return false;
    }

    OHAO_LOG(Info, render, "PathTracer: initialized ({}x{})", width, height);

#ifdef OHAO_NRD_ENABLED
    // NRD / à-trous / cinematic passes are sized to OUTPUT res: they only run in
    // non-upscaling modes (render==output), and this keeps them valid even if a
    // later switch to a DLSS upscaling preset shrinks the internal render res.
    m_nrdDenoiser = std::make_unique<NrdDenoiser>();
    if (instance != VK_NULL_HANDLE &&
        m_nrdDenoiser->initialize(instance, m_device, m_physicalDevice,
                                   graphicsQueueFamilyIndex,
                                   instanceExtensions, deviceExtensions,
                                   m_outW, m_outH)) {
        // Sub-plan 4.F T4: apply NVIDIA's reference REBLUR tuning for 1-4spp input.
        // Stock NRD defaults target 0.25spp game-style input with SHARC/ReSTIR; our
        // path tracer feeds N-spp averaged AOVs (see 4.F T3), so longer accumulation
        // + production prepass radii give visibly cleaner output.
        NrdDenoiser::NrdReblurProfile profile{};
        const bool ok = m_nrdDenoiser->setReblurSettings(profile);
        OHAO_LOG(Info, render, "NRD: persistent instance ready @ {}x{}{}", m_width, m_height,
                 ok ? " (T4: production ReblurSettings applied)"
                    : " (T4: ReblurSettings apply FAILED — defaults in use)");
    } else {
        if (instance == VK_NULL_HANDLE) {
            OHAO_LOG(Warn, render, "NRD: no VkInstance provided — skipping NRD init (caller should plumb it)");
        } else {
            OHAO_LOG(Error, render, "NRD: persistent instance init failed — disabling NRD path");
        }
        m_nrdDenoiser.reset();
    }
    m_nrdCompositor = std::make_unique<NrdCompositor>();
    if (!m_nrdCompositor->initialize(m_device, m_physicalDevice, m_outW, m_outH)) {
        OHAO_LOG(Error, render, "NRD compose: init failed — compose pass will be skipped");
        m_nrdCompositor.reset();
    }
    m_cinematicPost = std::make_unique<NrdCinematicPost>();
    if (!m_cinematicPost->initialize(m_device, m_physicalDevice, m_outW, m_outH)) {
        OHAO_LOG(Error, render, "NRD cinematic: init failed — cinematic pass will be skipped");
        m_cinematicPost.reset();
    }
#else
    (void)instance; (void)graphicsQueueFamilyIndex;
    (void)instanceExtensions; (void)deviceExtensions;
#endif

    // À-trous beauty denoiser (DenoiseMode::Atrous) — independent of NRD.
    // Cheap to keep resident; only dispatched when the active denoise mode is
    // Atrous, so None/OIDN/NRD are unaffected.
    m_atrousDenoiser = std::make_unique<AtrousDenoiser>();
    if (!m_atrousDenoiser->initialize(m_device, m_physicalDevice, m_outW, m_outH)) {
        OHAO_LOG(Error, render, "atrous: init failed — --denoise=atrous will pass through noisy beauty");
        m_atrousDenoiser.reset();
    }

#ifdef OHAO_DLSS_ENABLED
    // DLSS-RR NGX init is deferred to the first DLSSRR-mode frame (render()), since
    // the active denoiseMode is not yet known here (settings apply per-frame). Just
    // stash the instance handle needed for that lazy init.
    m_dlssInstance = instance;
#endif

    return true;
}


// ─── Scene inputs ────────────────────────────────────────────────────

void PathTracer::setSceneBuffers(const render::RenderScene& scene) {
    m_tlas = scene.tlas();
    m_vertexArena = scene.vertexArena();
    m_indexArena = scene.indexArena();
    m_meshTable = scene.meshTable();
    m_triangleSlots = scene.triangleSlotBuffer();
    m_materialTable = scene.materialBuffer();
    m_instances = scene.instanceBuffer();
    m_materialRefs = scene.materialRefBuffer();
    m_lightBuffer = scene.rtLightBuffer();
}

// ─── Accumulation reset ──────────────────────────────────────────────

void PathTracer::resetAccumulation() {
    m_sampleIndex = m_renderSeed;
    m_historyFrameCount = 0;
    m_accumFrames = 0;
    m_viewChangedThisFrame = false;
    m_hasPreviousView = false;

    // Sub-plan 4.E T2: keep NRD's prev matrices in sync with frameIndex=0
    // bootstrap. Leaving them stale across a view change / resize could
    // confuse NRD's temporal reprojection on first post-reset frame.
    m_prevViewMatrix = glm::mat4(1.0f);
    m_prevProjMatrix = glm::mat4(1.0f);

    // Sub-plan 4.F T4: reset Halton sequence + stale jitter history so the first
    // post-reset frame's NRD cameraJitterPrev matches cameraJitter = Halton(0).
    m_jitterCurrent = glm::vec2(0.0f);
    m_jitterPrev    = glm::vec2(0.0f);
    m_haltonIndex   = 0;
}

// ─── Resize ──────────────────────────────────────────────────────────

void PathTracer::resize(uint32_t width, uint32_t height) {
    // width/height are the OUTPUT resolution. Recompute the internal render res
    // (may be smaller under a DLSS upscaling preset) and reallocate.
    if (width == m_outW && height == m_outH) return;
    m_outW = width;
    m_outH = height;
    computeRenderResolution();

    // Recreate both images at new resolution
    destroyImages();
    createImages();

#ifdef OHAO_DLSS_ENABLED
    // Render/output res changed → the DLSS feature (created for the old dims) is
    // stale. Release it so the next DLSSRR frame recreates it at the new dims.
    if (m_dlssRR) {
        m_dlssRR->releaseFeature();
    }
    m_dlssInitAttempted = false;
    m_dlssColorOutFirstFrame = true;
#endif

    // SVGF (DenoiseMode::Atrous) owns persistent history images sized to the
    // framebuffer — recreate them so they match the new resolution. The
    // first-frame latch + resetHistory path handle re-bootstrapping.
    if (m_atrousDenoiser) {
        m_atrousDenoiser->shutdown();
        if (!m_atrousDenoiser->initialize(m_device, m_physicalDevice, m_outW, m_outH)) {
            OHAO_LOG(Error, render, "svgf: resize re-init failed — atrous will pass through noisy beauty");
            m_atrousDenoiser.reset();
        }
    }

    // Reset accumulation since the buffer dimensions changed
    resetAccumulation();

    // Sub-plan 4.E T2: keep NRD's prev matrices in sync with frameIndex=0
    // bootstrap after resize.
    m_prevViewMatrix = glm::mat4(1.0f);
    m_prevProjMatrix = glm::mat4(1.0f);

    // Sub-plan 4.F T4: same rationale as in resetAccumulation().
    m_jitterCurrent = glm::vec2(0.0f);
    m_jitterPrev    = glm::vec2(0.0f);
    m_haltonIndex   = 0;
}

// ─── Render-resolution derivation (DLSS upscaling) ───────────────────
//
// Decouples the internal RENDER resolution from the OUTPUT/display resolution.
// Only DLSSRR mode with an upscaling preset (Quality/Balanced/Performance/
// UltraPerformance) shrinks the render res; every other mode renders 1:1. The
// raygen traces at m_width×m_height and DLSS-RR reconstructs to m_outW×m_outH.
void PathTracer::computeRenderResolution() {
    float scale = 1.0f;
#ifdef OHAO_DLSS_ENABLED
    if (m_renderSettings.denoiseMode == DenoiseMode::DLSSRR) {
        scale = dlssRenderScale(dlssQualityFromEnv());
    }
#endif
    m_dlssRenderScale = scale;

    if (scale >= 0.999f) {
        // Native res — no upscale. Keep render == output exactly.
        m_width  = m_outW;
        m_height = m_outH;
        return;
    }

    // Round to the nearest even number (DLSS prefers even render dims), clamp ≥2.
    auto evenScale = [](uint32_t out, float s) -> uint32_t {
        uint32_t v = static_cast<uint32_t>(std::lround(static_cast<float>(out) * s));
        v &= ~1u;                 // align down to even
        return v < 2u ? 2u : v;
    };
    m_width  = evenScale(m_outW, scale);
    m_height = evenScale(m_outH, scale);
}

// ─── Render settings ─────────────────────────────────────────────────
//
// Switching to/from DLSSRR (or a different OHAO_DLSS_QUALITY) changes the
// internal render resolution, which requires reallocating every render-target
// image. This happens between frames (setRenderSettings is called from the
// renderer's per-frame prep, outside the command buffer), so a device-idle +
// destroy/create is safe here. The per-frame descriptor rewrite in render()
// rebinds all image views, so no stale descriptors survive.
void PathTracer::setRenderSettings(const RTRenderSettings& settings) {
    m_renderSettings = applyDenoisePolicy(settings);
    m_maxBounces = m_renderSettings.maxBounces;

    // If images haven't been created yet (pre-init), just store the settings.
    if (m_device == VK_NULL_HANDLE || m_outW == 0 || m_outH == 0) return;

    const uint32_t prevW = m_width, prevH = m_height;
    computeRenderResolution();
    if (m_width == prevW && m_height == prevH) return;   // render res unchanged — nothing to do

    // Render resolution changed (e.g. first switch into a DLSS upscaling preset).
    // Reallocate the render-target images at the new render res.
    vkDeviceWaitIdle(m_device);
    destroyImages();
    createImages();

#ifdef OHAO_DLSS_ENABLED
    // The DLSS feature (if any) was created for the old dims — force a fresh
    // lazy re-init at the new render/output dims on the next DLSSRR frame.
    if (m_dlssRR) {
        m_dlssRR->releaseFeature();
    }
    m_dlssInitAttempted      = false;
    m_dlssColorOutFirstFrame = true;
#endif

    // Buffer dimensions changed → restart accumulation + temporal history.
    resetAccumulation();

    OHAO_LOG(Info, render, "PathTracer: render res {}x{} → output {}x{} (scale {})",
             m_width, m_height, m_outW, m_outH, m_dlssRenderScale);
}

// ─── Cleanup ─────────────────────────────────────────────────────────

void PathTracer::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

#ifdef OHAO_NRD_ENABLED
    if (m_nrdDenoiser) {
        m_nrdDenoiser->shutdown();
        m_nrdDenoiser.reset();
    }
    if (m_nrdCompositor) {
        m_nrdCompositor->shutdown();
        m_nrdCompositor.reset();
    }
    if (m_cinematicPost) {
        m_cinematicPost->shutdown();
        m_cinematicPost.reset();
    }
#endif

    if (m_atrousDenoiser) {
        m_atrousDenoiser->shutdown();
        m_atrousDenoiser.reset();
    }

#ifdef OHAO_DLSS_ENABLED
    if (m_dlssRR) {
        m_dlssRR->shutdown();
        m_dlssRR.reset();
    }
#endif

    // CDF buffers are owned by VulkanRenderer, not by the path tracer.
    // Clear the cached handles so later destroy calls or reuse don't touch freed memory.
    m_envMarginalCDFBuffer = VK_NULL_HANDLE;
    m_envConditionalCDFBuffer = VK_NULL_HANDLE;
    m_envCDFWidth = 0;
    m_envCDFHeight = 0;
    m_envCDFIntegral = 0.0f;

    m_openpbrTables.destroy();

    // SBT
    if (m_sbtBuffer) { vkDestroyBuffer(m_device, m_sbtBuffer, nullptr); m_sbtBuffer = VK_NULL_HANDLE; }
    if (m_sbtMemory) { vkFreeMemory(m_device, m_sbtMemory, nullptr); m_sbtMemory = VK_NULL_HANDLE; }

    // Pipeline
    if (m_rtPipeline) { vkDestroyPipeline(m_device, m_rtPipeline, nullptr); m_rtPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }

    // Descriptors
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    m_descriptorSets = {};  // freed with pool

    // Images
    destroyImages();
}

} // namespace ohao
