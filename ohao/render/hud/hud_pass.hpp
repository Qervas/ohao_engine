#pragma once
// hud (sub-project 7 spec, "Presentation"): the frame's HudList drawn over the final RGBA8 image
// (after tonemapping and the gizmo, before capture.preview, present and readback). One instanced
// draw, six vertices an item (shaders/hud/hud.slang), blended in list order; the items go to a
// host-visible buffer of the frame slot's own, the font once. A frame with an empty list records
// nothing.
#include "render/deferred/render_pass_base.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/hud/hud_list.hpp"
#include "render/rt/rt_compute.hpp"

#include <array>
#include <span>

namespace ohao {

class HudPass : public RenderPassBase {
public:
    HudPass() = default;
    ~HudPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "HudPass"; }

    // The items the next frames draw (copied; at most kHudMaxItems).
    void setItems(std::span<const render::HudItem> items);
    [[nodiscard]] bool hasItems() const noexcept { return !m_items.empty(); }
    // The final RGBA8 image (SHADER_READ_ONLY_OPTIMAL, written by a render pass or compute): drawn
    // into in place, left in SHADER_READ_ONLY_OPTIMAL.
    void setTarget(VkImage image, VkImageView view, uint32_t width, uint32_t height);

private:
    [[nodiscard]] bool createPipeline();
    void destroyFramebuffer();

    std::vector<render::HudItem> m_items;
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    std::array<rtc::HostBuffer, MAX_FRAMES_IN_FLIGHT> m_itemBuffers{};
    rtc::HostBuffer m_font{};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkImage m_targetImage{VK_NULL_HANDLE};
    VkImageView m_targetView{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    uint32_t m_width{0}, m_height{0};
};

} // namespace ohao
