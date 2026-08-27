#pragma once

#include "diff/wavefront/path_state_layout.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <cstdint>
#include <vector>

namespace ohao::diff {

/// The three buffers a wavefront bounce dispatch reads and writes.
///
/// Path state cannot live in registers across a dispatch boundary (each
/// bounce is a separate `vkCmdDispatch`), so it lives in `state`. The set of
/// paths still alive going into a bounce is handed off via `queue`, and dead
/// paths are compacted out so the next dispatch does not pay for them --
/// `counter` holds the live counts that drive that compaction and (in a
/// later task) an indirect dispatch group count.
///
/// Ownership mirrors GradientArena: build()/destroy() are the explicit
/// lifecycle, the destructor is a backstop for early-return error paths, and
/// copy/move are deleted because the compiler-generated versions would copy
/// the raw GpuBuffer handles, leaving two objects that each believe they own
/// (and will each destroy) the same buffers.
class WavefrontBuffers {
public:
    WavefrontBuffers() = default;
    ~WavefrontBuffers();

    WavefrontBuffers(const WavefrontBuffers&) = delete;
    WavefrontBuffers& operator=(const WavefrontBuffers&) = delete;
    WavefrontBuffers(WavefrontBuffers&&) = delete;
    WavefrontBuffers& operator=(WavefrontBuffers&&) = delete;

    /// Number of uint slots reserved in the counter buffer. Slots 0 and 1
    /// are the current/next queue live-path counts; the remainder is
    /// reserved for a later task to write a {groupCountX, groupCountY,
    /// groupCountZ} vkCmdDispatchIndirect triple without reallocating.
    static constexpr std::uint32_t kCounterSlotCount = 8;
    static constexpr std::uint32_t kCurrentCountSlot = 0;
    static constexpr std::uint32_t kNextCountSlot = 1;

    [[nodiscard]] bool build(GpuAllocator& allocator, std::uint32_t capacity);
    void destroy(GpuAllocator& allocator);

    /// Records a fill over all three buffers plus a
    /// TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier, mirroring
    /// GradientArena::zero.
    void zero(VkCommandBuffer cmd);

    [[nodiscard]] VkBuffer stateBuffer() const noexcept { return m_stateBuffer.buffer; }
    [[nodiscard]] VkBuffer queueBuffer() const noexcept { return m_queueBuffer.buffer; }
    [[nodiscard]] VkBuffer counterBuffer() const noexcept { return m_counterBuffer.buffer; }

    [[nodiscard]] const PathStateLayout& layout() const noexcept { return m_layout; }

    /// Host-visible copy of one field's floats (exactly `layout().capacity()`
    /// elements). Caller must have waited for the GPU work that wrote it.
    [[nodiscard]] std::vector<float> readbackField(GpuAllocator& allocator, PathStateField field);

    /// Host-visible copy of one counter slot. Returns 0 for an out-of-range
    /// slot (there are kCounterSlotCount of them).
    [[nodiscard]] std::uint32_t readbackCounter(GpuAllocator& allocator, std::uint32_t slot);

private:
    PathStateLayout m_layout{0};
    GpuBuffer m_stateBuffer;
    GpuBuffer m_queueBuffer;
    GpuBuffer m_counterBuffer;
    GpuAllocator* m_allocator{nullptr};
};

}  // namespace ohao::diff
