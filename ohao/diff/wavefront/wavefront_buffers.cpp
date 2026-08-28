#include "diff/wavefront/wavefront_buffers.hpp"

#include <cstring>
#include <vector>

namespace ohao::diff {

namespace {
// TRANSFER_SRC (in addition to TRANSFER_DST for zero()'s vkCmdFillBuffer) so
// any of these buffers can also be the source of a vkCmdCopyBuffer -- e.g.
// GpuProbeContext::runWavefrontGenerateProbe copies queue 0 out into its own
// host-visible buffer for readback, since this class exposes only a raw
// VkBuffer for the queue, not the GpuBuffer wrapper GpuAllocator's
// invalidate/map calls need.
constexpr VkBufferUsageFlags kCommonUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}  // namespace

WavefrontBuffers::~WavefrontBuffers() {
    // Backstop only: normal teardown is the explicit destroy(allocator) call.
    // This exists so early-return error paths (mirroring GradientArena) that
    // never reach an explicit destroy() don't leak the buffers.
    if (m_allocator != nullptr) {
        if (m_stateBuffer.isValid()) m_allocator->destroyBuffer(m_stateBuffer);
        if (m_queueBuffer.isValid()) m_allocator->destroyBuffer(m_queueBuffer);
        if (m_counterBuffer.isValid()) m_allocator->destroyBuffer(m_counterBuffer);
        if (m_envMarginalBuffer.isValid()) m_allocator->destroyBuffer(m_envMarginalBuffer);
        if (m_envConditionalBuffer.isValid()) m_allocator->destroyBuffer(m_envConditionalBuffer);
    }
}

bool WavefrontBuffers::build(GpuAllocator& allocator, std::uint32_t capacity,
                             std::uint32_t envWidth, std::uint32_t envHeight) {
    if (capacity == 0) return false;
    if (envWidth == 0 || envHeight == 0) return false;

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

    // --- Environment CDF, read-only for every dispatch. Same CpuToGpu +
    // persistently-mapped allocation as the rest of this class, because the
    // upload is a host memcpy into the mapped pointer rather than a staged
    // transfer -- these are kilobytes of CDF, not an HDR image. ---
    m_envWidth = envWidth;
    m_envHeight = envHeight;
    m_envMarginalBuffer = allocator.createBuffer(
        static_cast<VkDeviceSize>(envHeight) * sizeof(float), kCommonUsage,
        AllocationUsage::CpuToGpu, /*persistentlyMapped=*/true);
    m_envConditionalBuffer = allocator.createBuffer(
        static_cast<VkDeviceSize>(envWidth) * static_cast<VkDeviceSize>(envHeight) * sizeof(float),
        kCommonUsage, AllocationUsage::CpuToGpu, /*persistentlyMapped=*/true);

    if (!m_stateBuffer.isValid() || !m_queueBuffer.isValid() || !m_counterBuffer.isValid() ||
        !m_envMarginalBuffer.isValid() || !m_envConditionalBuffer.isValid()) {
        return false;
    }

    // Seed with the UV-uniform CDF -- conditional[y][x] = (x+1)/W,
    // marginal[y] = (y+1)/H. This is exactly the fallback ohao::EnvCDF
    // itself writes for an all-black map, so a caller that never calls
    // uploadEnvironment gets a well-formed, strictly-increasing CDF whose
    // every texel has positive probability, rather than an all-zero one
    // whose differences are 0 and whose pdf is therefore 0 everywhere.
    // Uniform in UV is NOT uniform on the sphere; see kDefaultEnvWidth.
    std::vector<float> marginal(envHeight);
    for (std::uint32_t y = 0; y < envHeight; ++y) {
        marginal[y] = static_cast<float>(y + 1) / static_cast<float>(envHeight);
    }
    std::vector<float> conditional(static_cast<std::size_t>(envWidth) * envHeight);
    for (std::uint32_t y = 0; y < envHeight; ++y) {
        for (std::uint32_t x = 0; x < envWidth; ++x) {
            conditional[static_cast<std::size_t>(y) * envWidth + x] =
                static_cast<float>(x + 1) / static_cast<float>(envWidth);
        }
    }
    return uploadEnvironment(marginal, conditional, 1.0f);
}

bool WavefrontBuffers::uploadEnvironment(std::span<const float> marginalCdf,
                                         std::span<const float> conditionalCdf, float integral) {
    if (!m_envMarginalBuffer.isValid() || !m_envConditionalBuffer.isValid()) return false;
    if (marginalCdf.size() != static_cast<std::size_t>(m_envHeight)) return false;
    if (conditionalCdf.size() !=
        static_cast<std::size_t>(m_envWidth) * static_cast<std::size_t>(m_envHeight)) {
        return false;
    }

    auto* marg = static_cast<float*>(m_envMarginalBuffer.getMappedData());
    auto* cond = static_cast<float*>(m_envConditionalBuffer.getMappedData());
    if (marg == nullptr || cond == nullptr) return false;

    std::memcpy(marg, marginalCdf.data(), marginalCdf.size() * sizeof(float));
    std::memcpy(cond, conditionalCdf.data(), conditionalCdf.size() * sizeof(float));
    if (m_allocator != nullptr) {
        m_allocator->flushBuffer(m_envMarginalBuffer);
        m_allocator->flushBuffer(m_envConditionalBuffer);
    }
    m_envIntegral = integral;
    return true;
}

void WavefrontBuffers::destroy(GpuAllocator& allocator) {
    if (m_stateBuffer.isValid()) allocator.destroyBuffer(m_stateBuffer);
    if (m_queueBuffer.isValid()) allocator.destroyBuffer(m_queueBuffer);
    if (m_counterBuffer.isValid()) allocator.destroyBuffer(m_counterBuffer);
    if (m_envMarginalBuffer.isValid()) allocator.destroyBuffer(m_envMarginalBuffer);
    if (m_envConditionalBuffer.isValid()) allocator.destroyBuffer(m_envConditionalBuffer);
    m_layout = PathStateLayout(0);
    m_envWidth = 0;
    m_envHeight = 0;
    m_envIntegral = 0.0f;
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
