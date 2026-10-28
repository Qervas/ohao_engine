#pragma once
// The decals pass (sub-project 12, "The frame", step 3): the frame's live decal records as boxes
// projected onto the G-buffer after gbuffer.late and before the lighting. Each record is a unit cube
// of decal space drawn as its back faces (vfx/decal.slang); a pixel is decaled only where the surface
// the G-buffer holds lies inside the box. The pass loads the albedo and normal targets (no clear),
// blends the albedo by the texture's alpha and replaces the normal and roughness where it is at least
// 0.5, and never writes the position, the emissive or the depth. Its pipelines and images are its
// own; the records are the frame's (RenderScene::vfx()'s decals, uploaded per slot).
//
// Nothing here reads a clock or the frame counter: the same records and camera give the same pixels.

#include "render/rt/rt_compute.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/scene/vfx_frame.hpp"
#include "gpu/shared_layout.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace ohao {

class DecalPass {
public:
    DecalPass() = default;
    ~DecalPass();
    DecalPass(const DecalPass&) = delete;
    DecalPass& operator=(const DecalPass&) = delete;

    // `textures`: the bindless set's layout (set 1). False when a pipeline cannot be made.
    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkDescriptorSetLayout textures);
    void cleanup();
    // The frame's size and the G-buffer images it draws over (albedo and normal, drawn in place) and
    // reads (position, depth). The device must be idle.
    [[nodiscard]] bool resize(uint32_t width, uint32_t height, VkImageView albedo, VkImageView normal, VkImageView position,
                              VkImageView depth);

    // This slot's decal instances from the frame's decal records, in record order (host; the slot's
    // fence waited). A record's box: the record's pose, scaled by its decal's size and depth.
    void prepare(uint32_t slot, const render::VfxFrame& frame);
    [[nodiscard]] uint32_t count(uint32_t slot) const noexcept { return slot < m_slots.size() ? m_slots[slot].count : 0u; }

    struct View {
        glm::mat4 viewProj{1.0f};   // the jittered view-projection the G-buffer was drawn with
        glm::vec3 camera{0.0f};
        float clearance{0.6f};      // m: no decal nearer the eye (the first-person gun's reach)
    };
    // Records the pass into `cmd` (its render pass loads and stores the G-buffer targets, leaving them
    // SHADER_READ_ONLY). Nothing without decals.
    void record(VkCommandBuffer cmd, uint32_t slot, const View& view, VkDescriptorSet textures);

    [[nodiscard]] bool ready() const noexcept { return m_pipeline != VK_NULL_HANDLE && m_framebuffer != VK_NULL_HANDLE; }

private:
    struct Slot {
        rtc::HostBuffer decals;
        VkDeviceSize bytes{0};
        uint32_t count{0};
        VkDescriptorSet set{VK_NULL_HANDLE};   // set 0: the records and the position and depth
    };

    void upload(uint32_t slot, std::span<const gpu::shared::DecalGpu> decals);
    [[nodiscard]] bool createLayouts(VkDescriptorSetLayout textures);
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createRenderPass();
    void writeSet(uint32_t slot, VkBuffer decals);
    void destroySizeResources();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physical{VK_NULL_HANDLE};
    uint32_t m_width{0}, m_height{0};
    VkImageView m_albedo{VK_NULL_HANDLE}, m_normal{VK_NULL_HANDLE}, m_position{VK_NULL_HANDLE}, m_depth{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> m_slots{};
};

} // namespace ohao
