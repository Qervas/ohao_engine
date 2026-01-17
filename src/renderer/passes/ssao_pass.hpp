#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Screen-Space Ambient Occlusion (GTAO) compute pass
class SSAOPass : public RenderPassBase {
public:
    SSAOPass() = default;
    ~SSAOPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SSAOPass"; }

    // Configuration
    void setDepthBuffer(VkImageView depth);
    void setNormalBuffer(VkImageView normal);
    void setProjectionMatrix(const glm::mat4& proj, const glm::mat4& invProj);
    void updateDescriptorSet();

    void setRadius(float radius) { m_radius = radius; }
    void setBias(float bias) { m_bias = bias; }
    void setIntensity(float intensity) { m_intensity = intensity; }
    void setSampleCount(uint32_t count) { m_sampleCount = count; }

    // Get output
    VkImageView getOutputView() const { return m_aoOutputView; }
    VkSampler getSampler() const { return m_sampler; }

private:
    bool createOutputImage();
    bool createNoiseTexture();
    bool createDescriptors();
    bool createPipeline();
    void destroyOutputImage();

    // Input buffers
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImageView m_normalView{VK_NULL_HANDLE};

    // AO output
    VkImage m_aoOutput{VK_NULL_HANDLE};
    VkDeviceMemory m_aoMemory{VK_NULL_HANDLE};
    VkImageView m_aoOutputView{VK_NULL_HANDLE};

    // Noise texture (4x4 for randomization)
    VkImage m_noiseImage{VK_NULL_HANDLE};
    VkDeviceMemory m_noiseMemory{VK_NULL_HANDLE};
    VkImageView m_noiseView{VK_NULL_HANDLE};

    // Compute pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Samplers
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkSampler m_noiseSampler{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Parameters
    float m_radius{0.5f};
    float m_bias{0.025f};
    float m_intensity{1.0f};
    uint32_t m_sampleCount{8};
    float m_falloffStart{50.0f};
    float m_falloffEnd{300.0f};

    // Matrices
    glm::mat4 m_projection;
    glm::mat4 m_invProjection;

    // Push constants (must match shader)
    struct SSAOParams {
        glm::mat4 projection;
        glm::mat4 invProjection;
        glm::vec4 noiseScale;  // xy = noise scale, zw = screen size
        float radius;
        float bias;
        float intensity;
        uint32_t sampleCount;
        glm::vec2 texelSize;
        float falloffStart;
        float falloffEnd;
    };
};

} // namespace ohao
