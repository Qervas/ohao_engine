#pragma once

// Path guiding's GPU side (sub-project 23; docs/specs/2026-11-02-path-guiding-design.md;
// shaders/rt/path_guiding.slang has the grid's design): the grid's two state buffers, the claim
// words, the proposals and the per-pixel sample radiance, and the passes that learn from them
// (shaders/rt/path_guiding_update.slang).
//
// A frame with guiding on: recordBeginFrame (a pending clear), the path tracer's trace (reads
// `states()`, writes `proposals()` and `pixel()`), then recordLearn: update, owner and write over
// the proposals into the second buffer, which is copied over the first, so the next frame reads
// what this one learned and both buffers are equal again. clear() empties the grid (a reset).
//
// Until guiding is first used (ensure), every buffer is a placeholder of one element, so a tracer
// that never guides binds them and allocates nothing; ensure() allocates the grid (2 x 32 MiB, the
// claims 8 MiB) and a pixel's buffers (kGuideTrainVertices x 64 bytes and 16 bytes a pixel).

#include <vulkan/vulkan.h>

#include <cstdint>

#include <glm/glm.hpp>

namespace ohao::render {

class PathGuiding {
public:
    bool create(VkDevice device, VkPhysicalDevice physicalDevice);
    void destroy();

    // Allocates the grid and the buffers for `pixels` pixels if they are not there yet (idles the
    // device when it does). False when an allocation failed (guiding then stays off).
    bool ensure(uint32_t pixels);
    [[nodiscard]] bool ready(uint32_t pixels) const noexcept { return m_full && m_pixels >= pixels; }

    // Empties the grid at the next recordBeginFrame.
    void clear() noexcept { m_clearPending = true; }

    [[nodiscard]] VkBuffer states() const noexcept { return m_states.buffer; }
    [[nodiscard]] VkBuffer proposals() const noexcept { return m_proposals.buffer; }
    [[nodiscard]] VkBuffer pixel() const noexcept { return m_pixel.buffer; }

    // Before the trace: a pending clear, and the previous frame's copy ordered before the reads.
    void recordBeginFrame(VkCommandBuffer cmd);
    // After the trace: learn from `pixels` pixels' proposals (frame: the tracer's sample index).
    void recordLearn(VkCommandBuffer cmd, uint32_t pixels, uint32_t frame, const glm::vec3& camera);

    // A guided bounce's share (G4) and the lobes' prior in samples (G5); OHAO_GUIDE_PROB and
    // OHAO_GUIDE_PRIOR override them for tuning.
    [[nodiscard]] float share() const noexcept { return m_share; }
    [[nodiscard]] float prior() const noexcept { return m_prior; }
    // Vertices guided, from the camera's (0: every one); OHAO_GUIDE_DEPTHS overrides.
    [[nodiscard]] uint32_t depths() const noexcept { return m_depths; }

private:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
    };
    bool allocate(Buffer& b, VkDeviceSize size, VkBufferUsageFlags extraUsage);
    void release(Buffer& b);
    void writeSet();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    Buffer m_states, m_next, m_claims, m_proposals, m_pixel;
    bool m_full{false};
    uint32_t m_pixels{0};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkDescriptorSet m_set{VK_NULL_HANDLE};
    VkPipelineLayout m_layouts[3]{};
    VkPipeline m_pipelines[3]{};   // update, owner, write
    bool m_clearPending{true};
    float m_share{0.5f};
    float m_prior{1.0f};
    uint32_t m_depths{0};
};

} // namespace ohao::render
