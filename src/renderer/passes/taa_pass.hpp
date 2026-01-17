#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Temporal Anti-Aliasing pass
class TAAPass : public RenderPassBase {
public:
    TAAPass() = default;
    ~TAAPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "TAAPass"; }

    // Configuration
    void setCurrentFrame(VkImageView currentFrame);
    void setVelocityBuffer(VkImageView velocity);
    void setDepthBuffer(VkImageView depth);
    void updateDescriptorSets();

    void setBlendFactor(float factor) { m_blendFactor = factor; }
    void setMotionScale(float scale) { m_motionScale = scale; }
    void setUseVarianceClipping(bool enable) { m_useVarianceClipping = enable; }

    // Get output
    VkImageView getOutputView() const { return m_historyViews[m_currentHistoryIndex]; }

    // Get jitter for current frame
    glm::vec2 getJitterOffset(uint32_t frameIndex) const;

private:
    bool createHistoryBuffers();
    bool createRenderPass();
    bool createPipeline();
    bool createDescriptors();
    void destroyHistoryBuffers();

    void swapHistoryBuffers();

    // Current frame input
    VkImageView m_currentFrameView{VK_NULL_HANDLE};
    VkImageView m_velocityView{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};

    // Double-buffered history
    static constexpr uint32_t HISTORY_COUNT = 2;
    std::array<VkImage, HISTORY_COUNT> m_historyImages{};
    std::array<VkDeviceMemory, HISTORY_COUNT> m_historyMemory{};
    std::array<VkImageView, HISTORY_COUNT> m_historyViews{};
    std::array<VkFramebuffer, HISTORY_COUNT> m_framebuffers{};
    uint32_t m_currentHistoryIndex{0};

    // Render pass and pipeline
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, HISTORY_COUNT> m_descriptorSets{};

    // Sampler
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Parameters
    float m_blendFactor{0.9f};   // Higher = more history
    float m_motionScale{100.0f};
    bool m_useVarianceClipping{true};

    // Halton sequence for jitter
    static constexpr uint32_t JITTER_SAMPLES = 16;
    std::array<glm::vec2, JITTER_SAMPLES> m_jitterSequence;

    // Push constants
    struct TAAParams {
        glm::vec2 texelSize;
        float blendFactor;
        float motionScale;
        uint32_t flags; // Bit 0: use motion vectors, Bit 1: use variance clipping
        float padding[3];
    };
};

} // namespace ohao
