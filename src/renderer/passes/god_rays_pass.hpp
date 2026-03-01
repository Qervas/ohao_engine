#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// God Rays pass — radial light shafts from the sun.
// Runs after sky pass, reads depth to detect sky pixels, additively blends.
// Two bindings: 0 = HDR storage image (GENERAL), 1 = depth sampler.
class GodRaysPass : public RenderPassBase {
public:
    GodRaysPass()  = default;
    ~GodRaysPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "GodRaysPass"; }

    void setHDROutput(VkImageView view, VkImage image);
    void setDepthView(VkImageView view, VkSampler sampler);

    void  setEnabled(bool v)         { m_enabled   = v; }
    bool  getEnabled() const         { return m_enabled; }
    void  setIntensity(float v)      { m_intensity = glm::clamp(v, 0.0f, 2.0f); }
    float getIntensity() const       { return m_intensity; }
    void  setSunScreenPos(glm::vec2 p) { m_sunScreenPos = p; }
    void  setTime(float t)           { m_time      = t; }

private:
    struct GodRayParams {
        glm::vec2 sunScreenPos; //  0 (8)
        float intensity;        //  8 (4)
        float decay;            // 12 (4)
        float sunRadius;        // 16 (4)
        float exposure;         // 20 (4)
        int   numSamples;       // 24 (4)
        float padding;          // 28 (4)
    };
    static_assert(sizeof(GodRayParams) == 32, "GodRayParams must be 32 bytes");

    bool createDescriptors();
    bool createPipelineResources();
    void updateDescriptors();

    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};
    VkSampler             m_depthSampler{VK_NULL_HANDLE};
    bool                  m_ownsSampler{false};

    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkImage     m_hdrImage{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};
    bool        m_descriptorDirty{false};

    bool      m_enabled{true};
    float     m_intensity{1.0f};
    glm::vec2 m_sunScreenPos{0.5f, 0.25f};
    float     m_time{0.0f};
    uint32_t  m_width{1280};
    uint32_t  m_height{720};
};

} // namespace ohao
