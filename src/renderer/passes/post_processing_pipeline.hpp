#pragma once

#include "render_pass_base.hpp"
#include "bloom_pass.hpp"
#include "taa_pass.hpp"
#include "ssao_pass.hpp"
#include "ssr_pass.hpp"
#include "volumetric_pass.hpp"
#include "motion_blur_pass.hpp"
#include "dof_pass.hpp"
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
    void setDepthBuffer(VkImageView depth);
    void setNormalBuffer(VkImageView normal);
    void setVelocityBuffer(VkImageView velocity);

    // Feature toggles
    void setBloomEnabled(bool enabled) { m_bloomEnabled = enabled; }
    void setTAAEnabled(bool enabled) { m_taaEnabled = enabled; }
    void setSSAOEnabled(bool enabled) { m_ssaoEnabled = enabled; }
    void setSSREnabled(bool enabled) { m_ssrEnabled = enabled; }
    void setVolumetricsEnabled(bool enabled) { m_volumetricsEnabled = enabled; }
    void setTonemappingEnabled(bool enabled) { m_tonemappingEnabled = enabled; }
    void setMotionBlurEnabled(bool enabled) { m_motionBlurEnabled = enabled; }
    void setDoFEnabled(bool enabled) { m_dofEnabled = enabled; }

    // Tonemapping configuration
    void setTonemapOperator(TonemapOperator op) { m_tonemapOp = op; }
    void setExposure(float exposure) { m_exposure = exposure; }
    void setGamma(float gamma) { m_gamma = gamma; }

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

    // SSR configuration
    void setSSRMaxDistance(float dist);
    void setSSRThickness(float thickness);
    void setColorBuffer(VkImageView color);  // For SSR reflection sampling
    void setSSRMatrices(const glm::mat4& view, const glm::mat4& proj,
                        const glm::mat4& invView, const glm::mat4& invProj);

    // Volumetric configuration
    void setVolumetricDensity(float density);
    void setVolumetricScattering(float g);
    void setFogColor(const glm::vec3& color);
    void setFogHeight(float height);
    void setLightBuffer(VkBuffer lightBuffer);
    void setShadowMap(VkImageView shadow, VkSampler shadowSampler);
    void setVolumetricMatrices(const glm::mat4& view, const glm::mat4& proj,
                                const glm::mat4& invView, const glm::mat4& invProj);

    // Motion blur configuration
    void setMotionBlurIntensity(float intensity);
    void setMotionBlurSamples(uint32_t samples);
    void setMotionBlurVelocityScale(float scale);

    // DoF configuration
    void setDoFFocusDistance(float distance);
    void setDoFAperture(float fStop);
    void setDoFMaxBlurRadius(float pixels);
    void setDoFNearRange(float start, float end);
    void setDoFFarRange(float start, float end);

    // Get final output
    VkImageView getOutputView() const { return m_finalOutputView; }
    VkImage getOutputImage() const { return m_finalOutput; }
    VkImageView getSSAOOutput() const;
    VkImageView getSSROutput() const;
    VkImageView getVolumetricOutput() const;
    VkImageView getMotionBlurOutput() const;
    VkImageView getDoFOutput() const;

private:
    bool createTonemappingPass();
    bool createFinalOutput();
    void destroyFinalOutput();
    void updateTonemapDescriptors();

    // Sub-passes
    std::unique_ptr<BloomPass> m_bloomPass;
    std::unique_ptr<TAAPass> m_taaPass;
    std::unique_ptr<SSAOPass> m_ssaoPass;
    std::unique_ptr<SSRPass> m_ssrPass;
    std::unique_ptr<VolumetricPass> m_volumetricPass;
    std::unique_ptr<MotionBlurPass> m_motionBlurPass;
    std::unique_ptr<DoFPass> m_dofPass;

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
    bool m_ssrEnabled{false};
    bool m_volumetricsEnabled{false};
    bool m_motionBlurEnabled{false};
    bool m_dofEnabled{false};
    bool m_tonemappingEnabled{true};

    // Color buffer for SSR
    VkImageView m_colorBufferView{VK_NULL_HANDLE};

    // Tonemapping parameters
    TonemapOperator m_tonemapOp{TonemapOperator::ACES};
    float m_exposure{1.0f};
    float m_gamma{2.2f};

    // Push constants for tonemapping
    struct TonemapParams {
        float exposure;
        float gamma;
        uint32_t tonemapOp;
        uint32_t flags; // Bit 0: use bloom
    };
};

} // namespace ohao
