#pragma once

// RT GI (sub-project 2b spec, "RT GI" and "Reference quality"): one-bounce diffuse indirect
// light for the lighting pass (binding 11), traced with inline ray queries
// (VK_KHR_ray_query; without it init() fails and the frame has no indirect light).
//
// realtime   render() traces half resolution, kGIRealtimeRays cosine rays a pixel
//            (rt/gi_trace.slang), and denoise() turns it into the full-resolution output: a
//            reprojected temporal accumulation clamped to its neighbourhood, two a-trous
//            iterations, a joint-bilateral upsample (rt/gi_denoise.slang). Timed as rt.gi and
//            rt.gi.denoise.
// reference  render() traces full resolution, kGIReferenceRays rays a pixel, and keeps the
//            running mean of the frames since the last temporal reset; denoise() does nothing.
//
// The output is diffuse indirect light with the receiver's diffuse albedo applied:
// lighting adds it, times AO, to the ambient term.

#include "render_technique.hpp"
#include "rt_compute.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/shared_layout.hpp"
#include <array>

namespace ohao {

class RTGITechnique : public IGITechnique {
public:
    RTGITechnique() = default;
    ~RTGITechnique() override;

    const char* getName() const override { return "RT GI"; }
    bool needsRT() const override { return true; }

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    // The trace (rt.gi).
    void render(VkCommandBuffer cmd, const GIInput& input) override;
    // The realtime denoiser (rt.gi.denoise), after render() in the same frame; nothing in
    // reference quality or when render() did not trace.
    void denoise(VkCommandBuffer cmd);
    [[nodiscard]] bool denoises() const noexcept { return m_rendered && m_quality == RenderQuality::Realtime; }
    // The full-resolution indirect light lighting reads, valid once this frame's render()
    // (and denoise()) ran.
    GIOutput getOutput() const override;
    // True when the last render() traced rays. False means it bailed (no TLAS, no pipeline)
    // and nothing was written this frame: don't sample the output.
    [[nodiscard]] bool rendered() const { return m_rendered; }
    // What the last render() traced: its image's size (realtime: half the frame's, rounded
    // up; reference: the frame's) and its rays a pixel (test 10).
    [[nodiscard]] VkExtent2D traceExtent() const noexcept { return m_traceExtent; }
    [[nodiscard]] uint32_t raysPerPixel() const noexcept { return m_rays; }
    // Forget the temporal history: the next realtime frame takes none.
    void resetHistory() noexcept { m_historyValid = false; }
    void destroy() override;

private:
    using Image = rtc::Image;
    static constexpr VkDeviceSize kLightBytes = sizeof(gpu::shared::GILight) * gpu::shared::kGIMaxLights;

    bool createScreenImages();
    void destroyScreenImages();
    bool ensureReferenceImage();
    bool createDescriptorResources();
    bool createPipelines();
    // Every half-resolution image of the realtime chain, created in GENERAL on first use
    // after init or a resize.
    void prepareHalfImages(VkCommandBuffer cmd);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_halfWidth = 0, m_halfHeight = 0;

    // Realtime, half resolution, all kept in GENERAL: the trace's radiance (reused as the
    // second a-trous output), its guide, the two accumulations (ping-pong pairs: this frame's
    // and last frame's), the first a-trous output.
    Image m_noisy{}, m_atrous{};
    std::array<Image, 2> m_guide{}, m_fast{}, m_slow{};
    bool m_halfReady = false;
    // Full resolution: the output lighting reads, and (reference, created on first use) the
    // running mean in 32-bit floats.
    Image m_output{}, m_refMean{};
    // 1x1 stand-ins bound where a quality does not use a binding, and for a missing
    // environment map.
    Image m_guideDummy{}, m_refDummy{}, m_envDummy{};
    VkSampler m_envSampler = VK_NULL_HANDLE;

    uint32_t m_cur = 0;            // this frame's half of each pair
    bool m_historyValid = false;   // the other half holds the previous realtime frame

    // This frame (render() to denoise()).
    bool m_rendered = false;
    RenderQuality m_quality = RenderQuality::Realtime;
    uint32_t m_slot = 0;
    VkImageView m_velocityView = VK_NULL_HANDLE;
    VkImageView m_positionView = VK_NULL_HANDLE;
    VkImageView m_normalView = VK_NULL_HANDLE;
    VkImageView m_albedoView = VK_NULL_HANDLE;
    glm::vec3 m_cameraPos{0.0f};
    VkExtent2D m_traceExtent{0, 0};
    uint32_t m_rays = 0;

    std::array<rtc::HostBuffer, MAX_FRAMES_IN_FLIGHT> m_lights{};

    VkPipeline m_tracePipeline = VK_NULL_HANDLE, m_temporalPipeline = VK_NULL_HANDLE;
    VkPipeline m_atrousPipeline = VK_NULL_HANDLE, m_upsamplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_traceLayout = VK_NULL_HANDLE, m_temporalLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_atrousLayout = VK_NULL_HANDLE, m_upsampleLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_traceSetLayout = VK_NULL_HANDLE, m_temporalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_atrousSetLayout = VK_NULL_HANDLE, m_upsampleSetLayout = VK_NULL_HANDLE;

    // One set of each per frame in flight (written at record time); the a-trous pass has two
    // a frame, one per iteration.
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_traceSets{}, m_temporalSets{}, m_upsampleSets{};
    std::array<std::array<VkDescriptorSet, 2>, MAX_FRAMES_IN_FLIGHT> m_atrousSets{};
};

static_assert(GITechniqueLike<RTGITechnique>);

} // namespace ohao
