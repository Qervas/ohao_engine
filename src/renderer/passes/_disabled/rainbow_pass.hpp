#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Rainbow pass — prismatic arc on sky pixels opposite the sun.
// Active when rain is falling. Two bindings: 0 = HDR storage, 1 = depth sampler.
class RainbowPass : public RenderPassBase {
public:
    RainbowPass()  = default;
    ~RainbowPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "RainbowPass"; }
    bool reloadShader(const std::string& spvPath) override;

    void setHDROutput(VkImageView view, VkImage image);
    void setDepthView(VkImageView view, VkSampler sampler);

    void  setEnabled(bool v)              { m_enabled         = v; }
    bool  getEnabled() const              { return m_enabled; }
    void  setIntensity(float v)           { m_intensity       = glm::clamp(v, 0.0f, 1.0f); }
    float getIntensity() const            { return m_intensity; }
    void  setAntiSolarPos(glm::vec2 pos)  { m_antiSolarPos    = pos; }

private:
    struct RainbowParams {
        glm::vec2 antiSolarPos; //  0 (8)
        float arcRadius;        //  8 (4)
        float arcWidth;         // 12 (4)
        float intensity;        // 16 (4)
        float padding0;         // 20 (4)
        float padding1;         // 24 (4)
        float padding2;         // 28 (4)
    };
    static_assert(sizeof(RainbowParams) == 32, "RainbowParams must be 32 bytes");

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
    glm::vec2 m_antiSolarPos{0.5f, 0.7f};
    uint32_t  m_width{1280};
    uint32_t  m_height{720};
};

} // namespace ohao
