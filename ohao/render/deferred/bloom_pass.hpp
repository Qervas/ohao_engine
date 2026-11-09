#pragma once

#include "render_pass_base.hpp"
#include "render/shader_bindings/postprocess_bloom_downsample.hpp"
#include "render/shader_bindings/postprocess_bloom_upsample.hpp"
#include "render/frame/frame_resources.hpp"
#include <array>
#include <vector>

namespace ohao {

// Bloom (frontier F2 T4): the frame's energy convolved by a mip pyramid (Jimenez 2014): the frame
// into level 0 as it is (postprocess/bloom_input.slang), a 13-tap downsample chain (Karis's average
// on the first), a tent upsample chain that adds each level to the one above at half weight. The
// display pass mixes the result, divided by levelWeightSum(), into the frame at a small strength:
// energy moves, none is added.
class BloomPass : public RenderPassBase {
public:
    static constexpr uint32_t MAX_MIP_LEVELS = 8;

    BloomPass() = default;
    ~BloomPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "BloomPass"; }

    // Configuration
    void setInputImage(VkImageView hdrInput);
    void setFilterRadius(float radius) { m_filterRadius = radius; }
    // The weights the upsample chain gives the levels sum to this (1 + 1/2 + 1/4 + ...): level 0's
    // output divided by it is a normalised convolution of the frame.
    [[nodiscard]] float levelWeightSum() const;

    // Get output (for compositing)
    [[nodiscard]] VkImageView getBloomOutput() const { return m_mipViews[0]; }
    // False when execute() skipped (e.g. no input yet): the output holds nothing.
    [[nodiscard]] bool didExecute() const { return m_executed; }

private:
    [[nodiscard]] bool createMipChain();
    [[nodiscard]] bool createRenderPasses();
    [[nodiscard]] bool createPipelines();
    [[nodiscard]] bool createDescriptors();
    void destroyMipChain();

    void executeInput(VkCommandBuffer cmd, VkDescriptorSet inputSet);
    [[nodiscard]] bool createFramebuffers();   // one per mip; rebuilt with the mip chain
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
    VkRenderPass m_inputRenderPass{VK_NULL_HANDLE};
    VkRenderPass m_downsampleRenderPass{VK_NULL_HANDLE};
    VkRenderPass m_upsampleRenderPass{VK_NULL_HANDLE};

    // Pipelines
    VkPipeline m_inputPipeline{VK_NULL_HANDLE};
    VkPipeline m_downsamplePipeline{VK_NULL_HANDLE};
    VkPipeline m_upsamplePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_inputPipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_downsampleLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_upsampleLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_inputLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    // [i + 1] samples mip i; [0] is unused. The HDR input set is rewritten at record
    // time, so it gets one set per frame in flight instead.
    std::vector<VkDescriptorSet> m_descriptorSets;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_inputSets{};

    bool m_executed{false};   // execute() rendered the bloom chain this frame

    // Sampler
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Parameters
    float m_filterRadius{1.0f};

    // Push constants
    struct SampleParams {
        glm::vec2 texelSize;
        float filterRadius;
        float blendFactor;
    };
    // The upsample's params; the downsample reads blendFactor's slot as isFirstPass (> 0.5: the Karis average).
    OHAO_CHECK_PUSH(SampleParams, postprocess_bloom_upsample)
    static_assert(offsetof(shader::postprocess_bloom_downsample::DownsampleParams, isFirstPass) ==
                      offsetof(SampleParams, blendFactor) &&
                  sizeof(shader::postprocess_bloom_downsample::DownsampleParams) == sizeof(SampleParams));
};

} // namespace ohao
