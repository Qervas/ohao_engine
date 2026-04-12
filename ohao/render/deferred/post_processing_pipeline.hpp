#pragma once

#include "render_pass_base.hpp"
#include "bloom_pass.hpp"
#include "taa_pass.hpp"
#include "ssao_pass.hpp"
#include <memory>

namespace ohao {

// Tonemapping operator enum
enum class TonemapOperator : uint32_t {
    ACES = 0,
    Reinhard = 1,
    Uncharted2 = 2,
    Neutral = 3
};

// Post-processing pipeline that orchestrates all post-processing passes
class PostProcessingPipeline : public RenderPassBase {
public:
    PostProcessingPipeline() = default;
    ~PostProcessingPipeline() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "PostProcessingPipeline"; }

    // Input configuration
    void setHDRInput(VkImageView hdrInput);
    void setHDRInputWithImage(VkImageView hdrInput, VkImage hdrImage);
    void setDepthBuffer(VkImageView depth);
    void setNormalBuffer(VkImageView normal);
    void setVelocityBuffer(VkImageView velocity);

    // Feature toggles
    void setBloomEnabled(bool enabled) { m_bloomEnabled = enabled; }
    void setTAAEnabled(bool enabled) { m_taaEnabled = enabled; }
    void setSSAOEnabled(bool enabled) { m_ssaoEnabled = enabled; }
    bool getSSAOEnabled() const { return m_ssaoEnabled; }
    void setTonemappingEnabled(bool enabled) { m_tonemappingEnabled = enabled; }

    // Tonemapping configuration
    void setTonemapOperator(TonemapOperator op) { m_tonemapOp = op; }
    void setExposure(float exposure) { m_exposure = exposure; }
    void setGamma(float gamma) { m_gamma = gamma; }
    // Lightning flash — set each frame (0=off, 3+=bright strike)
    void setFlashIntensity(float v) { m_flashIntensity = glm::max(v, 0.0f); }

    // Bloom configuration
    void setBloomThreshold(float threshold);
    void setBloomIntensity(float intensity);

    // TAA configuration
    void setTAABlendFactor(float factor);
    glm::vec2 getJitterOffset(uint32_t frameIndex) const;

    // SSAO configuration
    void setSSAORadius(float radius);
    void setSSAOIntensity(float intensity);
    void setProjectionMatrix(const glm::mat4& proj, const glm::mat4& invProj);

    void setDeltaTime(float dt)          { m_totalTime += dt; }

    // Execute SSAO separately (called before lighting pass by DeferredRenderer)
    void executeSSAO(VkCommandBuffer cmd, uint32_t frameIndex);

    // Get final output
    VkImageView getOutputView() const { return m_finalOutputView; }
    VkImage getOutputImage() const { return m_finalOutput; }
    bool didExecute() const { return m_didExecute; }
    VkImageView getSSAOOutput() const;
    VkImage getSSAOImage() const;
    VkSampler getSSAOSampler() const;

private:
    bool createTonemappingPass();
    bool createFinalOutput();
    void destroyFinalOutput();
    void updateTonemapDescriptors();
    void updateTonemapInput(VkImageView input);

    // Sub-passes
    std::unique_ptr<BloomPass> m_bloomPass;
    std::unique_ptr<TAAPass> m_taaPass;
    std::unique_ptr<SSAOPass> m_ssaoPass;

    // Tonemapping (final pass)
    VkRenderPass m_tonemapRenderPass{VK_NULL_HANDLE};
    VkPipeline m_tonemapPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_tonemapLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_tonemapDescLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_tonemapDescPool{VK_NULL_HANDLE};
    VkDescriptorSet m_tonemapDescSet{VK_NULL_HANDLE};
    VkFramebuffer m_tonemapFramebuffer{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Final output (LDR)
    VkImage m_finalOutput{VK_NULL_HANDLE};
    VkDeviceMemory m_finalMemory{VK_NULL_HANDLE};
    VkImageView m_finalOutputView{VK_NULL_HANDLE};

    // Input views
    VkImageView m_hdrInputView{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Feature toggles (disabled by default for stability)
    bool m_bloomEnabled{false};
    bool m_taaEnabled{false};
    bool m_ssaoEnabled{false};
    bool m_tonemappingEnabled{true};
    bool m_didExecute{false};

    float m_totalTime{0.0f};

    // Tonemapping parameters
    TonemapOperator m_tonemapOp{TonemapOperator::ACES};
    float m_exposure{1.0f};
    float m_gamma{2.2f};
    float m_flashIntensity{0.0f};

    // Push constants for tonemapping (must match GLSL layout in tonemapping.frag)
    struct TonemapParams {
        float exposure;
        float gamma;
        float bloomStrength;   // 0.0 = no bloom composite, 1.0+ = bloom intensity
        uint32_t tonemapOp;    // 0=ACES, 1=Reinhard, 2=Uncharted2, 3=Neutral
        float flashIntensity;  // lightning flash (0=none, 3+ = bright strike)
        float paddingF;
    };

};

} // namespace ohao
