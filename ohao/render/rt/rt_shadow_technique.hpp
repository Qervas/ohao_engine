#pragma once

// RT Shadow Technique — hardware ray traced shadows.
//
// Fires shadow rays from each visible pixel toward the light source.
// Produces a soft shadow mask with proper penumbra based on light size.
//
// Replaces: CSM (cascade shadow maps) with 3x3 PCF
// Advantages: pixel-perfect shadows, all light types, soft penumbra, no cascade artifacts

#include "render_technique.hpp"
#include "rt_acceleration_structure.hpp"
#include "render/rt/rt_meta.hpp"

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
    void destroy() override;

    // RT shadow config
    void setLightRadius(float radius) { m_lightRadius = radius; }  // for soft shadows
    void setSampleCount(int samples) { m_sampleCount = samples; }  // 1=hard, 4-16=soft

private:
    bool createOutputImage();
    bool createRTPipeline();
    bool createShaderBindingTable();
    bool createDescriptorResources();
    bool loadFunctionPointers();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // Shadow config
    float m_lightRadius = 0.05f;   // sun angular radius for penumbra
    int m_sampleCount = 1;          // shadow rays per pixel

    // Output shadow mask
    VkImage m_shadowMask = VK_NULL_HANDLE;
    VkDeviceMemory m_shadowMaskMemory = VK_NULL_HANDLE;
    VkImageView m_shadowMaskView = VK_NULL_HANDLE;

    // RT Pipeline
    VkPipeline m_rtPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    // Shader Binding Table
    VkBuffer m_sbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_sbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};  // empty, not used

    // Push constants for shadow ray configuration
    struct ShadowPushConstants {
        glm::mat4 invView;
        glm::mat4 invProj;
        glm::vec4 lightDirAndRadius;   // xyz=direction, w=angular radius
        glm::vec4 lightPosAndRange;    // xyz=position, w=range
        glm::uvec4 params;             // x=width, y=height, z=lightType, w=sampleCount
    };

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
