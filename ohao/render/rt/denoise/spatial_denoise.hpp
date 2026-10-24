#pragma once
#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

#include "render/frame/frame_resources.hpp"   // MAX_FRAMES_IN_FLIGHT

namespace ohao {

/// The path tracer's internal spatial denoise (RTRenderSettings::enableInternalDenoise)
/// as a compute pass after the trace (shaders/rt/pt_spatial_denoise.comp): it reads the
/// frame's finished accumulation and normal AOV and writes the RGBA8 beauty. Inside the
/// raygen the same filter read neighbours other invocations were still writing.
class SpatialDenoise {
public:
    SpatialDenoise() = default;
    ~SpatialDenoise() { shutdown(); }
    SpatialDenoise(const SpatialDenoise&) = delete;
    SpatialDenoise& operator=(const SpatialDenoise&) = delete;

    bool initialize(VkDevice device);
    void shutdown();
    [[nodiscard]] bool ready() const { return m_pipeline != VK_NULL_HANDLE; }

    /// Records the pass. `frameSlot` is the frame-in-flight slot whose fence was just
    /// waited (its descriptor set is rewritten). All three images are in GENERAL; the
    /// caller orders the trace's writes before this dispatch and its write before readers.
    void dispatch(VkCommandBuffer cmd, uint32_t frameSlot, VkImageView accum, VkImageView normal,
                  VkImageView output, uint32_t width, uint32_t height);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
};

}  // namespace ohao
