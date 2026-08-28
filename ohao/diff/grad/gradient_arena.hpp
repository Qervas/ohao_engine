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
    GradientArena() = default;
    // The destructor has no GpuAllocator& to call destroy() with, so build()
    // stashes the allocator pointer and the destructor uses it as a backstop
    // if the buffer is still live. destroy() remains the normal, explicit
    // teardown path and stays idempotent so existing call sites are unaffected.
    ~GradientArena();

    // Not copyable or movable: the compiler-generated versions would copy the
    // raw GpuBuffer handle, leaving two objects that each believe they own
    // (and will each destroy) the same buffer/allocation.
    GradientArena(const GradientArena&) = delete;
    GradientArena& operator=(const GradientArena&) = delete;
    GradientArena(GradientArena&&) = delete;
    GradientArena& operator=(GradientArena&&) = delete;

    [[nodiscard]] bool build(GpuAllocator& allocator, const ArenaLayout& layout);
    void destroy(GpuAllocator& allocator);

    /// Records a single fill over the whole arena. One command, one barrier.
    void zero(VkCommandBuffer cmd);

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer.buffer; }
    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_layout.totalBytes(); }

    /// Host-visible copy of one block's floats. Caller must have waited for the
    /// GPU work that wrote it.
    [[nodiscard]] std::vector<float> readback(GpuAllocator& allocator,
                                              std::size_t blockIndex);

    /// Host-visible copy of EVERY float in the arena, including the inter-block
    /// alignment padding no block owns. The blocks are 256-byte aligned, so
    /// most of a small arena is padding -- and padding is precisely where a
    /// mis-computed element index lands, which is why a null test that reads
    /// only the blocks cannot see it. Same waiting requirement as readback().
    /// Returns an empty vector if the arena was never built.
    [[nodiscard]] std::vector<float> readbackAll(GpuAllocator& allocator);

private:
    ArenaLayout m_layout;
    GpuBuffer m_buffer;
    GpuAllocator* m_allocator{nullptr};
};

}  // namespace ohao::diff
