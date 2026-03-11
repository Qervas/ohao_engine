#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Screen-Space Global Illumination (SSGI) compute pass
// Half-resolution: ray-marches depth buffer, samples albedo at hit points
// to produce single-bounce indirect lighting (color bleeding).
class SSGIPass : public RenderPassBase {
public:
    SSGIPass() = default;
    ~SSGIPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SSGIPass"; }
    bool reloadShader(const std::string& spvPath) override;

    // Input buffers (full resolution)
    void setDepthBuffer(VkImageView depth);
    void setNormalBuffer(VkImageView normal);
    void setAlbedoBuffer(VkImageView albedo);
    void setPositionBuffer(VkImageView position);
    void updateDescriptorSet();

    // Matrices
    void setMatrices(const glm::mat4& view, const glm::mat4& proj, const glm::mat4& invProj);

    // Parameters
    void setRadius(float radius) { m_radius = radius; }
    void setIntensity(float intensity) { m_intensity = intensity; }
    void setSampleCount(uint32_t count) { m_sampleCount = count; }

    // Get output (half resolution RGBA16F)
    VkImageView getOutputView() const { return m_giOutputView; }
    VkImage getOutputImage() const { return m_giOutput; }
    VkSampler getSampler() const { return m_sampler; }

private:
    bool createOutputImage();
    bool createNoiseTexture();
    bool createDescriptors();
    bool createPipeline();
    void destroyOutputImage();

    // Input views (full resolution)
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImageView m_normalView{VK_NULL_HANDLE};
    VkImageView m_albedoView{VK_NULL_HANDLE};
    VkImageView m_positionView{VK_NULL_HANDLE};

    // GI output (half resolution)
    VkImage m_giOutput{VK_NULL_HANDLE};
    VkDeviceMemory m_giMemory{VK_NULL_HANDLE};
    VkImageView m_giOutputView{VK_NULL_HANDLE};

    // Noise texture (4x4)
    VkImage m_noiseImage{VK_NULL_HANDLE};
    VkDeviceMemory m_noiseMemory{VK_NULL_HANDLE};
    VkImageView m_noiseView{VK_NULL_HANDLE};

    // Staging buffer for deferred noise upload
    VkBuffer m_noiseStagingBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_noiseStagingMemory{VK_NULL_HANDLE};
    bool m_noiseUploaded{false};
    void uploadNoiseTexture(VkCommandBuffer cmd);

    // Compute pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Samplers
    VkSampler m_sampler{VK_NULL_HANDLE};       // Linear, clamp (for output + gbuffer)
    VkSampler m_noiseSampler{VK_NULL_HANDLE};  // Nearest, repeat (for noise)

    // Full resolution dimensions (output is half)
    uint32_t m_fullWidth{0};
    uint32_t m_fullHeight{0};

    // Parameters
    float m_radius{3.0f};
    float m_intensity{1.0f};
    uint32_t m_sampleCount{4};
    uint32_t m_maxSteps{8};
    float m_thickness{0.5f};
    float m_falloff{1.0f};

    // Matrices
    glm::mat4 m_view;
    glm::mat4 m_projection;
    glm::mat4 m_invProjection;

    // Push constants (must match shader layout, <=256 bytes)
    struct SSGIParams {
        glm::mat4 view;           // 64
        glm::mat4 projection;     // 64
        glm::mat4 invProjection;  // 64
        glm::vec4 screenParams;   // xy = full res, zw = half res  // 16
        float radius;             // 4
        float intensity;          // 4
        uint32_t sampleCount;     // 4
        uint32_t maxSteps;        // 4
        glm::vec2 texelSize;      // 8
        float thickness;          // 4
        float falloff;            // 4
    };                            // Total: 240 bytes
};

} // namespace ohao
