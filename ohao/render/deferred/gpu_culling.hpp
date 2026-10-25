#pragma once
// GPU culling (sub-project 2b spec, "GPU-driven geometry"): RenderScene's draw table to
// indirect draw lists, the same handful of commands whatever the instance count.
//
// A frame: cull(early) draws what the last late pass saw and the frustum holds; the early
// G-buffer pass draws it; buildHiZ() makes the max-depth pyramid of that depth; cull(late)
// tests every draw against the frustum and the pyramid, keeps those visible now and not drawn
// early, and records visibility for the next frame; the late G-buffer pass draws them. A CSM
// cascade culls its own frustum (shadow phase). Each phase writes two lists, one-sided and
// double-sided draws, so face culling is a pipeline per list.
//
// Lists are compacted by prefix sum (cull/instances.slang): draw order every run, no atomics.
// The visibility history is cleared by resetHistory() (a temporal reset) and whenever the
// draw table changes, so a capture depends only on the frames since its reset.
#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace ohao {

namespace render { class RenderScene; }

class GpuCulling {
public:
    // A phase's list pair (lists 2 * pair and 2 * pair + 1).
    static constexpr uint32_t kPairEarly = 0, kPairLate = 1, kPairCascade0 = 2;

    struct Settings {
        bool frustum{true};     // false: every draw is drawn (in the late pass, in draw order)
        bool occlusion{true};   // the late pass's Hi-Z test
    };

    // What the last captured frame's culling wrote (requestCapture): its lists' counts, the
    // visibility history after its late pass, and its commands (5 uints each, list by list,
    // drawCount a list).
    struct Capture {
        uint32_t drawCount{0};
        std::vector<uint32_t> counts;       // gpu::shared::kCullLists
        std::vector<uint32_t> visibility;   // drawCount
        std::vector<uint32_t> commands;     // kCullLists * drawCount * 5
    };

    GpuCulling() = default;
    ~GpuCulling();
    GpuCulling(const GpuCulling&) = delete;
    GpuCulling& operator=(const GpuCulling&) = delete;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();
    // The pyramid follows the frame's size; `depth` is the G-buffer depth view it is built
    // from (read in DEPTH_STENCIL_READ_ONLY_OPTIMAL). The device must be idle.
    bool resize(uint32_t width, uint32_t height, VkImageView depth);

    void setSettings(const Settings& s) noexcept { m_settings = s; }
    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }
    // Forget which draws were visible (the next early pass draws nothing).
    void resetHistory() noexcept { m_historyReset = true; }

    // Start of a frame's culling, outside any render pass: binds the scene's tables to the
    // slot's set and clears the history when it is stale. False when there is nothing to
    // draw (no draw table yet): the passes then draw nothing.
    bool beginFrame(VkCommandBuffer cmd, uint32_t slot, const render::RenderScene& scene);
    // One phase over the draw table, with the view-projection the draws are rasterized with.
    void cull(VkCommandBuffer cmd, uint32_t phase, uint32_t pair, const glm::mat4& viewProj);
    // The pyramid of the depth the early pass left (DEPTH_STENCIL_READ_ONLY_OPTIMAL).
    void buildHiZ(VkCommandBuffer cmd);
    // vkCmdDrawIndexedIndirectCount over list `list` (the caller binds the pipeline, the
    // vertex and index arenas and its sets). Nothing when the frame has no draws.
    void drawList(VkCommandBuffer cmd, uint32_t list) const;
    [[nodiscard]] uint32_t drawCount() const noexcept { return m_drawCount; }

    // Tests: while on, every frame copies its culling buffers out (after its late pass).
    void setCapture(bool on) noexcept { m_captureRequested = on; }
    // The last captured frame's buffers; call once that frame's fence has signalled.
    [[nodiscard]] Capture capture() const;

private:
    bool ensureCapacity(uint32_t draws);
    void destroyBuffers();
    void destroyHiZ();
    bool createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory);
    void recordCapture(VkCommandBuffer cmd);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    Settings m_settings;

    // cull/instances.slang: one set layout for the three entries, one set a frame slot
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_classify{VK_NULL_HANDLE}, m_scanBlocks{VK_NULL_HANDLE}, m_scatter{VK_NULL_HANDLE};

    // Per draw (capacity draws), device-local
    uint32_t m_capacity{0};
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
    };
    Buffer m_visibility, m_local, m_blockSums, m_blockOffsets, m_counts, m_commands;

    // The pyramid: R32F, every level, GENERAL; one set a level
    rtc::Image m_hiz{};
    uint32_t m_hizLevels{0};
    std::vector<VkImageView> m_hizLevelViews;
    std::vector<std::array<uint32_t, 2>> m_hizSizes;
    uint32_t m_width{0}, m_height{0};
    VkImageView m_depth{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_hizSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_hizPool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> m_hizSets;
    VkPipelineLayout m_hizLayout{VK_NULL_HANDLE};
    VkPipeline m_fromDepth{VK_NULL_HANDLE}, m_downsample{VK_NULL_HANDLE};

    // This frame
    VkDescriptorSet m_set{VK_NULL_HANDLE};
    uint32_t m_drawCount{0};
    uint64_t m_historyVersion{~0ull};   // the draw-table version the history belongs to
    bool m_historyReset{true};
    bool m_captureRequested{false};

    // Capture (tests): host-visible copies
    rtc::HostBuffer m_captureBuffer{};
    VkDeviceSize m_captureBytes{0};
    uint32_t m_capturedDraws{0};
    bool m_captured{false};
};

} // namespace ohao
