#include "diff/grad/gradient_arena.hpp"

#include <cstring>

namespace ohao::diff {

GradientArena::~GradientArena() {
    // Backstop only: normal teardown is the explicit destroy(allocator) call.
    // This exists so early-return error paths (e.g. diff_gpu_probe.cpp) that
    // never reach an explicit destroy() don't leak the buffer.
    if (m_buffer.isValid() && m_allocator != nullptr) {
        m_allocator->destroyBuffer(m_buffer);
    }
}

bool GradientArena::build(GpuAllocator& allocator, const ArenaLayout& layout) {
    if (layout.totalBytes() == 0) return false;

    m_layout = layout;
    m_allocator = &allocator;
    // CpuToGpu + persistently mapped keeps readback simple for the scaffolding
    // stage. Stage 1 can move this to GpuOnly with a staging copy once the
    // per-iteration cost matters.
    m_buffer = allocator.createBuffer(
        static_cast<VkDeviceSize>(layout.totalBytes()),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        AllocationUsage::CpuToGpu,
        /*persistentlyMapped=*/true);

    return m_buffer.isValid();
}

void GradientArena::destroy(GpuAllocator& allocator) {
    if (m_buffer.isValid()) allocator.destroyBuffer(m_buffer);
    m_layout = ArenaLayout{};
    m_allocator = nullptr;
}

void GradientArena::zero(VkCommandBuffer cmd) {
    if (!m_buffer.isValid()) return;
    vkCmdFillBuffer(cmd, m_buffer.buffer, 0,
                    static_cast<VkDeviceSize>(m_layout.totalBytes()), 0u);

    // "One command, one barrier": the fill is a TRANSFER-stage write. Any
    // compute work that scatters gradients into this buffer afterwards must
    // not begin until the fill is visible, or (in Stage 1's per-iteration
    // loop) iteration N's zero races iteration N-1's scatter and silently
    // corrupts gradients -- no crash, just wrong numbers. Today this arena is
    // only ever zeroed once before any compute submit (see diff_gpu_probe),
    // so vkQueueWaitIdle between separate submits already provides ordering
    // and this barrier is currently a no-op in practice. It becomes load
    // bearing the moment zero() and a compute dispatch share a command buffer
    // or queue timeline, which is exactly Stage 1's pattern.
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = m_buffer.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

std::vector<float> GradientArena::readback(GpuAllocator& allocator,
                                           std::size_t blockIndex) {
    std::vector<float> out;
    if (!m_buffer.isValid()) return out;

    const ArenaBlock block = m_layout.block(blockIndex);
    if (block.sizeBytes == 0) return out;

    allocator.invalidateBuffer(m_buffer);
    const auto* base = static_cast<const std::byte*>(m_buffer.getMappedData());
    if (base == nullptr) return out;

    out.resize(block.sizeBytes / sizeof(float));
    std::memcpy(out.data(), base + block.offsetBytes, block.sizeBytes);
    return out;
}

}  // namespace ohao::diff
