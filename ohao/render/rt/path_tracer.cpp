#include "path_tracer.hpp"

#include <iostream>

// Always include: nrd_denoise.hpp is header-only safe regardless of
// OHAO_NRD_ENABLED (just declares the class). PathTracer holds an
// unconditional std::unique_ptr<NrdDenoiser>, so the destructor needs
// NrdDenoiser to be a complete type in this TU for the unique_ptr
// instantiation to compile. Method calls on NrdDenoiser remain guarded
// by OHAO_NRD_ENABLED because definitions only exist in that mode.
#include "render/rt/denoise/nrd_denoise.hpp"
#include "render/rt/denoise/nrd_compose.hpp"
#include "render/rt/denoise/nrd_tonemap.hpp"

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
    m_width = width;
    m_height = height;
    m_sampleIndex = 0;
    m_historyFrameCount = 0;

    if (!loadFunctionPointers()) {
        std::cerr << "[PathTracer] Failed to load RT function pointers" << std::endl;
        return false;
    }

    if (!createImages()) {
        std::cerr << "[PathTracer] Failed to create output images" << std::endl;
        return false;
    }

    if (!createMaterialBuffer()) {
        std::cerr << "[PathTracer] Failed to create material buffer" << std::endl;
        return false;
    }

    if (!createDescriptorResources()) {
        std::cerr << "[PathTracer] Failed to create descriptor resources" << std::endl;
        return false;
    }

    if (!createRTPipeline()) {
        std::cerr << "[PathTracer] Failed to create RT pipeline" << std::endl;
        return false;
    }

    if (!createShaderBindingTable()) {
        std::cerr << "[PathTracer] Failed to create SBT" << std::endl;
        return false;
    }

    std::cout << "[PathTracer] Initialized (" << width << "x" << height << ")" << std::endl;

#ifdef OHAO_NRD_ENABLED
    m_nrdDenoiser = std::make_unique<NrdDenoiser>();
    if (instance != VK_NULL_HANDLE &&
        m_nrdDenoiser->initialize(instance, m_device, m_physicalDevice,
                                   graphicsQueueFamilyIndex,
                                   instanceExtensions, deviceExtensions,
                                   m_width, m_height)) {
        // Sub-plan 4.F T4: apply NVIDIA's reference REBLUR tuning for 1-4spp input.
        // Stock NRD defaults target 0.25spp game-style input with SHARC/ReSTIR; our
        // path tracer feeds N-spp averaged AOVs (see 4.F T3), so longer accumulation
        // + production prepass radii give visibly cleaner output.
        NrdDenoiser::NrdReblurProfile profile{};
        const bool ok = m_nrdDenoiser->setReblurSettings(profile);
        std::cout << "[NRD] persistent instance ready @ " << m_width << "x" << m_height
                  << (ok ? " (T4: production ReblurSettings applied)"
                         : " (T4: ReblurSettings apply FAILED — defaults in use)")
                  << std::endl;
    } else {
        if (instance == VK_NULL_HANDLE) {
            std::cerr << "[NRD] no VkInstance provided — skipping NRD init (caller should plumb it)" << std::endl;
        } else {
            std::cerr << "[NRD] persistent instance init FAILED — disabling NRD path" << std::endl;
        }
        m_nrdDenoiser.reset();
    }
    m_nrdCompositor = std::make_unique<NrdCompositor>();
    if (!m_nrdCompositor->initialize(m_device, m_physicalDevice, m_width, m_height)) {
        std::cerr << "[NRD compose] init FAILED — compose pass will be skipped" << std::endl;
        m_nrdCompositor.reset();
    }
    m_nrdTonemap = std::make_unique<NrdTonemap>();
    if (!m_nrdTonemap->initialize(m_device, m_physicalDevice, m_width, m_height)) {
        std::cerr << "[NRD tonemap] init FAILED — tonemap pass will be skipped" << std::endl;
        m_nrdTonemap.reset();
    }
#else
    (void)instance; (void)graphicsQueueFamilyIndex;
    (void)instanceExtensions; (void)deviceExtensions;
#endif

    return true;
}


// ─── Accumulation reset ──────────────────────────────────────────────

void PathTracer::resetAccumulation() {
    m_sampleIndex = 0;
    m_historyFrameCount = 0;
    m_viewChangedThisFrame = false;
    m_surfaceHistoryWriteIndex = 0;
    m_surfaceHistoryInitialized[0] = false;
    m_surfaceHistoryInitialized[1] = false;
    m_shadingHistoryWriteIndex = 0;
    m_shadingHistoryInitialized[0] = false;
    m_shadingHistoryInitialized[1] = false;

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
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    // Recreate both images at new resolution
    destroyImages();
    createImages();

    // Reset accumulation since the buffer dimensions changed
    m_sampleIndex = 0;
    m_historyFrameCount = 0;
    m_viewChangedThisFrame = false;
    m_surfaceHistoryWriteIndex = 0;
    m_surfaceHistoryInitialized[0] = false;
    m_surfaceHistoryInitialized[1] = false;
    m_shadingHistoryWriteIndex = 0;
    m_shadingHistoryInitialized[0] = false;
    m_shadingHistoryInitialized[1] = false;

    // Sub-plan 4.E T2: keep NRD's prev matrices in sync with frameIndex=0
    // bootstrap after resize.
    m_prevViewMatrix = glm::mat4(1.0f);
    m_prevProjMatrix = glm::mat4(1.0f);

    // Sub-plan 4.F T4: same rationale as in resetAccumulation().
    m_jitterCurrent = glm::vec2(0.0f);
    m_jitterPrev    = glm::vec2(0.0f);
    m_haltonIndex   = 0;
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
    if (m_nrdTonemap) {
        m_nrdTonemap->shutdown();
        m_nrdTonemap.reset();
    }
#endif

    // CDF buffers are owned by VulkanRenderer, not by the path tracer.
    // Clear the cached handles so later destroy calls or reuse don't touch freed memory.
    m_envMarginalCDFBuffer = VK_NULL_HANDLE;
    m_envConditionalCDFBuffer = VK_NULL_HANDLE;
    m_envCDFWidth = 0;
    m_envCDFHeight = 0;
    m_envCDFIntegral = 0.0f;

    // SBT
    if (m_sbtBuffer) { vkDestroyBuffer(m_device, m_sbtBuffer, nullptr); m_sbtBuffer = VK_NULL_HANDLE; }
    if (m_sbtMemory) { vkFreeMemory(m_device, m_sbtMemory, nullptr); m_sbtMemory = VK_NULL_HANDLE; }

    // Pipeline
    if (m_rtPipeline) { vkDestroyPipeline(m_device, m_rtPipeline, nullptr); m_rtPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }

    // Descriptors
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    m_descriptorSet = VK_NULL_HANDLE;  // freed with pool

    // Material buffer
    if (m_materialBuffer) { vkDestroyBuffer(m_device, m_materialBuffer, nullptr); m_materialBuffer = VK_NULL_HANDLE; }
    if (m_materialMemory) { vkFreeMemory(m_device, m_materialMemory, nullptr); m_materialMemory = VK_NULL_HANDLE; }

    // Images
    destroyImages();
}

} // namespace ohao
