#include "render/rt/manifold_rounds.hpp"

#include "core/log.hpp"

#include "gpu/shared_layout.hpp"

#include <algorithm>
#include <vector>

namespace ohao::render {

namespace {

constexpr uint32_t kRecordBytes = 6 * 16;   // 6 float4
constexpr uint32_t kItemBytes = 8;          // uint2
constexpr uint32_t kRootBytes = 4 * 16;     // 4 uint4
// The counts (rt/path_tracer.slang's kMC*): a header, then a block a batch and pass, all reset every frame; then two
// words a record (step 2c's cell statistics), cleared by the statistics' pass.
constexpr uint32_t kHeaderWords = ManifoldRounds::kHeaderWords;
constexpr uint32_t kBlockWords = ManifoldRounds::kBlockWords;
constexpr uint32_t kCountWords = kHeaderWords + kBlockWords * ManifoldRounds::kMaxBatches * ManifoldRounds::kPasses;

// Capacities a pixel of a batch. A batch is a band of rows, and a band over the showcase's casters is denser than the
// frame: the fullest 1280x720 band (102 rows) held 0.92 records, 6.5 groups, 6.4 clusters, 18.3 sub-clusters, 10.6
// candidates and 0.89 roots a pixel (2026-11-17), against the frame's 0.60, 4.2, 3.7, 10.5, 5.9 and 0.58. Records: a
// request a depth and light, kManifoldDepths with one light; the queues about 2.3 times the fullest band. Step 2c's
// pyramid nodes (over a pass's segments) and cells, uint4 items: 8 each, a placeholder (the showcase's mapped walk
// asked for 2,700 pyramid nodes a pixel at 320x180, stage 2's measurement, far past any capacity; carried or lost).
constexpr std::array<uint32_t, 7> kPerPixel{16, 16, 48, 24, 2, 8, 8};   // groups, clusters, sub-clusters, candidates,
                                                                          // roots, pyramid nodes, cells

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

bool ManifoldRounds::ensure(uint32_t width, uint32_t height, uint32_t lights) {
    if (m_device == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    lights = std::max(lights, 1u);
    // The batch: whole rows, kBatchPixels at most (one row at least), and at most kMaxBatches of them.
    m_rows = std::max(1u, kBatchPixels / width);
    m_rows = std::max(m_rows, (height + kMaxBatches - 1) / kMaxBatches);
    m_batches = (height + m_rows - 1) / m_rows;
    const uint32_t batchPixels = m_rows * width;
    if (!m_queues || m_queuePixels < batchPixels || m_queueLights < lights) {
        m_recordCap = batchPixels * gpu::shared::kManifoldDepths * lights;
        m_caps[Records] = m_recordCap;
        uint64_t items = 0;
        for (uint32_t q = Groups; q < kQueues; ++q) m_caps[q] = batchPixels * kPerPixel[q - 1];
        for (uint32_t q = 0; q < kQueues; ++q)
            if (q != Roots) items += (q >= Pyramid ? 4ull : 2ull) * m_caps[q];   // two regions each (uint4: two slots)
        m_queues = allocate(m_records, VkDeviceSize{m_recordCap} * kRecordBytes) &&
                   allocate(m_items, VkDeviceSize{items} * kItemBytes) &&
                   allocate(m_roots, VkDeviceSize{m_caps[Roots]} * kRootBytes) &&
                   allocate(m_counts, (VkDeviceSize{kCountWords} + 2ull * m_recordCap) * 4u,
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        if (!m_queues) {
            OHAO_LOG(Error, render, "manifold rounds: the queues for a batch of {} pixels could not be allocated; the "
                     "pass stays off", batchPixels);
            allocate(m_records, kRecordBytes);
            allocate(m_items, kItemBytes);
            allocate(m_roots, kRootBytes);
            allocate(m_counts, kCountWords * 4u, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            m_queuePixels = 0;
            return false;
        }
        m_clearRecordStats = true;
        m_queuePixels = batchPixels;
        m_queueLights = lights;
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
    // The header (rt/path_tracer.slang's kMC*: capacities at 16, regions at 25 (in slots), rows, batches and passes at
    // 33-35, the dumped pixel at 36, the records' statistics at 37 (0: off), the sparse grid at 38, the mapped walk's
    // orientation at 39, counters from 40); the batches' blocks zero
    // (a reservation sets its arguments' height and depth).
    std::array<uint32_t, kHeaderWords> words{};
    words[16] = m_recordCap;
    uint32_t offset = 0;
    for (uint32_t q = 0; q < kQueues; ++q) {
        words[17 + q] = m_caps[q];
        if (q != Roots) {
            words[25 + q] = offset;
            offset += (q >= Pyramid ? 4 : 2) * m_caps[q];
        }
    }
    words[33] = m_rows;
    words[34] = m_batches;
    words[35] = kPasses;
    words[36] = m_dumpPixel;
    words[37] = m_cellStats ? kCountWords : 0u;
    words[38] = m_sparse;
    words[39] = m_oriented ? 1u : 0u;
    Pass pass(frame, nullptr, Pass::Transfer);
    pass.write(m_counts, BufferUse::CopyDst);
    if (m_clearSum) pass.write(m_sum, BufferUse::CopyDst);
    pass.begin();
    vkCmdUpdateBuffer(frame.cmd, m_counts.buffer, 0, sizeof words, words.data());
    vkCmdFillBuffer(frame.cmd, m_counts.buffer, sizeof words, VkDeviceSize{kBlockWords} * 4u * m_batches * kPasses, 0u);
    if (m_clearRecordStats) {
        vkCmdFillBuffer(frame.cmd, m_counts.buffer, VkDeviceSize{kCountWords} * 4u, VK_WHOLE_SIZE, 0u);
        m_clearRecordStats = false;
    }
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
    pass.read(m_records, BufferUse::Storage).readWrite(m_items, BufferUse::Storage).write(m_roots, BufferUse::Storage);
}

void ManifoldRounds::connectPass(Pass& pass) {
    pass.read(m_counts, BufferUse::Indirect).readWrite(m_counts, BufferUse::Storage).read(m_records, BufferUse::Storage).read(m_roots, BufferUse::Storage);
    pass.readWrite(m_sum, BufferUse::Storage);
}

void ManifoldRounds::resolvePass(Pass& pass) {
    pass.read(m_counts, BufferUse::Storage).readWrite(m_sum, BufferUse::Storage);
}

} // namespace ohao::render
