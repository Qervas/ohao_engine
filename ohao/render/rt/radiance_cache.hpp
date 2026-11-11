#pragma once

// The radiance cache's GPU side (frontier F1 T3; shaders/rt/radiance_cache.slang has the design):
// three device-local buffers of `capacity` entries and the resolve pass that blends each frame's
// samples into them. Two owners, each its own cache: the path tracer (bindings 39-41; per realtime
// frame the update trace, resolve() and then the frame's trace) and RT GI (bindings 12-14; its trace
// reads and adds, then resolve()). clear() empties it (a reset or resize:
// a capture after a reset starts from an empty cache in any host, so it repeats).

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace ohao::render {

class RadianceCache {
public:
    static constexpr uint32_t kDefaultCapacity = 1u << 21;   // 2M entries, 72 MiB
    static constexpr uint32_t kMaxSamples = 256;             // a cell's history, in samples (the path tracer's)
    static constexpr uint32_t kStaleFrames = 64;             // frames without a sample before a cell is freed

    // `maxSamples`: a cell's history; a new frame's samples weigh at least their share of it, so a
    // shorter one follows changing light (and RT GI's bounce a frame) sooner and averages less.
    bool create(VkDevice device, GpuAllocator& allocator, uint32_t capacity = kDefaultCapacity,
                uint32_t maxSamples = kMaxSamples);
    void destroy();

    [[nodiscard]] bool valid() const noexcept { return m_keys.buffer != VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] VkBuffer keys() const noexcept { return m_keys.buffer; }
    [[nodiscard]] VkBuffer accum() const noexcept { return m_accum.buffer; }
    [[nodiscard]] VkBuffer resolved() const noexcept { return m_resolved.buffer; }

    // What a trace declares: one that feeds the cache (looks cells up, claims them and adds its samples), and one
    // that only looks cells up.
    void fedBy(Pass& pass);
    void lookedUpBy(Pass& pass);

    // Empties every cell at the next recordBeginFrame.
    void clear() noexcept { m_clearPending = true; }
    // Before the update trace: fills the three buffers with zeros if a clear is pending.
    void recordBeginFrame(const Frame& frame);
    // The resolve pass: after the update trace's writes, before the frame's trace reads.
    void recordResolve(const Frame& frame);

private:
    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    uint32_t m_capacity{0};
    uint32_t m_maxSamples{kMaxSamples};
    GpuBuffer m_keys, m_accum, m_resolved;
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkDescriptorSet m_set{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    bool m_clearPending{true};
};

} // namespace ohao::render
