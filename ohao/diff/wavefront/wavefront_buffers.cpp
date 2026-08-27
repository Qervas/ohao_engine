#include "diff/wavefront/wavefront_buffers.hpp"

#include <cstring>

namespace ohao::diff {

namespace {
constexpr VkBufferUsageFlags kCommonUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}  // namespace

WavefrontBuffers::~WavefrontBuffers() {
    // Backstop only: normal teardown is the explicit destroy(allocator) call.
    // This exists so early-return error paths (mirroring GradientArena) that
    // never reach an explicit destroy() don't leak the buffers.
    if (m_allocator != nullptr) {
        if (m_stateBuffer.isValid()) m_allocator->destroyBuffer(m_stateBuffer);
        if (m_queueBuffer.isValid()) m_allocator->destroyBuffer(m_queueBuffer);
        if (m_counterBuffer.isValid()) m_allocator->destroyBuffer(m_counterBuffer);
    }
}

bool WavefrontBuffers::build(GpuAllocator& allocator, std::uint32_t capacity) {
    if (capacity == 0) return false;

    m_layout = PathStateLayout(capacity);
    if (m_layout.arena().totalBytes() == 0) return false;

    m_allocator = &allocator;

    // CpuToGpu + persistently mapped keeps readback simple for the
    // scaffolding stage, matching GradientArena's tradeoff.
    m_stateBuffer = allocator.createBuffer(
        static_cast<VkDeviceSize>(m_layout.arena().totalBytes()),
        kCommonUsage, AllocationUsage::CpuToGpu, /*persistentlyMapped=*/true);

    // Two capacity-sized uint rings back to back: current bounce at index 0,
    // next bounce at index capacity.
    const VkDeviceSize queueBytes =
        static_cast<VkDeviceSize>(capacity) * 2u * sizeof(std::uint32_t);
    m_queueBuffer = allocator.createBuffer(
        queueBytes, kCommonUsage, AllocationUsage::CpuToGpu, /*persistentlyMapped=*/true);

    // INDIRECT_BUFFER_BIT: a later task issues vkCmdDispatchIndirect reading
    // a group count out of this buffer.
    const VkDeviceSize counterBytes =
        static_cast<VkDeviceSize>(kCounterSlotCount) * sizeof(std::uint32_t);
    m_counterBuffer = allocator.createBuffer(
        counterBytes, kCommonUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        AllocationUsage::CpuToGpu, /*persistentlyMapped=*/true);

    return m_stateBuffer.isValid() && m_queueBuffer.isValid() && m_counterBuffer.isValid();
}

void WavefrontBuffers::destroy(GpuAllocator& allocator) {
    if (m_stateBuffer.isValid()) allocator.destroyBuffer(m_stateBuffer);
    if (m_queueBuffer.isValid()) allocator.destroyBuffer(m_queueBuffer);
    if (m_counterBuffer.isValid()) allocator.destroyBuffer(m_counterBuffer);
    m_layout = PathStateLayout(0);
    m_allocator = nullptr;
}

void WavefrontBuffers::zero(VkCommandBuffer cmd) {
    if (!m_stateBuffer.isValid() || !m_queueBuffer.isValid() || !m_counterBuffer.isValid()) {
        return;
    }

    vkCmdFillBuffer(cmd, m_stateBuffer.buffer, 0,
                     static_cast<VkDeviceSize>(m_layout.arena().totalBytes()), 0u);
    vkCmdFillBuffer(cmd, m_queueBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, m_counterBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);

    // One barrier covering all three fills, same reasoning as
    // GradientArena::zero: any compute work reading/writing these buffers
    // afterwards must not begin until the fills are visible.
    VkBufferMemoryBarrier barriers[3]{};
    VkBuffer buffers[3] = {m_stateBuffer.buffer, m_queueBuffer.buffer, m_counterBuffer.buffer};
    for (int i = 0; i < 3; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].buffer = buffers[i];
        barriers[i].offset = 0;
        barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 3, barriers, 0, nullptr);
}

std::vector<float> WavefrontBuffers::readbackField(GpuAllocator& allocator, PathStateField field) {
    std::vector<float> out;
    if (!m_stateBuffer.isValid()) return out;

    const std::size_t blockIndex = m_layout.block(field);
    const ArenaBlock block = m_layout.arena().block(blockIndex);
    if (block.sizeBytes == 0) return out;

    allocator.invalidateBuffer(m_stateBuffer);
    const auto* base = static_cast<const std::byte*>(m_stateBuffer.getMappedData());
    if (base == nullptr) return out;

    out.resize(block.sizeBytes / sizeof(float));
    std::memcpy(out.data(), base + block.offsetBytes, block.sizeBytes);
    return out;
}

std::uint32_t WavefrontBuffers::readbackCounter(GpuAllocator& allocator, std::uint32_t slot) {
    if (!m_counterBuffer.isValid() || slot >= kCounterSlotCount) return 0;

    allocator.invalidateBuffer(m_counterBuffer);
    const auto* base = static_cast<const std::byte*>(m_counterBuffer.getMappedData());
    if (base == nullptr) return 0;

    std::uint32_t value = 0;
    std::memcpy(&value, base + static_cast<std::size_t>(slot) * sizeof(std::uint32_t),
                sizeof(std::uint32_t));
    return value;
}

}  // namespace ohao::diff
