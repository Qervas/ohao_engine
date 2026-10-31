#pragma once

// RT GI (sub-project 2b spec, "RT GI" and "Reference quality"; every bounce since frontier F5a):
// diffuse indirect light for the lighting pass (binding 11), traced with inline ray queries
// (VK_KHR_ray_query; without it init() fails and the frame has no indirect light). Bounces past the
// first come from its radiance cache, which the trace feeds and render() resolves (rt/gi_trace.slang).
//
// realtime   render() traces half resolution, kGIRealtimeRays cosine rays a pixel
//            (rt/gi_trace.slang), and denoise() turns it into the full-resolution output: a
//            reprojected temporal accumulation clamped to its neighbourhood, two a-trous
//            iterations, a joint-bilateral upsample (rt/gi_denoise.slang). Timed as rt.gi and
//            rt.gi.denoise.
// reference  render() traces full resolution, kGIReferenceRays rays a pixel, and keeps the
//            running mean of the frames since the last temporal reset; denoise() does nothing.
//
// The output is the incoming radiance over the cosine lobe (rgb, the receiver's albedo not
// applied) and the environment's visibility (a, the share of the rays that left the scene):
// lighting adds rgb times the diffuse albedo and AO to the ambient term. Beside it the reflection
// (frontier F5b): one ray a pixel from the GGX lobe, denoised by the same chain (its a-trous spread by
// roughness), which lighting weighs by the specular albedo.

#include "render_technique.hpp"
#include "render/material/openpbr_tables_buffer.hpp"
#include "render/rt/radiance_cache.hpp"
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

    // The trace's sets 1 and 2 (rt/alpha_test.slang): rtc::AlphaTables' layout and the bindless
    // textures'. Before init (its pipeline needs them; without them init fails: no RT GI).
    void setAlphaLayouts(VkDescriptorSetLayout tables, VkDescriptorSetLayout textures) noexcept {
        m_alphaTablesLayout = tables;
        m_texturesLayout = textures;
    }
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
    // Forget the temporal history: the next realtime frame takes none, and the reference
    // running mean starts again.
    void resetHistory() noexcept {
        m_historyValid = false;
        m_refFrames = 0;
        m_cache.clear();   // a capture after a reset starts from an empty cache, so it repeats
    }
    // Realtime quality's trace (render.settings rtGI): half or full resolution (the denoiser
    // runs at the trace's size and the upsample becomes a copy), and its rays a pixel (1-32).
    // A resolution change idles the device and rebuilds the realtime images; call it between
    // frames.
    void setRealtime(bool fullResolution, uint32_t rays);
    [[nodiscard]] bool fullResolution() const noexcept { return m_shift == 0u; }
    void destroy() override;

private:
    using Image = rtc::Image;

    bool createScreenImages();
    void destroyScreenImages();
    bool ensureReferenceImage();
    bool createDescriptorResources();
    bool createPipelines();
    // Every half-resolution image of the realtime chain, created in GENERAL on first use
    // after init or a resize.
    void prepareHalfImages(VkCommandBuffer cmd);
    // One denoising chain: `chain` 0 the diffuse (into m_output), 1 the reflection (into m_specOutput).
    void denoiseChain(VkCommandBuffer cmd, uint32_t chain);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_halfWidth = 0, m_halfHeight = 0;

    // Realtime, half resolution, all kept in GENERAL: the trace's radiance (reused as the
    // second a-trous output), its guide, the two accumulations (ping-pong pairs: this frame's
    // and last frame's), the first a-trous output.
    Image m_noisy{}, m_atrous{};
    std::array<Image, 2> m_guide{}, m_fast{}, m_slow{};
    // The reflection's chain (frontier F5b): the same images, the guide shared.
    Image m_specNoisy{}, m_specAtrous{};
    std::array<Image, 2> m_specFast{}, m_specSlow{};
    bool m_halfReady = false;
    // Full resolution: the output lighting reads, and (reference, created on first use) the
    // running mean in 32-bit floats.
    Image m_output{}, m_refMean{};
    Image m_specOutput{}, m_specRefMean{};
    // 1x1 stand-ins bound where a quality does not use a binding, and for a missing
    // environment map.
    Image m_guideDummy{}, m_refDummy{}, m_envDummy{};
    VkSampler m_envSampler = VK_NULL_HANDLE;

    uint32_t m_cur = 0;            // this frame's half of each pair
    bool m_historyValid = false;   // the other half holds the previous realtime frame
    uint32_t m_refFrames = 0;      // the reference frames in m_refMean; 0 after a reset, a new image or a realtime frame

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
    uint32_t m_shift = 1;   // realtime: the trace is the frame >> m_shift (1 half, 0 full)
    uint32_t m_realtimeRays = gpu::shared::kGIRealtimeRays;

    // The bounce's lights, a buffer per frame in flight, grown to the frame's count (in GILights).
    std::array<rtc::HostBuffer, MAX_FRAMES_IN_FLIGHT> m_lights{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_lightCapacity{};
    render::OpenPBRTablesBuffer m_openpbrTables;   // the trace's binding 11
    // The bounces past the first (frontier F5a): the trace's bindings 12-14, resolved after it.
    render::RadianceCache m_cache;
    static constexpr uint32_t kCacheCapacity = 1u << 20;   // 1M cells, 36 MiB
    // A cell's history in samples: light moves one bounce a frame through it, and 256 (the path
    // tracer's) kept the first frames' fewer bounces in a cell for a hundred frames.
    static constexpr uint32_t kCacheMaxSamples = 8;

    VkPipeline m_tracePipeline = VK_NULL_HANDLE, m_temporalPipeline = VK_NULL_HANDLE;
    VkPipeline m_atrousPipeline = VK_NULL_HANDLE, m_upsamplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_traceLayout = VK_NULL_HANDLE, m_temporalLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_atrousLayout = VK_NULL_HANDLE, m_upsampleLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_traceSetLayout = VK_NULL_HANDLE, m_temporalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_alphaTablesLayout = VK_NULL_HANDLE, m_texturesLayout = VK_NULL_HANDLE;   // setAlphaLayouts
    VkDescriptorSetLayout m_atrousSetLayout = VK_NULL_HANDLE, m_upsampleSetLayout = VK_NULL_HANDLE;

    // One set of each per frame in flight (written at record time), the denoiser's per chain (0 the
    // diffuse, 1 the reflection); the a-trous pass has two a chain, one per iteration.
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_traceSets{};
    std::array<std::array<VkDescriptorSet, 2>, MAX_FRAMES_IN_FLIGHT> m_temporalSets{}, m_upsampleSets{};
    // The a-trous steps 1, 2, 4, 8: a flat wall's low-frequency noise is wider than two steps reach.
    static constexpr uint32_t kAtrousSteps = 4;
    std::array<std::array<std::array<VkDescriptorSet, kAtrousSteps>, 2>, MAX_FRAMES_IN_FLIGHT> m_atrousSets{};
};

static_assert(GITechniqueLike<RTGITechnique>);

} // namespace ohao
