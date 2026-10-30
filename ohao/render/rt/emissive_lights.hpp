#pragma once

// The path tracer's emissive triangle lights (frontier F3, ruling F-R7): every triangle of the
// instances with an emissive material (RenderScene::emissiveInstances) is a light, drawn by power.
// Built on the GPU every frame (RTXDI's light preparation): rt/emissive_collect.slang writes each
// triangle in world space and its power into level 0 of a power image, rt/emissive_reduce.slang sums
// the levels above to one texel, and the path tracer draws a light by descending the levels
// (rt/path_tracer.slang, sampleEmissive). Its bindings:
//   42 lights      4 uint4 a light (float bits): (corner 0, material), (corner 1, uv0.x), (corner 2, uv0.y), (uv1, uv2);
//                  the material an integer (as a float it would be a denormal, flushed to zero on the GPU)
//   43 power       R32F, a power-of-two square with every level; a light's texel over the top is its probability
//   44 ranges      [0] (N, S, levels, R), [1] (the analytic lights' power as float bits), [2 + r] (slot, first
//                  light, triangles, 0) of the R emissive instances
//   45 first       per instance slot, its first light (~0: none): a bounce that meets light first + primitive
// Ranges and first are host-written per frame slot; lights and power are rewritten each frame, after
// the previous frame's trace by queue order.

#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"

#include <array>
#include <span>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao::render {

class EmissiveLights {
public:
    // `textureCapacity`: the bindless table's size (the collect pass reads emissive textures).
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t textureCapacity);
    void destroy();

    // This frame's emissive instances (and the scene's slot count, for `first`). Grows the device
    // resources if they are too small (the device idles once), writes this slot's host buffers.
    void prepare(uint32_t frameSlot, const std::vector<RenderScene::EmissiveInstance>& instances, uint64_t version,
                 uint32_t slotEnd, float analyticPower);

    struct SceneBuffers {
        VkBuffer vertices, indices, meshes, triangleSlots, materials, instances, materialRefs;
    };
    // The collect and reduce passes (nothing without a light); `textures` the bindless table's
    // entries (the path tracer's binding 12), written into this slot's set.
    void record(VkCommandBuffer cmd, uint32_t frameSlot, const SceneBuffers& scene,
                std::span<const VkDescriptorImageInfo> textures);

    [[nodiscard]] uint32_t count() const noexcept { return m_count; }
    [[nodiscard]] VkBuffer lightBuffer() const noexcept { return m_lights; }
    [[nodiscard]] VkImageView powerView() const noexcept { return m_powerView; }
    [[nodiscard]] VkBuffer rangeBuffer(uint32_t frameSlot) const noexcept { return m_ranges[frameSlot].buffer; }
    [[nodiscard]] VkBuffer firstBuffer(uint32_t frameSlot) const noexcept { return m_first[frameSlot].buffer; }

private:
    bool createLights(uint32_t capacity);
    bool createPower(uint32_t side);
    void destroyPower();

    VkDevice m_device{VK_NULL_HANDLE};
    uint32_t m_textureCapacity{0};
    VkPhysicalDevice m_physical{VK_NULL_HANDLE};

    VkBuffer m_lights{VK_NULL_HANDLE};
    VkDeviceMemory m_lightsMemory{VK_NULL_HANDLE};
    uint32_t m_lightCapacity{0};

    VkImage m_power{VK_NULL_HANDLE};
    VkDeviceMemory m_powerMemory{VK_NULL_HANDLE};
    VkImageView m_powerView{VK_NULL_HANDLE};          // every level, sampled by the path tracer
    std::vector<VkImageView> m_levelViews;             // one a level, storage
    uint32_t m_side{0}, m_levels{0};
    bool m_powerFresh{true};                           // UNDEFINED until its first transition

    std::array<rtc::HostBuffer, MAX_FRAMES_IN_FLIGHT> m_ranges{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_rangeCapacity{};
    std::array<rtc::HostBuffer, MAX_FRAMES_IN_FLIGHT> m_first{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_firstCapacity{};
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_firstVersion{};   // the version and slot end each slot holds
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_firstSlotEnd{};

    uint32_t m_count{0};        // N this frame
    uint32_t m_frameSide{1};    // S this frame
    uint32_t m_frameLevels{1};

    VkDescriptorSetLayout m_collectSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_collectLayout{VK_NULL_HANDLE};
    VkPipeline m_collect{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_reduceSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_reduceLayout{VK_NULL_HANDLE};
    VkPipeline m_reduce{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_collectSets{};
    std::vector<VkDescriptorSet> m_reduceSets;   // one a level above 0 (written at createPower)
};

}  // namespace ohao::render
