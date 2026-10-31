#pragma once

// The radiance cache's GPU side (frontier F1 T3; shaders/rt/radiance_cache.slang has the design):
// three device-local buffers of `capacity` entries and the resolve pass that blends each frame's
// samples into them. Two owners, each its own cache: the path tracer (bindings 39-41; per realtime
// frame the update trace, resolve() and then the frame's trace) and RT GI (bindings 12-14; its trace
// reads and adds, then resolve()). clear() empties it (a reset or resize:
// a capture after a reset starts from an empty cache in any host, so it repeats).

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
    bool create(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t capacity = kDefaultCapacity,
                uint32_t maxSamples = kMaxSamples);
    void destroy();

    [[nodiscard]] bool valid() const noexcept { return m_keys != VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] VkBuffer keys() const noexcept { return m_keys; }
    [[nodiscard]] VkBuffer accum() const noexcept { return m_accum; }
    [[nodiscard]] VkBuffer resolved() const noexcept { return m_resolved; }

    // Empties every cell at the next recordBeginFrame.
    void clear() noexcept { m_clearPending = true; }
    // Before the update trace: orders the previous frame's cache work before this frame's, and
    // fills the three buffers with zeros if a clear is pending.
    void recordBeginFrame(VkCommandBuffer cmd);
    // The resolve pass: after the update trace's writes, before the frame's trace reads.
    void recordResolve(VkCommandBuffer cmd);

private:
    VkDevice m_device{VK_NULL_HANDLE};
    uint32_t m_capacity{0};
    uint32_t m_maxSamples{kMaxSamples};
    VkBuffer m_keys{VK_NULL_HANDLE}, m_accum{VK_NULL_HANDLE}, m_resolved{VK_NULL_HANDLE};
    VkDeviceMemory m_keysMemory{VK_NULL_HANDLE}, m_accumMemory{VK_NULL_HANDLE}, m_resolvedMemory{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkDescriptorSet m_set{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    bool m_clearPending{true};
};

} // namespace ohao::render
