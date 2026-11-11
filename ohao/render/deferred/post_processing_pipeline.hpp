#pragma once

#include "render_pass_base.hpp"
#include "render/shader_bindings/postprocess_tonemap.hpp"
#include "bloom_pass.hpp"
#include "taa_pass.hpp"
#include "auto_exposure.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/display/display.hpp"
#include "gpu/vulkan/gpu_image.hpp"
#include "render/graph/pass.hpp"
#include <array>
#include <memory>

namespace ohao {

// Post-processing pipeline that orchestrates all post-processing passes
class PostProcessingPipeline : public RenderPassBase {
public:
    PostProcessingPipeline() = default;
    ~PostProcessingPipeline() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override { record(render::Frame{cmd, nullptr, frameIndex}); }
    // Bloom, TAA, auto exposure and the display pass, timed as post.bloom, post.taa, exposure and post.tonemap.
    void record(const render::Frame& frame);
    void onResize(uint32_t width, uint32_t height) override;   // the output's size
    // The lit image's size when the frame is rendered smaller than the output (render.settings renderScale):
    // bloom runs at it and TAA upscales from it. Called before onResize; equal sizes are plain TAA.
    void setRenderSize(uint32_t width, uint32_t height);
    // The jitter the frame about to be resolved was rendered with (getJitterOffset's).
    void setFrameJitter(glm::vec2 ndc);
    [[nodiscard]] const char* getName() const override { return "PostProcessingPipeline"; }

    // Input configuration
    // The lit image the stack starts from.
    void setHDRInput(gpu::Image* hdrInput) { m_hdrInput = hdrInput; }
    // The G-buffer's depth and velocity, which TAA reprojects with. Call it as soon as the G-buffer is recreated.
    void setGBufferInputs(gpu::Image* depth, gpu::Image* velocity);
    // The transparent pass's coverage mask, which TAA's history weighs less under (null: none).
    // LEGACY(step 4a): the mask is VfxRenderer's, a view until that image is tracked.
    void setTransparencyMask(VkImageView mask);
    // Bloom's share of the frame's energy (frontier F2 T4): the display shows (1 - s) frame + s bloom.
    static constexpr float kBloomStrength = 0.04f;

    // Feature toggles
    void setBloomEnabled(bool enabled) { m_bloomEnabled = enabled; }
    void setTAAEnabled(bool enabled) { m_taaEnabled = enabled; }
    [[nodiscard]] bool getBloomEnabled() const { return m_bloomEnabled; }
    [[nodiscard]] bool getTAAEnabled() const { return m_taaEnabled; }
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

    void setDeltaTime(float dt)          { m_totalTime += dt; }

    // The final image (RGBA8, display encoded); it holds this frame once didExecute().
    [[nodiscard]] gpu::Image& output() { return m_output; }
    [[nodiscard]] bool didExecute() const { return m_didExecute; }

private:
    [[nodiscard]] bool createTonemappingPass();
    [[nodiscard]] bool createFinalOutput();
    void destroyFinalOutput();
    // Writes both tonemap bindings into this frame slot's set.
    void writeTonemapSet(uint32_t slot, VkImageView input, VkImageView bloom);

    // Sub-passes
    std::unique_ptr<BloomPass> m_bloomPass;
    std::unique_ptr<TAAPass> m_taaPass;
    std::unique_ptr<AutoExposure> m_autoExposure;

    // Tonemapping (final pass)
    VkPipeline m_tonemapPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_tonemapLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_tonemapDescLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_tonemapDescPool{VK_NULL_HANDLE};
    // One set per frame in flight: the input alternates with TAA's ping-pong every
    // frame, and a single set would be rewritten while the last frame still reads it.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_tonemapDescSets{};
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Final output (LDR)
    gpu::Image m_output;   // post.output

    // Input views
    gpu::Image* m_hdrInput{nullptr};

    // Dimensions
    uint32_t m_renderWidth{0};   // setRenderSize; 0: the output's
    uint32_t m_renderHeight{0};
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Feature toggles (disabled by default for stability)
    bool m_bloomEnabled{false};
    bool m_taaEnabled{false};
    bool m_didExecute{false};

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
    OHAO_CHECK_PUSH(TonemapParams, postprocess_tonemap)

};

} // namespace ohao
