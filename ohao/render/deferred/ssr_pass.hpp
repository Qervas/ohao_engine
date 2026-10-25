#pragma once

#include "render_pass_base.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/shared_layout.hpp"

#include <array>

namespace ohao {

class GBufferPass;

// Screen-space reflections (sub-project 2b spec, "Screen-space passes and order"): a compute
// pass at half resolution (postprocess/ssr.slang) after deferred lighting; post's tonemap adds
// it, upsampled with depth awareness.
class SSRPass : public RenderPassBase {
public:
    SSRPass() = default;
    ~SSRPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "SSRPass"; }

    void setGBufferPass(GBufferPass* gbuffer) { m_gbuffer = gbuffer; }
    void setLitSceneView(VkImageView view) { m_litSceneView = view; }
    void setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos);

    // Half resolution, rounded up: rgb the reflection, a the pixel's view depth (0 for sky).
    [[nodiscard]] VkImageView getOutputView() const { return m_output.view; }
    [[nodiscard]] VkExtent2D outputExtent() const { return {m_output.width, m_output.height}; }
    [[nodiscard]] VkExtent2D extent() const { return {m_width, m_height}; }   // the frame the pass follows

private:
    [[nodiscard]] bool createOutputImage();

    GBufferPass* m_gbuffer{nullptr};
    VkImageView m_litSceneView{VK_NULL_HANDLE};
    rtc::Image m_output{};

    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    // One set per frame in flight: execute() rewrites its slot's set at record time.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};
    VkSampler m_sampler{VK_NULL_HANDLE};

    uint32_t m_width{0}, m_height{0};
    gpu::shared::SSRPush m_params{};
};

} // namespace ohao
