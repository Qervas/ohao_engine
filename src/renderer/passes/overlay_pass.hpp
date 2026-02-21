#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Overlay pass for grid, debug visualizations, and other screen-space overlays.
// Renders after post-processing tonemapping, composites on top of the final LDR image.
class OverlayPass : public RenderPassBase {
public:
    OverlayPass() = default;
    ~OverlayPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "OverlayPass"; }

    // Input configuration
    void setInputImage(VkImageView input) { m_inputView = input; }
    void setDepthBuffer(VkImageView depth) { m_depthView = depth; }
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       const glm::mat4& invViewProj, const glm::vec3& cameraPos);

    // Grid configuration
    void setGridEnabled(bool enabled) { m_gridEnabled = enabled; }
    void setGridMajorSpacing(float spacing) { m_majorSpacing = spacing; }
    void setGridMinorSpacing(float spacing) { m_minorSpacing = spacing; }
    void setGridFadeDistance(float distance) { m_fadeDistance = distance; }

    // Output
    VkImageView getOutputView() const { return m_outputView; }
    VkImage getOutputImage() const { return m_outputImage; }

private:
    bool createOutputImage();
    bool createRenderPass();
    bool createFramebuffer();
    bool createDescriptors();
    bool createPipeline();
    void destroyOutputImage();

    // Input views
    VkImageView m_inputView{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};

    // Output image (LDR with overlay composited)
    VkImage m_outputImage{VK_NULL_HANDLE};
    VkDeviceMemory m_outputMemory{VK_NULL_HANDLE};
    VkImageView m_outputView{VK_NULL_HANDLE};

    // Render pass and framebuffer
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Sampler
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Grid parameters
    bool m_gridEnabled{true};
    float m_majorSpacing{10.0f};
    float m_minorSpacing{1.0f};
    float m_fadeDistance{100.0f};

    // Camera data
    glm::mat4 m_invViewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};

    // Push constants (must match shader)
    struct GridPushConstants {
        glm::mat4 invViewProj;
        glm::vec4 cameraPos;    // xyz = camera position, w = unused
        glm::vec4 gridParams;   // x = major spacing, y = minor spacing, z = fade distance, w = line width
    };
};

} // namespace ohao
