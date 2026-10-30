#pragma once

#include "render_pass_base.hpp"
#include "bloom_pass.hpp"
#include "taa_pass.hpp"
#include "ssao_pass.hpp"
#include "auto_exposure.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/display/display.hpp"
#include <array>
#include <memory>

namespace ohao::render { class GpuPassTimer; }

namespace ohao {

// Post-processing pipeline that orchestrates all post-processing passes
class PostProcessingPipeline : public RenderPassBase {
public:
    PostProcessingPipeline() = default;
    ~PostProcessingPipeline() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "PostProcessingPipeline"; }

    // Input configuration
    void setHDRInput(VkImageView hdrInput);
    void setHDRInputWithImage(VkImageView hdrInput, VkImage hdrImage);
    void setDepthBuffer(VkImageView depth);
    void setNormalBuffer(VkImageView normal);
    void setVelocityBuffer(VkImageView velocity);
    // All G-buffer-derived inputs in one step. Call it as soon as the G-buffer is
    // recreated and before onResize(): every pass that rewrites a descriptor set
    // must already hold the new views when it does.
    void setGBufferInputs(VkImageView depth, VkImageView normal, VkImageView velocity);
    // The transparent pass's coverage mask, which TAA's history weighs less under (null: none).
    void setTransparencyMask(VkImageView mask);

    // The G-buffer views TAA currently references (null where the pass is absent).
    struct BoundGBufferInputs {
        VkImageView taaDepth{VK_NULL_HANDLE}, taaVelocity{VK_NULL_HANDLE};
    };
    [[nodiscard]] BoundGBufferInputs boundGBufferInputs() const;
    // Times bloom, TAA and tonemapping as post.bloom, post.taa, post.tonemap (null: untimed).
    void setPassTimer(render::GpuPassTimer* timer) noexcept { m_passTimer = timer; }

    // Bloom's share of the frame's energy (frontier F2 T4): the display shows (1 - s) frame + s bloom.
    static constexpr float kBloomStrength = 0.04f;

    // Feature toggles
    void setBloomEnabled(bool enabled) { m_bloomEnabled = enabled; }
    void setTAAEnabled(bool enabled) { m_taaEnabled = enabled; }
    void setSSAOEnabled(bool enabled) { m_ssaoEnabled = enabled; }
    [[nodiscard]] bool getSSAOEnabled() const { return m_ssaoEnabled; }
    [[nodiscard]] bool getBloomEnabled() const { return m_bloomEnabled; }
    [[nodiscard]] bool getTAAEnabled() const { return m_taaEnabled; }
    void setTonemappingEnabled(bool enabled) { m_tonemappingEnabled = enabled; }
    // Forget TAA's history, as on a fresh pipeline (DeferredRenderer::resetTemporalState).
    void resetHistory() noexcept;

    // The display transform (shaders/display/display.slang): the camera's EV100 and the curve. The
    // path tracer's frames read them from here too (VulkanRenderer), so every path displays alike.
    void setEv100(float ev100) { m_ev100 = ev100; }
    [[nodiscard]] float ev100() const { return m_ev100; }
    void setDisplayCurve(render::DisplayCurve curve) { m_displayCurve = curve; }
    [[nodiscard]] render::DisplayCurve displayCurve() const { return m_displayCurve; }
    // Auto exposure (a window's): the frame metered (AutoExposure) in place of the fixed EV100.
    void setAutoExposure(bool on) {
        if (on && !m_autoExposureEnabled && m_autoExposure) m_autoExposure->reset();
        m_autoExposureEnabled = on;
    }
    [[nodiscard]] bool autoExposure() const { return m_autoExposureEnabled; }
    void setExposureCompensation(float ev) { m_autoExposureSettings.compensation = ev; }
    [[nodiscard]] float exposureCompensation() const { return m_autoExposureSettings.compensation; }
    // Seconds since the last frame, for the adaptation's speed (a window's frame time).
    void setFrameDeltaTime(float dt) { m_frameDt = dt > 0.0f ? dt : m_frameDt; }
    // The newest finished frame's metered EV100 (`fenceSignalled(slot)`), if auto exposure ran.
    template <class FenceSignalled>
    [[nodiscard]] std::optional<float> meteredEv100(FenceSignalled&& fenceSignalled) {
        if (!m_autoExposure) return std::nullopt;
        return m_autoExposure->latestEv100(fenceSignalled);
    }

    // TAA configuration
    void setTAABlendFactor(float factor);
    [[nodiscard]] glm::vec2 getJitterOffset(uint32_t frameIndex) const;

    // SSAO configuration
    void setSSAORadius(float radius);
    void setSSAOIntensity(float intensity);
    // SSAO's per-frame inputs: the G-buffer's position and normal, the unjittered
    // view-projection and the camera position (DeferredRenderer::render).
    void setSSAOInputs(VkImageView position, VkImageView normal, const glm::mat4& viewProj, const glm::vec3& cameraPos);
    // The SSAO noise seed (VulkanRenderer::setRenderSeed); kept across re-initialization.
    void setNoiseSeed(uint32_t seed);
    // The seed the SSAO pass's noise comes from.
    [[nodiscard]] uint32_t noiseSeed() const noexcept { return m_ssaoPass ? m_ssaoPass->noiseSeed() : m_noiseSeed; }

    void setDeltaTime(float dt)          { m_totalTime += dt; }

    // Execute SSAO separately (called before lighting pass by DeferredRenderer)
    void executeSSAO(VkCommandBuffer cmd, uint32_t frameIndex);

    // Get final output
    [[nodiscard]] VkImageView getOutputView() const { return m_finalOutputView; }
    [[nodiscard]] VkImage getOutputImage() const { return m_finalOutput; }
    [[nodiscard]] bool didExecute() const { return m_didExecute; }
    [[nodiscard]] VkImageView getSSAOOutput() const;
    [[nodiscard]] VkImage getSSAOImage() const;
    [[nodiscard]] VkSampler getSSAOSampler() const;
    [[nodiscard]] SSAOPass* ssaoPass() noexcept { return m_ssaoPass.get(); }

private:
    [[nodiscard]] bool createTonemappingPass();
    [[nodiscard]] bool createFinalOutput();
    void destroyFinalOutput();
    // Writes both tonemap bindings into this frame slot's set.
    void writeTonemapSet(uint32_t slot, VkImageView input, VkImageView bloom);

    // Sub-passes
    std::unique_ptr<BloomPass> m_bloomPass;
    std::unique_ptr<TAAPass> m_taaPass;
    std::unique_ptr<SSAOPass> m_ssaoPass;
    std::unique_ptr<AutoExposure> m_autoExposure;
    uint32_t m_noiseSeed{0};

    // Tonemapping (final pass)
    VkRenderPass m_tonemapRenderPass{VK_NULL_HANDLE};
    VkPipeline m_tonemapPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_tonemapLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_tonemapDescLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_tonemapDescPool{VK_NULL_HANDLE};
    // One set per frame in flight: the input alternates with TAA's ping-pong every
    // frame, and a single set would be rewritten while the last frame still reads it.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_tonemapDescSets{};
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
    render::GpuPassTimer* m_passTimer{nullptr};

    float m_totalTime{0.0f};

    // The display transform's settings.
    float m_ev100{render::kDefaultEv100};
    render::DisplayCurve m_displayCurve{render::DisplayCurve::AgX};
    bool m_autoExposureEnabled{false};
    AutoExposure::Settings m_autoExposureSettings;
    float m_frameDt{1.0f / 60.0f};

    // Push constants for the display pass (postprocess/tonemap.slang's DisplayParams).
    struct TonemapParams {
        float exposure;        // linear: displayExposure(EV100)
        float bloomWeight;     // bloom's level 0 x this (the strength over its weight sum); 0: none
        uint32_t curve;        // render::DisplayCurve
        uint32_t autoExposure; // 1: the metered EV100
        float frameWeight;     // the frame x this: 1 - the strength (energy moves, none is added)
    };

};

} // namespace ohao
