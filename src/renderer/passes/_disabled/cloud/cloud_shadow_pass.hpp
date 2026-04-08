#pragma once

#include "renderer/passes/render_pass_base.hpp"
#include <cstdint>

namespace ohao {

// Cloud shadow map pass — top-down vertical march through the cloud slab
// to produce a 1024x1024 R16F shadow factor [0..1].
// 0 = fully shadowed, 1 = clear sky.
// Deferred lighting samples this to darken surfaces under clouds.
class CloudShadowPass : public RenderPassBase {
public:
    CloudShadowPass() = default;
    ~CloudShadowPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t /*width*/, uint32_t /*height*/) override {}
    const char* getName() const override { return "CloudShadowPass"; }
    bool reloadShader(const std::string& spvPath) override;

    // --- Inputs (set before execute) ---
    void setNoiseTexture(VkImageView view, VkSampler sampler);
    void setWeatherTexture(VkImageView view, VkSampler sampler);
    void setSunDirection(const glm::vec3& dir) { m_sunDir = dir; }
    void setCameraPos(const glm::vec3& pos);
    void setTime(float t) { m_time = t; }

    // --- Cloud parameters (forwarded from DeferredRenderer) ---
    void setCloudParams(float altMin, float altMax, float coverage,
                        float density, float absorption, float speed);

    // --- Output ---
    VkImageView getOutputView() const  { return m_shadowOutput.view; }
    VkImage     getOutputImage() const { return m_shadowOutput.image; }
    VkSampler   getOutputSampler() const { return m_shadowSampler; }

    // Shadow map world-space extents (for deferred lighting UV calculation)
    glm::vec2 getMapCenter() const { return m_mapCenter; }
    glm::vec2 getMapExtent() const { return m_mapExtent; }

private:
    bool createShadowOutput();
    bool createDescriptors();
    bool createPipelineResources();
    void updateDescriptors();

    // Shadow map image (1024^2, R16F, GENERAL layout)
    static constexpr uint32_t SHADOW_MAP_SIZE = 1024;
    RenderTarget m_shadowOutput;
    VkSampler    m_shadowSampler{VK_NULL_HANDLE};
    bool         m_shadowOutputReady{false};

    // Compute pipeline
    VkPipeline       m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};
    bool                  m_descriptorsDirty{true};

    // External texture references (not owned)
    VkImageView m_noiseView{VK_NULL_HANDLE};
    VkSampler   m_noiseSampler{VK_NULL_HANDLE};
    VkImageView m_weatherView{VK_NULL_HANDLE};
    VkSampler   m_weatherSampler{VK_NULL_HANDLE};

    // Per-frame state
    float     m_time{0.0f};
    glm::vec3 m_sunDir{0.3f, 0.9f, 0.3f};
    glm::vec2 m_mapCenter{0.0f, 0.0f};
    glm::vec2 m_mapExtent{4096.0f, 4096.0f};

    // Cloud parameters
    float m_altMin{1500.0f};
    float m_altMax{8000.0f};
    float m_coverage{0.5f};
    float m_density{0.45f};
    float m_absorption{0.15f};
    float m_speed{1.0f};

    // Push constants (80 bytes — must match cloud_shadow.comp)
    struct ShadowParams {
        glm::vec2 mapCenter;       //  0
        glm::vec2 mapExtent;       //  8
        glm::vec2 outputSize;      // 16
        float     cloudAltMin;     // 24
        float     cloudAltMax;     // 28  -> 32
        float     cloudCoverage;   // 32
        float     cloudDensity;    // 36
        float     cloudAbsorption; // 40
        float     time;            // 44  -> 48
        float     cloudSpeed;      // 48
        float     pad0;            // 52  -> 56
        glm::vec4 sunDirectionAndPad;  // 56  (16) -> 72  — xyz=sunDir, w=unused
        float     pad2;               // 72
        float     pad3;               // 76  -> 80
    };
    static_assert(sizeof(ShadowParams) == 80, "ShadowParams must be 80 bytes");
};

} // namespace ohao
