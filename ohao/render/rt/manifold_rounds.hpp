#pragma once

// The manifold pass as rounds of bounded items (docs/plans/2026-11-08-caustics-step2c.md, "The pass as rounds"): the
// buffers rt/path_tracer.slang's manifoldSetup, manifoldWalk*, manifoldSolveRound, manifoldConnect and manifoldResolve
// share (the rounds after the setup traced indirectly, as many threads as the round before queued). The path tracer's
// bindings:
//   58 records   6 float4 a request and light (manifoldSetup writes them)
//   59 items     uint2 an item, a region a queue: groups, clusters, sub-clusters, candidate triangles
//   60 roots     4 uint4 a root (manifoldSolveRound writes them, manifoldConnect reads them)
//   61 counts    the queues' trace arguments and counts, the capacities and regions, the frame's counters (the
//                words: rt/path_tracer.slang, "the manifold pass as rounds")
//   62 sum       3 words a pixel: the frame's light in fixed point (kCausticSplatScale), cleared by the resolve
//
// The frame's pixels are taken in batches of whole rows, at most kBatchPixels each: a batch's setup and rounds run on
// its rows, into the one fixed-point sum, and the resolve runs once after the last. The queues hold a batch: their
// capacities are the batch's pixels times the peaks a pixel measured, with headroom; an item past its queue's capacity
// is still counted (the stats' word 12) and never written, which no measured frame has done.

#include <vulkan/vulkan.h>

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"

#include <array>
#include <cstdint>

namespace ohao::render {

class ManifoldRounds {
public:
    // The queues whose items the walk and the solve read, in manifoldItems' order, then the roots.
    enum Queue : uint32_t { Groups = 0, Clusters = 1, Subs = 2, Candidates = 3, Roots = 4 };
    // A batch's pixels at most (whole rows: a row more when a row is wider), and the batches a frame at most.
    static constexpr uint32_t kBatchPixels = 131072u;
    static constexpr uint32_t kMaxBatches = 256u;

    bool create(VkDevice device, GpuAllocator& allocator);
    void destroy();

    // The batches and buffers for a width x height frame (the buffers they replace are retired). False: the pass
    // stays off.
    bool ensure(uint32_t width, uint32_t height);
    [[nodiscard]] uint32_t batches() const noexcept { return m_batches; }
    [[nodiscard]] uint32_t rowsPerBatch() const noexcept { return m_rows; }

    // The frame's counts reset (dispatch arguments, counts, counters; the capacities and regions written), and the
    // sum cleared on the first frame after it was (re)allocated.
    void recordReset(const Frame& frame);

    // What each round declares (besides the scene and the casters).
    void setupPass(Pass& pass);
    void walkPass(Pass& pass);
    void solvePass(Pass& pass);
    void connectPass(Pass& pass);
    void resolvePass(Pass& pass);

    // A batch's queue's trace arguments (VkTraceRaysIndirectCommandKHR: its items rounded up to 64) in the counts
    // buffer.
    [[nodiscard]] static VkDeviceSize argsOffset(uint32_t batch, Queue q) noexcept {
        return 4u * (64u + 32u * VkDeviceSize{batch}) + VkDeviceSize{q} * 16u;
    }

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
    uint32_t m_queuePixels{0}, m_sumPixels{0};
    bool m_queues{false};
    bool m_clearSum{true};
    uint32_t m_recordCap{0};
    std::array<uint32_t, 5> m_caps{};   // the queues', Groups to Roots
};

} // namespace ohao::render
