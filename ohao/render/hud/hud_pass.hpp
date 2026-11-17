#pragma once
// hud (sub-project 7 spec, "Presentation"): the frame's HudList drawn over the final RGBA8 image
// (after tonemapping and the gizmo, before capture.preview, present and readback). One instanced
// draw, six vertices an item (shaders/hud/hud.slang), blended in list order; the items go to a
// host-visible buffer of the frame slot's own, the font once. A frame with an empty list records
// nothing.
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/deferred/render_pass_base.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/pass.hpp"
#include "render/hud/hud_list.hpp"

#include <array>
#include <span>

namespace ohao {

class HudPass : public RenderPassBase {
public:
    HudPass() = default;
    ~HudPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer, uint32_t) override {}   // record() draws: the pass needs its target
    // The items blended over `target` (the final RGBA8 image) in place, timed as hud.
    void record(const render::Frame& frame, gpu::Image& target);
    [[nodiscard]] const char* getName() const override { return "HudPass"; }

    // The items the next frames draw (copied; at most kHudMaxItems).
    void setItems(std::span<const render::HudItem> items);
    [[nodiscard]] bool hasItems() const noexcept { return !m_items.empty(); }

private:
    [[nodiscard]] bool createPipeline();

    std::vector<render::HudItem> m_items;
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    std::array<GpuBuffer, MAX_FRAMES_IN_FLIGHT> m_itemBuffers{};
    GpuBuffer m_font;
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
};

} // namespace ohao
