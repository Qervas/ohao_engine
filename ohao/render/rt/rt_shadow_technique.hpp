#pragma once

// RT shadows (sub-project 2b spec, "RT shadows" and "Reference quality"): hardware ray traced
// visibility of the frame's shadow-casting lights, one channel a light, for the lighting pass.
// Both passes are compute: the trace uses inline ray queries (VK_KHR_ray_query; without it
// init() fails and CSM shadows the frame).
//
// realtime   render() traces one ray to each of the pixel's most important lights (at most
//            ShadowInput::maxLightsPerPixel) into a packed guide/bits image, and filter()
//            denoises it: a 5x5 depth- and normal-aware spatial filter, then a reprojected
//            temporal accumulation (rt/shadow_trace.slang, rt/shadow_filter.slang). Two
//            passes, timed as rt.shadows and rt.shadows.filter.
// reference  render() traces every light with 8 rays and keeps the running mean of the frames
//            since the last temporal reset; filter() does nothing.
//
// The CSM pass is the fallback when this technique does not run.

#include "render_technique.hpp"
#include "rt_compute.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/shared_layout.hpp"
#include <array>
#include <optional>

namespace ohao {

class RTShadowTechnique : public IShadowTechnique {
public:
    RTShadowTechnique() = default;
    ~RTShadowTechnique() override;

    const char* getName() const override { return "RT Shadows"; }
    bool needsRT() const override { return true; }

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    // The trace (rt.shadows). Lights beyond kShadowMaxLights are ignored.
    void render(VkCommandBuffer cmd, const ShadowInput& input) override;
    // The realtime filter (rt.shadows.filter), after render() in the same frame; nothing in
    // reference quality or when render() did not trace.
    void filter(VkCommandBuffer cmd);
    [[nodiscard]] bool filters() const noexcept { return m_rendered && m_quality == RenderQuality::Realtime; }
    // The visibility lighting reads (RG32UI, eight 8-bit channels: x lights 0-3, y 4-7),
    // valid once this frame's render() (and filter()) ran.
    ShadowOutput getOutput() const override;
    // True when the last render() traced rays. False means it bailed (no TLAS, no lights, no
    // pipeline) and nothing was written this frame: don't sample the output.
    [[nodiscard]] bool rendered() const { return m_rendered; }
    [[nodiscard]] VkExtent2D extent() const { return {m_width, m_height}; }
    // Forget the temporal history: the next realtime frame takes none.
    void resetHistory() noexcept { m_historyValid = false; }
    void destroy() override;

    // Pixels of a finished frame that had more lights worth tracing than they may trace
    // (render.stats' shadowLightsDropped). Reads every slot whose frame has finished
    // (`fenceSignalled(slot)`); `latestDropped` is the newest frame read, if any.
    template <class FenceSignalled>
    void collect(FenceSignalled&& fenceSignalled) {
        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
            if (m_slots[s].pending && fenceSignalled(s)) collectSlot(s);
    }
    struct Dropped {
        uint64_t frame{0};
        uint32_t pixels{0};
    };
    [[nodiscard]] std::optional<Dropped> latestDropped() const noexcept { return m_latestDropped; }

private:
    using Image = rtc::Image;
    // A frame slot's host-visible buffer: the lights (kShadowMaxLights ShadowLights), then
    // the dropped-pixel counter at kCounterOffset.
    struct Slot {
        rtc::HostBuffer buf{};
        bool pending{false};   // a submitted realtime frame whose counter is not read yet
        uint64_t frame{0};
    };
    static constexpr VkDeviceSize kLightBytes = sizeof(gpu::shared::ShadowLight) * gpu::shared::kShadowMaxLights;
    static constexpr VkDeviceSize kCounterOffset = 512;   // a multiple of any minStorageBufferOffsetAlignment
    static_assert(kLightBytes <= kCounterOffset);

    bool createImage(Image& img, VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height);
    void destroyImage(Image& img);
    bool createScreenImages();
    void destroyScreenImages();
    bool ensureReferenceImages();
    bool createSlots();
    bool createTracePipeline();
    bool createFilterPipeline();
    bool createDescriptorResources();
    void collectSlot(uint32_t slot);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // The trace's guide/bits image (realtime) and the visibility lighting reads (eight 8-bit
    // channels in RG32UI), each a ping-pong pair: this frame's and last frame's.
    std::array<Image, 2> m_meta{};
    std::array<Image, 2> m_vis{};
    // Reference: the running means in 32-bit floats (created on first use), and a 1x1
    // stand-in bound in their place in realtime.
    Image m_refLo{}, m_refHi{}, m_refDummy{};
    uint32_t m_cur = 0;            // this frame's half of each pair
    bool m_historyValid = false;   // the other half holds the previous frame's realtime output
    uint32_t m_historyFrames = 0;  // the frames that output averages (the filter's blend weight)

    // This frame (render() to filter()).
    bool m_rendered = false;
    RenderQuality m_quality = RenderQuality::Realtime;
    uint32_t m_lightCount = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_renderSeed = 0;
    uint32_t m_slot = 0;
    VkImageView m_velocityView = VK_NULL_HANDLE;

    std::array<Slot, MAX_FRAMES_IN_FLIGHT> m_slots{};
    std::optional<Dropped> m_latestDropped;

    // Trace pipeline (compute, inline ray queries)
    VkPipeline m_tracePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_tracePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_traceSetLayout = VK_NULL_HANDLE;

    // Filter pipeline (compute)
    VkPipeline m_filterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_filterPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_filterSetLayout = VK_NULL_HANDLE;

    // One set of each per frame in flight: they are written at record time.
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_traceSets{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_filterSets{};
};

// Compile-time check: concrete technique still models the documented concept.
static_assert(ShadowTechniqueLike<RTShadowTechnique>);

} // namespace ohao
