#pragma once

#include "render_pass_base.hpp"

namespace ohao {

class GBufferPass;

// Screen-Space Reflections — ray-march the depth buffer for glossy reflections.
// Runs as a compute pass after deferred lighting, blended into final output.
class SSRPass : public RenderPassBase {
public:
    SSRPass() = default;
    ~SSRPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SSRPass"; }

    void setGBufferPass(GBufferPass* gbuffer) { m_gbuffer = gbuffer; }
    void setLitSceneView(VkImageView view) { m_litSceneView = view; }
    void setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos);

    VkImageView getOutputView() const { return m_outputView; }

private:
    bool createOutputImage();
    bool createComputePipeline();
    bool createDescriptors();

    GBufferPass* m_gbuffer{nullptr};
    VkImageView m_litSceneView{VK_NULL_HANDLE};

    // Output
    VkImage m_output{VK_NULL_HANDLE};
    VkDeviceMemory m_outputMem{VK_NULL_HANDLE};
    VkImageView m_outputView{VK_NULL_HANDLE};

    // Compute pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};

    uint32_t m_width{0}, m_height{0};

    struct SSRPushConstants {
        glm::mat4 viewProj;
        glm::mat4 invViewProj;
        glm::vec4 cameraPos;
        glm::vec2 screenSize;
        float maxDistance;
        float thickness;
    };

    SSRPushConstants m_params{};
};

} // namespace ohao
