#pragma once
// The barriers between what last touched a resource (gpu::State, kept on the resource) and its next use: the one
// derivation every barrier in the engine comes from (system design 4.6, step 4a). render::Pass (render/graph/pass.hpp)
// states a frame's uses through it; code under gpu/vulkan that records its own work (the texture table's uploads)
// states its own. One vkCmdPipelineBarrier2 per batch.
#include "gpu/vulkan/gpu_image.hpp"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace ohao::gpu {

class Barriers {
public:
    // A use of one subresource of `image` (its state `s`): a write waits for every access since the last write, a
    // read for the last write. `discard`: the old contents are not wanted (from UNDEFINED). `s` becomes the state
    // after the use.
    void image(VkImage image, VkImageAspectFlags aspect, State& s, uint32_t mip, uint32_t layer, VkImageLayout layout,
               VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write, bool discard);
    // A use of a buffer (its state `s`).
    void buffer(State& s, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write);

    [[nodiscard]] bool empty() const noexcept { return m_images.empty() && m_memory.dstStageMask == 0; }
    // Records what was stated since the last record, in one call, and starts a new batch.
    void record(VkCommandBuffer cmd);

private:
    // A dependency that changes no layout (a buffer's, or an image's in the layout it is in): joined into the
    // batch's one global memory barrier, which the driver takes as one dependency rather than one per resource.
    void memory(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                VkAccessFlags2 dstAccess);

    std::vector<VkImageMemoryBarrier2> m_images;   // the layout changes
    VkMemoryBarrier2 m_memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
};

} // namespace ohao::gpu
