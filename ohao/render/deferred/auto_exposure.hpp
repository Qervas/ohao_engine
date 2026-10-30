#pragma once

// Auto exposure (frontier F2 T2, ruling F-R5): a window's camera meters the frame it shows.
// shaders/postprocess/exposure.slang: a 128-bin histogram of log2 luminance with each bin's exact
// sum (integers, 1/16 EV), then one thread meters the mean between two percentiles to middle grey,
// less the compensation, and eases the camera's EV100 toward it. The EV100 stays on the GPU (the
// display pass reads it); a copy per frame slot is what render.stats reports. Captures never meter.

#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.h>

namespace ohao {

class AutoExposure {
public:
    struct Settings {
        float compensation{0.0f};   // EV: positive brightens
        float lowFraction{0.5f};    // the percentiles metered
        float highFraction{0.95f};
        float speedUp{3.0f};        // 1/s toward a brighter frame
        float speedDown{1.0f};      // 1/s toward a darker one
        float minLog2{-10.0f};      // the histogram's range in log2 luminance
        float rangeLog2{30.0f};
    };

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();

    // Meters `hdr` (SHADER_READ_ONLY, w x h, last written as a colour attachment) into the EV100
    // the display pass reads, `dt` seconds after the last frame; frame slot `slot`'s copy follows.
    void record(VkCommandBuffer cmd, uint32_t slot, VkImageView hdr, VkSampler sampler, uint32_t width,
                uint32_t height, float dt, const Settings& settings);
    // The next metered frame takes its target at once.
    void reset() noexcept { m_reset = true; }

    [[nodiscard]] VkBuffer stateBuffer() const noexcept { return m_state; }

    // The EV100 of the newest metered frame whose fence has signalled (`fenceSignalled(slot)`).
    template <class FenceSignalled>
    std::optional<float> latestEv100(FenceSignalled&& fenceSignalled) {
        std::optional<float> out;
        uint64_t newest = 0;
        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
            if (m_slotFrame[s] == 0 || m_slotFrame[s] <= newest || !fenceSignalled(s)) continue;
            const float* v = static_cast<const float*>(m_readback[s].mapped);
            if (!v || v[1] == 0.0f) continue;
            newest = m_slotFrame[s];
            out = v[0];
        }
        return out;
    }

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkBuffer m_bins{VK_NULL_HANDLE};
    VkDeviceMemory m_binsMemory{VK_NULL_HANDLE};
    VkBuffer m_state{VK_NULL_HANDLE};
    VkDeviceMemory m_stateMemory{VK_NULL_HANDLE};
    std::array<rtc::HostBuffer, MAX_FRAMES_IN_FLIGHT> m_readback{};
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_slotFrame{};   // which metered frame each copy holds
    uint64_t m_frames{0};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_histogram{VK_NULL_HANDLE};
    VkPipelineLayout m_adaptLayout{VK_NULL_HANDLE};
    VkPipeline m_adapt{VK_NULL_HANDLE};
    bool m_cleared{false};   // the bins and state were zeroed on the GPU
    bool m_reset{true};
};

}  // namespace ohao
