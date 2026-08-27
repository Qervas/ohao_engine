#include "diff/grad/gradient_arena.hpp"

#include <cstring>

namespace ohao::diff {

bool GradientArena::build(GpuAllocator& allocator, const ArenaLayout& layout) {
    if (layout.totalBytes() == 0) return false;

    m_layout = layout;
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
}

void GradientArena::zero(VkCommandBuffer cmd) const {
    if (!m_buffer.isValid()) return;
    vkCmdFillBuffer(cmd, m_buffer.buffer, 0,
                    static_cast<VkDeviceSize>(m_layout.totalBytes()), 0u);
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
