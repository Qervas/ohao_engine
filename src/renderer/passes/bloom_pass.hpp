#pragma once

#include "render_pass_base.hpp"
#include <array>
#include <vector>

namespace ohao {

// Bloom post-processing pass
// Multi-stage: Threshold -> Downsample chain -> Upsample chain -> Composite
class BloomPass : public RenderPassBase {
public:
    static constexpr uint32_t MAX_MIP_LEVELS = 8;

    BloomPass() = default;
    ~BloomPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "BloomPass"; }

    // Configuration
    void setInputImage(VkImageView hdrInput);
    void setThreshold(float threshold) { m_threshold = threshold; }
    void setSoftThreshold(float softThreshold) { m_softThreshold = softThreshold; }
    void setIntensity(float intensity) { m_intensity = intensity; }
    void setFilterRadius(float radius) { m_filterRadius = radius; }

    // Get output (for compositing)
    VkImageView getBloomOutput() const { return m_mipViews[0]; }

private:
    bool createMipChain();
    bool createRenderPasses();
    bool createPipelines();
    bool createDescriptors();
    void destroyMipChain();

    void executeThreshold(VkCommandBuffer cmd);
    void executeDownsample(VkCommandBuffer cmd);
    void executeUpsample(VkCommandBuffer cmd);

    // Input
    VkImageView m_hdrInputView{VK_NULL_HANDLE};

    // Bloom mip chain
    VkImage m_bloomImage{VK_NULL_HANDLE};
    VkDeviceMemory m_bloomMemory{VK_NULL_HANDLE};
    std::array<VkImageView, MAX_MIP_LEVELS> m_mipViews{};
    std::array<VkFramebuffer, MAX_MIP_LEVELS> m_framebuffers{};
    uint32_t m_mipLevels{0};
    std::array<glm::uvec2, MAX_MIP_LEVELS> m_mipSizes{};

    // Render passes
    VkRenderPass m_thresholdRenderPass{VK_NULL_HANDLE};
    VkRenderPass m_downsampleRenderPass{VK_NULL_HANDLE};
    VkRenderPass m_upsampleRenderPass{VK_NULL_HANDLE};

    // Pipelines
    VkPipeline m_thresholdPipeline{VK_NULL_HANDLE};
    VkPipeline m_downsamplePipeline{VK_NULL_HANDLE};
    VkPipeline m_upsamplePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_thresholdLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_downsampleLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_upsampleLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_inputLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> m_descriptorSets;

    // Sampler
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Parameters
    float m_threshold{1.0f};
    float m_softThreshold{0.5f};
    float m_intensity{1.0f};
    float m_filterRadius{1.0f};

    // Push constants
    struct ThresholdParams {
        float threshold;
        float softThreshold;
        float intensity;
        float padding;
    };

    struct SampleParams {
        glm::vec2 texelSize;
        float filterRadius;
        float blendFactor;
    };
};

} // namespace ohao
