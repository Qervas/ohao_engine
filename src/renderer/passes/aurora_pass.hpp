#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Aurora Borealis pass — animated ribbon effect on sky pixels.
// Two bindings: 0 = HDR storage image (GENERAL), 1 = depth sampler.
class AuroraPass : public RenderPassBase {
public:
    AuroraPass()  = default;
    ~AuroraPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "AuroraPass"; }

    void setHDROutput(VkImageView view, VkImage image);
    void setDepthView(VkImageView view, VkSampler sampler);

    void  setEnabled(bool v)      { m_enabled   = v; }
    bool  getEnabled() const      { return m_enabled; }
    void  setIntensity(float v)   { m_intensity = glm::clamp(v, 0.0f, 1.0f); }
    float getIntensity() const    { return m_intensity; }
    void  setHue(float v)         { m_hue       = glm::clamp(v, 0.0f, 1.0f); }
    void  setHeight(float v)      { m_height    = glm::clamp(v, 0.1f, 0.9f); }
    void  setTime(float t)        { m_time      = t; }

private:
    struct AuroraParams {
        float time;       //  0 (4)
        float intensity;  //  4 (4)
        float hue;        //  8 (4)
        float height;     // 12 (4)
    };
    static_assert(sizeof(AuroraParams) == 16, "AuroraParams must be 16 bytes");

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

    bool     m_enabled{false};
    float    m_intensity{0.5f};
    float    m_hue{0.0f};
    float    m_height{0.45f};
    float    m_time{0.0f};
    uint32_t m_width{1280};
    uint32_t m_height_px{720};
};

} // namespace ohao
