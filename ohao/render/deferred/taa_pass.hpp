#pragma once

#include "render_pass_base.hpp"
#include "render/frame/frame_resources.hpp"
#include <array>

namespace ohao {

// Temporal Anti-Aliasing pass
class TAAPass : public RenderPassBase {
public:
    TAAPass() = default;
    ~TAAPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "TAAPass"; }

    // Configuration
    void setCurrentFrame(VkImageView currentFrame);
    void setVelocityBuffer(VkImageView velocity);
    void setDepthBuffer(VkImageView depth);
    // Both G-buffer inputs at once, one descriptor rewrite.
    void setGBufferInputs(VkImageView depth, VkImageView velocity);
    // The transparent pass's coverage (R8, SHADER_READ_ONLY when TAA runs): history weighs less
    // where it is set (sub-project 12, OPEN ruling 15). Null: none bound.
    void setTransparencyMask(VkImageView mask);
    [[nodiscard]] VkImageView depthInput() const noexcept { return m_depthView; }
    [[nodiscard]] VkImageView velocityInput() const noexcept { return m_velocityView; }

    void setBlendFactor(float factor) { m_blendFactor = factor; }
    void setMotionScale(float scale) { m_motionScale = scale; }
    void setUseVarianceClipping(bool enable) { m_useVarianceClipping = enable; }

    // The image execute() just wrote (it becomes next frame's history).
    [[nodiscard]] VkImageView getOutputView() const { return m_historyViews[m_currentHistoryIndex]; }

    // Get jitter for current frame
    [[nodiscard]] glm::vec2 getJitterOffset(uint32_t frameIndex) const;

    // Forget the history, as on a fresh pass: the next execute() clears both images in its
    // own command buffer, after earlier frames' accesses by queue order. No idle needed.
    void resetHistory() noexcept {
        m_historyValid = false;
        m_currentHistoryIndex = 0;
    }

private:
    [[nodiscard]] bool createHistoryBuffers();
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createDescriptors();
    void destroyHistoryBuffers();
    // Zero both history images and leave them SHADER_READ_ONLY. Run on the first
    // frame after (re)creation, when the image about to be sampled was never written.
    void clearHistory(VkCommandBuffer cmd);
    // Writes this frame slot's set: current frame, history[historyIdx], velocity, depth.
    void writeDescriptorSet(uint32_t slot, uint32_t historyIdx);

    // Current frame input
    VkImageView m_currentFrameView{VK_NULL_HANDLE};
    VkImageView m_velocityView{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImageView m_maskView{VK_NULL_HANDLE};

    // Double-buffered history
    static constexpr uint32_t HISTORY_COUNT = 2;
    std::array<VkImage, HISTORY_COUNT> m_historyImages{};
    std::array<VkDeviceMemory, HISTORY_COUNT> m_historyMemory{};
    std::array<VkImageView, HISTORY_COUNT> m_historyViews{};
    std::array<VkFramebuffer, HISTORY_COUNT> m_framebuffers{};
    uint32_t m_currentHistoryIndex{0};   // index of the image written most recently
    bool m_historyValid{false};          // false until a frame has been resolved into history

    // Render pass and pipeline
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    // One set per frame in flight, written at record time for that frame's slot.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};

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
