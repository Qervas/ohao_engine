#include "render/rt/manifold_rounds.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <vector>

namespace ohao::render {

namespace {

constexpr uint32_t kRecordBytes = 6 * 16;   // 6 float4
constexpr uint32_t kItemBytes = 8;          // uint2
constexpr uint32_t kRootBytes = 4 * 16;     // 4 uint4
// The counts (rt/path_tracer.slang's kMC*): a header of 64 words, then 32 a batch; all reset every frame.
constexpr uint32_t kHeaderWords = 64;
constexpr uint32_t kBlockWords = 32;
constexpr uint32_t kCountWords = kHeaderWords + kBlockWords * ManifoldRounds::kMaxBatches;

// Capacities a pixel of a batch. A batch is a band of rows, and a band over the showcase's casters is denser than the
// frame: the fullest 1280x720 band (102 rows) held 0.92 records, 6.5 groups, 6.4 clusters, 18.3 sub-clusters, 10.6
// candidates and 0.89 roots a pixel (2026-11-17), against the frame's 0.60, 4.2, 3.7, 10.5, 5.9 and 0.58. Records: a
// request a depth and light, kManifoldDepths with one light; the queues about 2.3 times the fullest band.
constexpr uint32_t kRecordsPerPixel = 2;
constexpr std::array<uint32_t, 5> kPerPixel{16, 16, 48, 24, 2};   // groups, clusters, sub-clusters, candidates, roots

} // namespace

bool ManifoldRounds::allocate(GpuBuffer& b, VkDeviceSize size, VkBufferUsageFlags extra) {
    m_allocator->retire(b);
    b = m_allocator->createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                                  AllocationUsage::GpuOnly);
    return b.isValid();
}

bool ManifoldRounds::create(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;
    // The smallest buffers, so every binding is valid before a frame runs the pass; the counts at their size.
    return allocate(m_records, kRecordBytes) && allocate(m_items, kItemBytes) && allocate(m_roots, kRootBytes) &&
           allocate(m_counts, kCountWords * 4u,
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) &&
           allocate(m_sum, 16);
}

void ManifoldRounds::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    for (GpuBuffer* b : {&m_records, &m_items, &m_roots, &m_counts, &m_sum}) m_allocator->retire(*b);
    m_queues = false;
    m_sumPixels = 0;
}

bool ManifoldRounds::ensure(uint32_t width, uint32_t height) {
    if (m_device == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    // The batch: whole rows, kBatchPixels at most (one row at least), and at most kMaxBatches of them.
    m_rows = std::max(1u, kBatchPixels / width);
    m_rows = std::max(m_rows, (height + kMaxBatches - 1) / kMaxBatches);
    m_batches = (height + m_rows - 1) / m_rows;
    const uint32_t batchPixels = m_rows * width;
    if (!m_queues || m_queuePixels < batchPixels) {
        m_recordCap = batchPixels * kRecordsPerPixel;
        uint64_t items = 0;
        for (uint32_t q = 0; q < 5; ++q) {
            m_caps[q] = batchPixels * kPerPixel[q];
            if (q != Roots) items += m_caps[q];
        }
        m_queues = allocate(m_records, VkDeviceSize{m_recordCap} * kRecordBytes) &&
                   allocate(m_items, VkDeviceSize{items} * kItemBytes) &&
                   allocate(m_roots, VkDeviceSize{m_caps[Roots]} * kRootBytes);
        if (!m_queues) {
            OHAO_LOG(Error, render, "manifold rounds: the queues for a batch of {} pixels could not be allocated; the "
                     "pass stays off", batchPixels);
            allocate(m_records, kRecordBytes);
            allocate(m_items, kItemBytes);
            allocate(m_roots, kRootBytes);
            m_queuePixels = 0;
            return false;
        }
        m_queuePixels = batchPixels;
        OHAO_LOG(Debug, render, "manifold rounds: batches of {} pixels: {} records, {} items, {} roots ({} MB)",
                 batchPixels, m_recordCap, items, m_caps[Roots],
                 (VkDeviceSize{m_recordCap} * kRecordBytes + items * kItemBytes +
                  VkDeviceSize{m_caps[Roots]} * kRootBytes) >> 20);
    }
    const uint32_t pixels = width * height;
    if (m_sumPixels < pixels) {
        if (!allocate(m_sum, VkDeviceSize{pixels} * 3u * 4u)) {
            OHAO_LOG(Error, render, "manifold rounds: a sum for {} pixels could not be allocated; the pass stays off",
                     pixels);
            allocate(m_sum, 16);
            m_sumPixels = 0;
            return false;
        }
        m_sumPixels = pixels;
        m_clearSum = true;
    }
    return true;
}

void ManifoldRounds::recordReset(const Frame& frame) {
    std::vector<uint32_t> words(kHeaderWords + kBlockWords * m_batches, 0u);
    words[24] = m_recordCap;
    words[30] = m_rows;
    words[31] = m_batches;
    uint32_t offset = 0;
    for (uint32_t q = 0; q < 5; ++q) {
        words[25 + q] = m_caps[q];
        if (q != Roots) {
            words[32 + q] = offset;
            offset += m_caps[q];
        }
    }
    for (uint32_t b = 0; b < m_batches; ++b)
        for (uint32_t q = 0; q < 5; ++q) {   // (width, 1, 1) and the count
            words[kHeaderWords + kBlockWords * b + q * 4 + 1] = 1;
            words[kHeaderWords + kBlockWords * b + q * 4 + 2] = 1;
        }
    Pass pass(frame, nullptr, Pass::Transfer);
    pass.write(m_counts, BufferUse::CopyDst);
    if (m_clearSum) pass.write(m_sum, BufferUse::CopyDst);
    pass.begin();
    vkCmdUpdateBuffer(frame.cmd, m_counts.buffer, 0, words.size() * 4u, words.data());   // at most 33 KB
    if (m_clearSum) {
        vkCmdFillBuffer(frame.cmd, m_sum.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_clearSum = false;
    }
}

void ManifoldRounds::setupPass(Pass& pass) {
    pass.write(m_records, BufferUse::Storage).write(m_items, BufferUse::Storage);
    pass.readWrite(m_counts, BufferUse::Storage).readWrite(m_sum, BufferUse::Storage);
}

void ManifoldRounds::walkPass(Pass& pass) {
    pass.read(m_counts, BufferUse::Indirect).readWrite(m_counts, BufferUse::Storage);
    pass.read(m_records, BufferUse::Storage).readWrite(m_items, BufferUse::Storage);
}

void ManifoldRounds::solvePass(Pass& pass) {
    pass.read(m_counts, BufferUse::Indirect).readWrite(m_counts, BufferUse::Storage);
    pass.read(m_records, BufferUse::Storage).read(m_items, BufferUse::Storage).write(m_roots, BufferUse::Storage);
}

void ManifoldRounds::connectPass(Pass& pass) {
    pass.read(m_counts, BufferUse::Indirect).readWrite(m_counts, BufferUse::Storage).read(m_records, BufferUse::Storage).read(m_roots, BufferUse::Storage);
    pass.readWrite(m_sum, BufferUse::Storage);
}

void ManifoldRounds::resolvePass(Pass& pass) {
    pass.read(m_counts, BufferUse::Storage).readWrite(m_sum, BufferUse::Storage);
}

} // namespace ohao::render
