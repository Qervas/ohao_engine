#pragma once

#include "render_pass_base.hpp"

namespace ohao {

class GBufferPass;

// Separable Subsurface Scattering — two-pass (H+V) depth-aware blur on skin pixels.
// Runs after deferred lighting, blurs the HDR output for soft skin rendering.
class SSSPass : public RenderPassBase {
public:
    SSSPass() = default;
    ~SSSPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SSSPass"; }

    void setGBufferPass(GBufferPass* gbuffer) { m_gbuffer = gbuffer; }
    void setLitSceneView(VkImageView view) { m_litSceneView = view; }
    void setSSSWidth(float w) { m_sssWidth = w; }

    VkImageView getOutputView() const { return m_outputView; }
    VkImage getOutputImage() const { return m_output; }

private:
    bool createImages();
    bool createComputePipeline();
    bool createDescriptors();
    void executePass(VkCommandBuffer cmd, VkImageView input, VkImage inputImg,
                     VkImageView output, VkImage outputImg, glm::vec2 direction);

    GBufferPass* m_gbuffer{nullptr};
    VkImageView m_litSceneView{VK_NULL_HANDLE};
    float m_sssWidth{8.0f};

    // Ping-pong images for two-pass blur
    VkImage m_temp{VK_NULL_HANDLE};
    VkDeviceMemory m_tempMem{VK_NULL_HANDLE};
    VkImageView m_tempView{VK_NULL_HANDLE};

    VkImage m_output{VK_NULL_HANDLE};
    VkDeviceMemory m_outputMem{VK_NULL_HANDLE};
    VkImageView m_outputView{VK_NULL_HANDLE};

    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descSetH{VK_NULL_HANDLE};  // horizontal pass
    VkDescriptorSet m_descSetV{VK_NULL_HANDLE};  // vertical pass
    VkSampler m_sampler{VK_NULL_HANDLE};

    uint32_t m_width{0}, m_height{0};

    struct SSSPushConstants {
        glm::vec2 direction;
        glm::vec2 screenSize;
        float sssWidth;
        float padding;
    };
};

} // namespace ohao
