#pragma once

#include "diff/grad/arena_layout.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <cstddef>
#include <vector>

namespace ohao::diff {

/// One VkBuffer holding every gradient and optimizer-state block.
///
/// Buffer-backed, never image-backed: the scatter path needs
/// shaderBufferFloat32AtomicAdd only, which is materially more available than
/// the image equivalent (design doc S4.3).
class GradientArena {
public:
    [[nodiscard]] bool build(GpuAllocator& allocator, const ArenaLayout& layout);
    void destroy(GpuAllocator& allocator);

    /// Records a single fill over the whole arena. One command, one barrier.
    void zero(VkCommandBuffer cmd) const;

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer.buffer; }
    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_layout.totalBytes(); }

    /// Host-visible copy of one block's floats. Caller must have waited for the
    /// GPU work that wrote it.
    [[nodiscard]] std::vector<float> readback(GpuAllocator& allocator,
                                              std::size_t blockIndex);

private:
    ArenaLayout m_layout;
    GpuBuffer m_buffer;
};

}  // namespace ohao::diff
