#pragma once

#include "render_pass_base.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"
#include "render/frame/frame_resources.hpp"

#include <array>

namespace ohao {

class GBufferPass;

// Subsurface scattering (sub-project 2b spec, "Screen-space passes and order"): a separable,
// depth-aware blur of the lit image on the 16x16 tiles the G-buffer pass flagged for materials
// with `subsurface` > 0 (postprocess/sss.slang), two indirect dispatches. It writes the lit
// image in place: everything after it (SSR, post) reads the lighting output. A frame with no
// subsurface material dispatches no workgroup.
class SSSPass : public RenderPassBase {
public:
    SSSPass() = default;
    ~SSSPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override { record(render::Frame{cmd, nullptr, frameIndex}); }
    // The two blurs, untimed (the frame times them as "sss").
    void record(const render::Frame& frame);
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "SSSPass"; }

    void setGBufferPass(GBufferPass* gbuffer) { m_gbuffer = gbuffer; }
    void setLitImage(gpu::Image* lit) noexcept { m_lit = lit; }
    void setCameraPos(const glm::vec3& p) noexcept { m_cameraPos = p; }
    // The lighting pass's subsurface diffuse light: the only light the pass spreads.
    void setSubsurfaceDiffuse(gpu::Image* diffuse) noexcept { m_diffuse = diffuse; }
    // RenderScene's material table (each material's subsurface free path and colour).
    void setMaterials(GpuBuffer* materials) noexcept { m_materials = materials; }
    // The kernel's step in pixels (3 steps each way), at most 10: the vertical blur reaches no
    // further than the tile rows the horizontal one covers.
    void setSSSWidth(float w) { m_sssWidth = glm::clamp(w, 0.0f, 10.0f); }

    [[nodiscard]] VkExtent2D extent() const { return {m_width, m_height}; }   // the frame the pass follows

private:
    bool createScratch();

    GBufferPass* m_gbuffer{nullptr};
    gpu::Image* m_lit{nullptr};
    gpu::Image* m_diffuse{nullptr};
    GpuBuffer* m_materials{nullptr};
    glm::vec3 m_cameraPos{0.0f};
    float m_sssWidth{8.0f};

    gpu::Image m_scratch{};   // the horizontal pass's output, full size (only flagged tiles written)

    VkPipeline m_hPipeline{VK_NULL_HANDLE}, m_vPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_hLayout{VK_NULL_HANDLE}, m_vLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_hSetLayout{VK_NULL_HANDLE}, m_vSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_hSets{}, m_vSets{};

    uint32_t m_width{0}, m_height{0};
};

} // namespace ohao
