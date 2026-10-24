#pragma once

// RT Shadow Technique — hardware ray traced shadows.
//
// Fires shadow rays from each visible pixel toward up to kMaxShadowLights scene lights
// (ShadowInput::lights), sampling points on each light's area for the penumbra, and
// writes one RGBA8 mask channel per light.
//
// Replaces: CSM (cascade shadow maps) with 3x3 PCF
// Advantages: pixel-perfect shadows, all light types, soft penumbra, no cascade artifacts

#include "render_technique.hpp"
#include "rt_acceleration_structure.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/frame/frame_resources.hpp"
#include <algorithm>
#include <array>

namespace ohao {

class RTShadowTechnique : public IShadowTechnique {
public:
    RTShadowTechnique() = default;
    ~RTShadowTechnique() override;

    const char* getName() const override { return "RT Shadows"; }
    bool needsRT() const override { return true; }

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    void render(VkCommandBuffer cmd, const ShadowInput& input) override;
    ShadowOutput getOutput() const override;
    // True when the last render() traced rays. False means it bailed (no TLAS, no
    // pipeline) and the output image was never written this frame: don't sample it.
    [[nodiscard]] bool rendered() const { return m_rendered; }
    [[nodiscard]] VkExtent2D extent() const { return {m_width, m_height}; }   // shadow mask size
    void destroy() override;

    // Shadow rays a pixel, shared evenly by the frame's shadow lights (each gets
    // max(1, rays / lights)): the pass costs about the same with one light or four.
    void setRaysPerPixel(uint32_t rays) { m_raysPerPixel = rays; }
    [[nodiscard]] static uint32_t raysPerLight(uint32_t raysPerPixel, uint32_t lights) {
        return lights == 0 ? 0u : std::max(1u, raysPerPixel / lights);
    }

private:
    // G-buffer sampler. Lives as long as the technique: it is written into a
    // descriptor set that recorded command buffers reference, so destroying it
    // before those buffers finish executing invalidates them.
    VkSampler m_gbufferSampler{VK_NULL_HANDLE};
    bool createOutputImage();
    bool createRTPipeline();
    bool createShaderBindingTable();
    bool createDescriptorResources();
    bool loadFunctionPointers();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    uint32_t m_raysPerPixel = 8;

    // Output shadow mask
    VkImage m_shadowMask = VK_NULL_HANDLE;
    VkDeviceMemory m_shadowMaskMemory = VK_NULL_HANDLE;
    VkImageView m_shadowMaskView = VK_NULL_HANDLE;

    // RT Pipeline
    VkPipeline m_rtPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // One set per frame in flight: render() rewrites its slot's set at record time.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};
    bool m_rendered = false;

    // Shader Binding Table
    VkBuffer m_sbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_sbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};  // empty, not used

    // Push constants (rt_shadow.rgen), 224 bytes: three vec4s a light, then the params.
    struct ShadowLightGpu {
        glm::vec4 positionAndType;   // xyz = position (directional: travel direction), w = 0 dir, 1 sphere, 2 rect
        glm::vec4 edge1AndRadius;    // xyz = rect edge 1, w = sphere radius (directional: angular radius, rad)
        glm::vec4 edge2AndRange;     // xyz = rect edge 2, w = range (0: unlimited)
    };
    struct ShadowPushConstants {
        ShadowLightGpu lights[4];
        glm::uvec4 params;           // x=width, y=height, z=light count, w=rays per light
        glm::uvec4 seed;             // x=render seed, y=frame index since the last temporal reset
    };
    static_assert(sizeof(ShadowPushConstants) <= 256, "within the guaranteed push-constant range");

    // Function pointers
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddressFn = nullptr;

    // Helpers
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
};

// Compile-time check: concrete technique still models the documented concept.
static_assert(ShadowTechniqueLike<RTShadowTechnique>);

} // namespace ohao
