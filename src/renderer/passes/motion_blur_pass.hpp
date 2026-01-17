#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Per-pixel Motion Blur using velocity buffer
class MotionBlurPass : public RenderPassBase {
public:
    MotionBlurPass() = default;
    ~MotionBlurPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "MotionBlurPass"; }

    // Input configuration
    void setColorBuffer(VkImageView color);
    void setVelocityBuffer(VkImageView velocity);
    void setDepthBuffer(VkImageView depth);

    void updateDescriptorSet();

    // Configuration
    void setIntensity(float intensity) { m_intensity = intensity; }
    void setMaxSamples(uint32_t samples) { m_maxSamples = samples; }
    void setVelocityScale(float scale) { m_velocityScale = scale; }
    void setTileSize(uint32_t size) { m_tileSize = size; }

    // Output
    VkImageView getOutputView() const { return m_outputView; }
    VkSampler getSampler() const { return m_sampler; }

private:
    bool createOutputImage();
    bool createTileBuffers();
    bool createDescriptors();
    bool createTileMaxPipeline();
    bool createNeighborMaxPipeline();
    bool createBlurPipeline();
    void destroyResources();

    // Input views
    VkImageView m_colorView{VK_NULL_HANDLE};
    VkImageView m_velocityView{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};

    // Output
    VkImage m_outputImage{VK_NULL_HANDLE};
    VkDeviceMemory m_outputMemory{VK_NULL_HANDLE};
    VkImageView m_outputView{VK_NULL_HANDLE};

    // Tile max velocity (for scattered blur)
    VkImage m_tileMaxImage{VK_NULL_HANDLE};
    VkDeviceMemory m_tileMaxMemory{VK_NULL_HANDLE};
    VkImageView m_tileMaxView{VK_NULL_HANDLE};

    // Neighbor max (dilated tile max)
    VkImage m_neighborMaxImage{VK_NULL_HANDLE};
    VkDeviceMemory m_neighborMaxMemory{VK_NULL_HANDLE};
    VkImageView m_neighborMaxView{VK_NULL_HANDLE};

    // Pipelines
    VkPipeline m_tileMaxPipeline{VK_NULL_HANDLE};
    VkPipeline m_neighborMaxPipeline{VK_NULL_HANDLE};
    VkPipeline m_blurPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Sampler
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Parameters
    float m_intensity{1.0f};
    uint32_t m_maxSamples{16};
    float m_velocityScale{1.0f};
    uint32_t m_tileSize{20};

    // Push constants
    struct MotionBlurParams {
        glm::vec4 screenSize;     // xy = size, zw = 1/size
        float intensity;
        float velocityScale;
        uint32_t maxSamples;
        uint32_t tileSize;
    };
};

} // namespace ohao
