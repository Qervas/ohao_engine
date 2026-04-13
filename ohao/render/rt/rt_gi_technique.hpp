#pragma once

// RT Global Illumination — 1-bounce path traced indirect lighting.
// Traces cosine-weighted rays from each pixel, hits surfaces, reads their
// albedo from a material buffer, and computes indirect light contribution.
// This creates color bleeding (e.g. red wall tinting white floor in Cornell box).

#include "render_technique.hpp"
#include "rt_acceleration_structure.hpp"
#include <vector>

namespace ohao {

class RTGITechnique : public IGITechnique {
public:
    ~RTGITechnique() override;

    const char* getName() const override { return "RT GI"; }
    bool needsRT() const override { return true; }

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    void render(VkCommandBuffer cmd, const GIInput& input) override;
    GIOutput getOutput() const override;
    void destroy() override;

    void setSampleCount(int samples) { m_sampleCount = samples; }

    // Set per-instance material albedo colors (must match TLAS instance order)
    void setMaterialAlbedos(const std::vector<glm::vec3>& albedos);

private:
    bool createOutputImage();
    bool createRTPipeline();
    bool createShaderBindingTable();
    bool createDescriptorResources();
    bool createMaterialBuffer();
    bool loadFunctionPointers();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
    int m_sampleCount = 4;

    // Output (current frame)
    VkImage m_giOutput = VK_NULL_HANDLE;
    VkDeviceMemory m_giOutputMemory = VK_NULL_HANDLE;
    VkImageView m_giOutputView = VK_NULL_HANDLE;

    // Temporal history (previous frame, blended)
    VkImage m_giHistory = VK_NULL_HANDLE;
    VkDeviceMemory m_giHistoryMemory = VK_NULL_HANDLE;
    VkImageView m_giHistoryView = VK_NULL_HANDLE;

    // RT Pipeline
    VkPipeline m_rtPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    // SBT
    VkBuffer m_sbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_sbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};

    // Material buffer (per-instance albedo)
    VkBuffer m_materialBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_materialMemory = VK_NULL_HANDLE;
    std::vector<glm::vec4> m_materialData;

    struct GIPushConstants {
        glm::mat4 invView;
        glm::mat4 invProj;
        glm::vec4 lightPosAndIntensity;
        glm::uvec4 params;  // x=width, y=height, z=sampleCount, w=frameIndex
    };

    uint32_t m_frameIndex = 0;

    // Function pointers
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddressFn = nullptr;

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
};

} // namespace ohao
