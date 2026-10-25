#pragma once
// GPU culling (sub-project 2b spec, "GPU-driven geometry", "Meshlets and mesh shaders"):
// RenderScene's draw table to indirect draw lists, the same handful of commands whatever the
// instance count.
//
// A frame: cull(early) draws what the last late pass saw and the frustum holds; the early
// G-buffer pass draws it; buildHiZ() makes the max-depth pyramid of that depth; cull(late)
// tests every draw against the frustum and the pyramid, keeps those visible now and not drawn
// early, and records visibility for the next frame; the late G-buffer pass draws them. A CSM
// cascade culls its own frustum (shadow phase, one list, every draw on the instance path).
//
// A G-buffer phase writes four lists (gpu::shared::kCullListEarly / kCullListLate + 0..3):
// instance draws one-sided and double-sided (face culling is a pipeline per list), then the
// meshlet path's (geometry auto: draws of kMeshletMinTriangles or more). With task and mesh
// shaders a meshlet list is one task dispatch a draw, whose task shader culls the draw's
// meshlets (drawMeshTasks); without them, or with Settings::meshShaders off, cull() also tests
// every meshlet of the listed draws in compute and writes one VkDrawIndirectCommand a
// survivor (drawMeshletFallback). Both use cull/common.slang's test.
//
// Lists are compacted by prefix sum (cull/instances.slang): draw (and meshlet) order every
// run, no atomics. The visibility history is cleared by resetHistory() (a temporal reset) and
// whenever the draw table changes, so a capture depends only on the frames since its reset.
#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"
#include "gpu/shared_layout.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace ohao {

namespace render { class RenderScene; }

class GpuCulling {
public:
    // Which draws take the meshlet path (the spec's `geometry` setting).
    enum class Geometry : uint32_t {
        Instances = gpu::shared::kGeometryInstances,   // none
        Auto = gpu::shared::kGeometryAuto,             // kMeshletMinTriangles triangles or more
        Meshlets = gpu::shared::kGeometryMeshlets,     // every draw with meshlets
    };

    struct Settings {
        bool frustum{true};     // false: every draw is drawn (in the late pass, in draw order), every meshlet
        bool occlusion{true};   // the late pass's Hi-Z test (draws and meshlets)
        Geometry geometry{Geometry::Auto};
        bool meshShaders{true};   // false: the compute fallback even where task/mesh shaders exist
    };

    // What the last captured frame's culling wrote (setCapture): its counts, the visibility
    // history after its late pass, its draw commands (5 uints each, list by list, drawCount a
    // list) and, when the fallback ran, its per-meshlet commands (4 uints each, list by list,
    // meshletEntries a list).
    struct Capture {
        uint32_t drawCount{0};
        uint32_t meshletEntries{0};
        std::vector<uint32_t> counts;            // gpu::shared::kCullCounts
        std::vector<uint32_t> visibility;        // drawCount
        std::vector<uint32_t> commands;          // kCullLists * drawCount * 5
        std::vector<uint32_t> meshletCommands;   // kCullMeshletLists * meshletEntries * 4
    };

    GpuCulling() = default;
    ~GpuCulling();
    GpuCulling(const GpuCulling&) = delete;
    GpuCulling& operator=(const GpuCulling&) = delete;

    // `meshShaders`: VK_EXT_mesh_shader is enabled on the device.
    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, bool meshShaders);
    void cleanup();
    // The pyramid follows the frame's size; `depth` is the G-buffer depth view it is built
    // from (read in DEPTH_STENCIL_READ_ONLY_OPTIMAL). The device must be idle.
    bool resize(uint32_t width, uint32_t height, VkImageView depth);

    void setSettings(const Settings& s) noexcept { m_settings = s; }
    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }
    // Task and mesh shaders draw the meshlet lists (supported and not turned off).
    [[nodiscard]] bool meshShadersActive() const noexcept { return m_meshShaderSupported && m_settings.meshShaders; }
    // Forget which draws were visible (the next early pass draws nothing).
    void resetHistory() noexcept { m_historyReset = true; }

    // Start of a frame's culling, outside any render pass: binds the scene's tables to the
    // slot's set and clears the history when it is stale. False when there is nothing to
    // draw (no draw table yet): the passes then draw nothing.
    bool beginFrame(VkCommandBuffer cmd, uint32_t slot, const render::RenderScene& scene);
    // One phase over the draw table into lists firstList.. (listCount: 4 for a G-buffer phase,
    // 1 for a cascade), with the view-projection the draws are rasterized with and the
    // camera's position (the cone test).
    void cull(VkCommandBuffer cmd, uint32_t phase, uint32_t firstList, uint32_t listCount, const glm::mat4& viewProj,
              const glm::vec3& camera);
    // The pyramid of the depth the early pass left (DEPTH_STENCIL_READ_ONLY_OPTIMAL).
    void buildHiZ(VkCommandBuffer cmd);
    // vkCmdDrawIndexedIndirectCount over instance list `list` (the caller binds the pipeline,
    // the vertex and index arenas and its sets). Nothing when the frame has no draws.
    void drawList(VkCommandBuffer cmd, uint32_t list) const;
    // vkCmdDrawMeshTasksIndirectCountEXT over meshlet list `list` (meshShadersActive()).
    void drawMeshTasks(VkCommandBuffer cmd, uint32_t list) const;
    // vkCmdDrawIndirectCount over the fallback's per-meshlet list `meshletList`
    // (0..kCullMeshletLists-1: early one-sided, early double-sided, late one-sided, late
    // double-sided); nothing when the fallback did not run this frame.
    void drawMeshletFallback(VkCommandBuffer cmd, uint32_t meshletList) const;
    [[nodiscard]] uint32_t drawCount() const noexcept { return m_drawCount; }

    // What the G-buffer's meshlet shaders bind: the commands (the task shader reads its draw
    // from them), the pyramid and its shape. A list's first command is at uint
    // list * drawCount() * 5.
    [[nodiscard]] VkBuffer commandBuffer() const noexcept { return m_commands.buffer; }
    [[nodiscard]] VkImageView hizView() const noexcept { return m_hiz.view; }
    [[nodiscard]] uint32_t hizLevels() const noexcept { return m_hizLevels; }
    // The flags a meshlet test gets in `phase` (kCullFrustum, kCullCone, kCullOcclusion).
    [[nodiscard]] uint32_t meshletFlags(uint32_t phase) const noexcept;

    // Tests: while on, every frame copies its culling buffers out (after its late pass).
    void setCapture(bool on) noexcept { m_captureRequested = on; }
    // The last captured frame's buffers; call once that frame's fence has signalled.
    [[nodiscard]] Capture capture() const;

private:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
    };
    bool ensureCapacity(uint32_t draws, uint32_t entries);
    void destroyBuffers();
    void destroyHiZ();
    bool createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& out);
    void recordCapture(VkCommandBuffer cmd);
    // The fallback's meshlet passes for a G-buffer phase: after its draw scatter.
    void cullMeshlets(VkCommandBuffer cmd, uint32_t phase, const gpu::shared::CullPush& drawPush);
    [[nodiscard]] bool fallbackRuns() const noexcept {
        return !meshShadersActive() && m_settings.geometry != Geometry::Instances && m_entryCount > 0;
    }

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    Settings m_settings;
    bool m_meshShaderSupported{false};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT m_drawMeshTasksIndirectCount{nullptr};
    VkPipelineStageFlags m_taskMeshStages{0};   // TASK | MESH when supported, else none

    // cull/instances.slang: one set layout for every entry, one set a frame slot
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_classify{VK_NULL_HANDLE}, m_scanBlocks{VK_NULL_HANDLE}, m_scatter{VK_NULL_HANDLE};
    VkPipeline m_classifyMeshlets{VK_NULL_HANDLE}, m_scatterMeshlets{VK_NULL_HANDLE};

    // Per draw (m_capacity draws) and per meshlet entry (m_entryCapacity), device-local
    uint32_t m_capacity{0};
    uint32_t m_entryCapacity{0};
    Buffer m_visibility, m_local, m_blockSums, m_blockOffsets, m_counts, m_commands;
    Buffer m_meshletLocal, m_meshletCommands;

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
    uint32_t m_entryCount{0};      // the draw table's meshlets
    bool m_fallbackDrawn{false};   // this frame's fallback lists were written
    uint64_t m_historyVersion{~0ull};   // the draw-table version the history belongs to
    bool m_historyReset{true};
    bool m_captureRequested{false};

    // Capture (tests): host-visible copies
    rtc::HostBuffer m_captureBuffer{};
    VkDeviceSize m_captureBytes{0};
    uint32_t m_capturedDraws{0}, m_capturedEntries{0};
    bool m_captured{false};
};

} // namespace ohao
