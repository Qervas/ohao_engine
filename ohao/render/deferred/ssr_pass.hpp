#pragma once

#include "render_pass_base.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/shared_layout.hpp"

#include <array>

namespace ohao {

class GBufferPass;

// Screen-space reflections (sub-project 2b spec, "Screen-space passes and order"), after
// deferred lighting, SSS and the sky, before post: a trace at half resolution
// (postprocess/ssr.slang), then a full-resolution resolve (postprocess/ssr_resolve.slang) that
// replaces, in the lit image in place, the environment's reflection with the traced one where it
// hit, under the lighting pass's reflection weight: reflected light is counted once.
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
    // The lit image (RGBA16F, SHADER_READ_ONLY, storage usage) the trace reads and the resolve
    // rewrites, and the lighting pass's environment specular and reflection weight beside it.
    void setLitScene(VkImage image, VkImageView view, VkImageView envSpecular, VkImageView specularWeight) {
        m_litImage = image;
        m_litSceneView = view;
        m_envSpecularView = envSpecular;
        m_specularWeightView = specularWeight;
    }
    void setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos);

    // The trace, half resolution rounded up: rgb the reflected radiance times the confidence c,
    // a c (0: the environment's reflection stays).
    [[nodiscard]] VkImageView getOutputView() const { return m_output.view; }
    [[nodiscard]] VkExtent2D outputExtent() const { return {m_output.width, m_output.height}; }
    [[nodiscard]] VkExtent2D extent() const { return {m_width, m_height}; }   // the frame the pass follows

private:
    [[nodiscard]] bool createOutputImage();
    [[nodiscard]] bool createSetLayout(const VkDescriptorSetLayoutBinding* bindings, uint32_t count,
                                       VkDescriptorSetLayout& layout,
                                       std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& sets);

    GBufferPass* m_gbuffer{nullptr};
    VkImage m_litImage{VK_NULL_HANDLE};
    VkImageView m_litSceneView{VK_NULL_HANDLE};
    VkImageView m_envSpecularView{VK_NULL_HANDLE};
    VkImageView m_specularWeightView{VK_NULL_HANDLE};
    rtc::Image m_output{};

    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkPipeline m_resolvePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_resolvePipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_resolveDescriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    // One set per frame in flight for each entry: execute() rewrites its slot's sets at record time.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_resolveSets{};
    VkSampler m_sampler{VK_NULL_HANDLE};

    uint32_t m_width{0}, m_height{0};
    gpu::shared::SSRPush m_params{};
    gpu::shared::SSRResolvePush m_resolveParams{};
};

} // namespace ohao
