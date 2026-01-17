#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace ohao {

// IBL (Image-Based Lighting) processor
// Handles HDR environment map loading and preprocessing for PBR rendering
class IBLProcessor {
public:
    IBLProcessor() = default;
    ~IBLProcessor();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    VkCommandPool commandPool, VkQueue graphicsQueue);
    void cleanup();

    // Load and process an HDR environment map
    // This performs: equirect->cubemap, prefilter specular, generate irradiance
    bool loadEnvironmentMap(const std::string& hdrPath);

    // Generate BRDF LUT (call once, reusable for all environments)
    bool generateBRDFLUT();

    // Get processed textures for rendering
    VkImageView getEnvironmentCubemapView() const { return m_envCubemapView; }
    VkImageView getIrradianceCubemapView() const { return m_irradianceView; }
    VkImageView getPrefilteredCubemapView() const { return m_prefilteredView; }
    VkImageView getBRDFLUTView() const { return m_brdfLUTView; }
    VkSampler getCubemapSampler() const { return m_cubemapSampler; }
    VkSampler getBRDFSampler() const { return m_brdfSampler; }

    // Check if IBL is ready
    bool isReady() const { return m_envCubemapView != VK_NULL_HANDLE; }

    // Cubemap size configuration
    static constexpr uint32_t ENV_CUBEMAP_SIZE = 512;
    static constexpr uint32_t IRRADIANCE_SIZE = 32;
    static constexpr uint32_t PREFILTER_SIZE = 128;
    static constexpr uint32_t PREFILTER_MIP_LEVELS = 5;
    static constexpr uint32_t BRDF_LUT_SIZE = 512;

private:
    bool loadHDRImage(const std::string& path, std::vector<float>& pixels,
                      uint32_t& width, uint32_t& height);
    bool createEquirectTexture(const std::vector<float>& pixels,
                               uint32_t width, uint32_t height);
    bool createCubemapImages();
    bool createComputePipelines();
    bool createDescriptors();

    void executeEquirectToCubemap();
    void executeIrradianceConvolution();
    void executePrefilterEnvironment();
    void executeBRDFIntegration();
    void updateDescriptorSet(VkDescriptorSet descSet, VkImageView inputView,
                             VkSampler sampler, VkImageView outputView);

    VkShaderModule loadShaderModule(const std::string& path);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels = 1, uint32_t layerCount = 1);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};

    // Equirectangular HDR input
    VkImage m_equirectImage{VK_NULL_HANDLE};
    VkDeviceMemory m_equirectMemory{VK_NULL_HANDLE};
    VkImageView m_equirectView{VK_NULL_HANDLE};

    // Environment cubemap (HDR)
    VkImage m_envCubemap{VK_NULL_HANDLE};
    VkDeviceMemory m_envCubemapMemory{VK_NULL_HANDLE};
    VkImageView m_envCubemapView{VK_NULL_HANDLE};

    // Irradiance cubemap (diffuse IBL)
    VkImage m_irradianceCubemap{VK_NULL_HANDLE};
    VkDeviceMemory m_irradianceMemory{VK_NULL_HANDLE};
    VkImageView m_irradianceView{VK_NULL_HANDLE};

    // Prefiltered environment map (specular IBL with roughness mips)
    VkImage m_prefilteredCubemap{VK_NULL_HANDLE};
    VkDeviceMemory m_prefilteredMemory{VK_NULL_HANDLE};
    VkImageView m_prefilteredView{VK_NULL_HANDLE};
    std::vector<VkImageView> m_prefilteredMipViews;

    // BRDF LUT
    VkImage m_brdfLUT{VK_NULL_HANDLE};
    VkDeviceMemory m_brdfLUTMemory{VK_NULL_HANDLE};
    VkImageView m_brdfLUTView{VK_NULL_HANDLE};

    // Samplers
    VkSampler m_cubemapSampler{VK_NULL_HANDLE};
    VkSampler m_brdfSampler{VK_NULL_HANDLE};

    // Compute pipelines
    VkPipeline m_equirectToCubemapPipeline{VK_NULL_HANDLE};
    VkPipeline m_irradiancePipeline{VK_NULL_HANDLE};
    VkPipeline m_prefilterPipeline{VK_NULL_HANDLE};
    VkPipeline m_brdfPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptor sets
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_equirectDescSet{VK_NULL_HANDLE};
    VkDescriptorSet m_irradianceDescSet{VK_NULL_HANDLE};
    VkDescriptorSet m_prefilterDescSet{VK_NULL_HANDLE};
    VkDescriptorSet m_brdfDescSet{VK_NULL_HANDLE};
};

} // namespace ohao
