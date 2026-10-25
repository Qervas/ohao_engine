#pragma once
// Clustered lighting (sub-project 2b spec, "Clustered lighting"): the raster light table on the
// GPU and every froxel's light list, rebuilt each frame by lights.cluster
// (shaders/compute/light_cluster.slang); core/deferred_lighting.frag sums a pixel's froxel's
// lights.
//
// The light table is one host-visible storage buffer per frame slot (LightGpu records): the
// slot's buffer is rewritten only after its fence, and only when the lights changed since it
// was last written (a version), so a still scene of thousands of lights uploads nothing. It
// grows to the frame's light count (at most gpu::shared::kLightCapacity). The froxel lists are
// one device-local buffer (kClusterCount counts, then kClusterMaxLights indices a froxel). Each
// slot has a host-visible stats word triple: the lights dropped by full froxels, the froxels
// that dropped any, the largest froxel count; latestOverflow() is the newest finished frame's.
#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene_types.hpp"
#include "gpu/shared_layout.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ohao {

class LightClusters {
public:
    LightClusters() = default;
    ~LightClusters();
    LightClusters(const LightClusters&) = delete;
    LightClusters& operator=(const LightClusters&) = delete;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();

    // This frame's lights into the slot's buffer, after the slot's fence: skipped when the slot
    // already holds `version` (0: always written). Lights past kLightCapacity are dropped.
    void setLights(uint32_t slot, std::span<const LightData> lights, uint64_t version);
    // The froxel lists for the camera the lighting pass shades with: `view`, the projection the
    // G-buffer was rasterized with (jitter included), the frame's size and the far plane; the
    // renderer's frame number labels the overflow. Outside any render pass.
    void build(VkCommandBuffer cmd, uint32_t slot, const glm::mat4& view, const glm::mat4& proj, uint32_t width,
               uint32_t height, float farPlane, uint64_t frameNumber);

    [[nodiscard]] VkBuffer lightBuffer(uint32_t slot) const noexcept {
        return slot < MAX_FRAMES_IN_FLIGHT ? m_slots[slot].lights.buffer : VK_NULL_HANDLE;
    }
    [[nodiscard]] const gpu::shared::LightGpu* lights(uint32_t slot) const noexcept {
        return slot < MAX_FRAMES_IN_FLIGHT ? static_cast<const gpu::shared::LightGpu*>(m_slots[slot].lights.mapped)
                                           : nullptr;
    }
    [[nodiscard]] uint32_t lightCount(uint32_t slot) const noexcept {
        return slot < MAX_FRAMES_IN_FLIGHT ? m_slots[slot].count : 0u;
    }
    [[nodiscard]] VkBuffer clusterBuffer() const noexcept { return m_clusters.buffer; }
    // The lighting pass's slice mapping (ClusterPush.screen.zw) for the last build.
    [[nodiscard]] float sliceScale() const noexcept { return m_sliceScale; }
    [[nodiscard]] float sliceBias() const noexcept { return m_sliceBias; }

    struct Overflow {
        uint64_t frame{0};
        uint32_t lights{0};     // dropped by froxels past kClusterMaxLights
        uint32_t clusters{0};   // froxels that dropped any
        uint32_t maxLights{0};  // the largest froxel's count, before the cut
    };
    // Reads every slot whose frame has finished (`fenceSignalled(slot)`).
    template <class FenceSignalled>
    void collect(FenceSignalled&& fenceSignalled) {
        for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
            if (m_slots[s].pending && fenceSignalled(s)) collectSlot(s);
    }
    [[nodiscard]] std::optional<Overflow> latestOverflow() const noexcept { return m_latest; }

    // Tests: the froxel buffer copied out after each build while on; read once that frame's
    // fence has signalled (counts, then lists, as the buffer holds them).
    void setCapture(bool on) noexcept { m_captureRequested = on; }
    [[nodiscard]] std::vector<uint32_t> capture() const;

private:
    struct Slot {
        rtc::HostBuffer lights{};
        uint32_t capacity{0};   // LightGpu records
        uint32_t count{0};
        uint64_t version{0};
        rtc::HostBuffer stats{};
        bool pending{false};
        uint64_t frame{0};
    };
    void collectSlot(uint32_t slot);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> m_slots{};
    struct DeviceBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
    } m_clusters;
    float m_sliceScale{0.0f}, m_sliceBias{0.0f};
    std::optional<Overflow> m_latest;
    bool m_captureRequested{false};
    rtc::HostBuffer m_captureBuffer{};
    bool m_captured{false};
};

} // namespace ohao
