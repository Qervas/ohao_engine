#pragma once

// The manifold pass as rounds of bounded items (docs/plans/2026-11-08-caustics-step2c.md, "The pass as rounds"): the
// buffers rt/path_tracer.slang's manifoldSetup, manifoldWalk*, manifoldSolveRound, manifoldConnect and manifoldResolve
// share (the rounds after the setup traced indirectly, as many threads as the round before queued). The path tracer's
// bindings:
//   58 records   6 float4 a request and light (manifoldSetup writes them)
//   59 items     uint2 an item, a region a queue: groups, clusters, sub-clusters, candidate triangles; step 2c's pyramid
//                nodes and cells (uint4 items, two slots each)
//   60 roots     4 uint4 a root (manifoldSolveRound writes them, manifoldConnect reads them)
//   61 counts    the queues' trace arguments and counts, the capacities and regions, the frame's counters (the
//                words: rt/path_tracer.slang, "the manifold pass as rounds")
//   62 sum       3 words a pixel: the frame's light in fixed point (kCausticSplatScale), cleared by the resolve
//
// The frame's pixels are taken in batches of whole rows, at most kBatchPixels each, and each batch in kPasses passes:
// pass 0 runs the setup and every round on the batch's rows; an item whose outputs do not fit their queue is carried
// into its own queue's next pass, where every round runs again. A pass's rounds: the records, groups, clusters,
// sub-clusters, kPyramidRounds of the pyramid queue (a segment each: a mapped triangle's footprint a level a round),
// the cells, the solve, the connect. Into the one fixed-point sum; the resolve runs once
// after the last batch. The queues' capacities are the batch's pixels times the peaks a pixel measured, with headroom
// (the records' exact: kManifoldDepths a pixel and light), so carries are rare; an item still carried after the last
// pass is counted lost (the stats' word 12).

#include <vulkan/vulkan.h>

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"

#include <array>
#include <cstdint>

namespace ohao::render {

class ManifoldRounds {
public:
    // The queues, in the order the rounds read them: the records' items (R0), groups, clusters, sub-clusters,
    // candidates (each two regions of manifoldItems, a pass's input and the next pass's carries), then the roots.
    enum Queue : uint32_t { Records = 0, Groups = 1, Clusters = 2, Subs = 3, Candidates = 4, Roots = 5, Pyramid = 6,
                            Cells = 7 };
    static constexpr uint32_t kQueues = 8;
    // The pyramid queue's rounds a pass (rt/path_tracer.slang's kManifoldPyramidRounds).
    static constexpr uint32_t kPyramidRounds = 10u;
    // A batch's pixels at most (whole rows: a row more when a row is wider), the batches a frame at most, and the passes
    // a batch: every round runs once a pass, on the items the last pass carried and what they yield. Measured on the
    // showcase at 1920x1080 (2026-11-17) with the queues planted smaller: at a tenth of their capacities (a fifth of
    // the fullest band's peaks) 6 passes held an item, at a sixteenth (an eighth of the peaks) 10, none lost; an empty
    // pass costs its six traces, about 10 us each.
    static constexpr uint32_t kBatchPixels = 131072u;
    static constexpr uint32_t kMaxBatches = 256u;
    static constexpr uint32_t kPasses = 12u;

    bool create(VkDevice device, GpuAllocator& allocator);
    void destroy();

    // The batches and buffers for a width x height frame lit by `lights` directional lights (the buffers they replace
    // are retired). False: the pass stays off.
    bool ensure(uint32_t width, uint32_t height, uint32_t lights);
    [[nodiscard]] uint32_t batches() const noexcept { return m_batches; }
    [[nodiscard]] uint32_t rowsPerBatch() const noexcept { return m_rows; }

    // The frame's counts reset (trace arguments, counts, counters; the capacities and regions written), and the sum
    // cleared on the first frame after it was (re)allocated.
    void recordReset(const Frame& frame);

    // What each round declares (besides the scene and the casters).
    void setupPass(Pass& pass);
    void walkPass(Pass& pass);
    void solvePass(Pass& pass);
    void connectPass(Pass& pass);
    void resolvePass(Pass& pass);

    // A batch and pass's queue's trace arguments (VkTraceRaysIndirectCommandKHR: its items rounded up to 64) in the
    // counts buffer; the pyramid queue's segment j's (0: its queue's).
    static constexpr uint32_t kHeaderWords = 128;
    static constexpr uint32_t kBlockWords = 96;
    [[nodiscard]] static VkDeviceSize blockOffset(uint32_t batch, uint32_t pass) noexcept {
        return 4u * (kHeaderWords + kBlockWords * (VkDeviceSize{batch} * kPasses + pass));
    }
    [[nodiscard]] static VkDeviceSize argsOffset(uint32_t batch, uint32_t pass, Queue q) noexcept {
        return blockOffset(batch, pass) + VkDeviceSize{q} * 16u;
    }
    [[nodiscard]] static VkDeviceSize segmentOffset(uint32_t batch, uint32_t pass, uint32_t j) noexcept {
        return j == 0 ? argsOffset(batch, pass, Pyramid) : blockOffset(batch, pass) + 4u * (36u + 4u * (j - 1u));
    }
    // Step 2c's cell statistics (each record's cells and pyramid tests, binned after each batch: OHAO_MANIFOLD_CELL_STATS)
    // and the pixel whose requests are dumped (OHAO_MANIFOLD_DUMP; ~0 none): words of the header.
    void setCellStats(bool on) noexcept { m_cellStats = on; }
    void setDumpPixel(uint32_t pixel) noexcept { m_dumpPixel = pixel; }
    // Only the pixels on a grid of this step make requests (OHAO_MANIFOLD_SPARSE; 0 or 1: every pixel): step 2c's
    // measurement of requests whose walks a whole frame of could not hold.
    void setSparse(uint32_t step) noexcept { m_sparse = step; }
    // The mapped walk keeps only a path's roots (h . H > 0: rt/manifold.slang, manifoldMapHullTest); off
    // (OHAO_MANIFOLD_ORIENTED=0) it keeps either orientation, to measure what the sign prunes.
    void setOriented(bool on) noexcept { m_oriented = on; }
    [[nodiscard]] uint32_t recordCapacity() const noexcept { return m_recordCap; }

    [[nodiscard]] VkBuffer records() const noexcept { return m_records.buffer; }
    [[nodiscard]] VkBuffer items() const noexcept { return m_items.buffer; }
    [[nodiscard]] VkBuffer roots() const noexcept { return m_roots.buffer; }
    [[nodiscard]] VkBuffer counts() const noexcept { return m_counts.buffer; }
    [[nodiscard]] VkBuffer sum() const noexcept { return m_sum.buffer; }

private:
    bool allocate(GpuBuffer& b, VkDeviceSize size, VkBufferUsageFlags extra = 0);

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_records, m_items, m_roots, m_counts, m_sum;
    uint32_t m_rows{1}, m_batches{1};
    uint32_t m_queuePixels{0}, m_queueLights{0}, m_sumPixels{0};
    bool m_queues{false};
    bool m_clearSum{true};
    uint32_t m_recordCap{0};
    bool m_cellStats{false};
    bool m_clearRecordStats{true};
    uint32_t m_dumpPixel{~0u};
    uint32_t m_sparse{0};
    bool m_oriented{true};
    std::array<uint32_t, kQueues> m_caps{};
};

} // namespace ohao::render
