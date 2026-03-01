#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Heat Haze pass — screen-space UV distortion.
// Runs in PostProcessingPipeline after DoF and before Tonemapping.
// Reads from input HDR (sampler), writes to owned output buffer (storage image).
class HeatHazePass : public RenderPassBase {
public:
    HeatHazePass()  = default;
    ~HeatHazePass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "HeatHazePass"; }

    // Input: HDR from previous pass (sampler read)
    void setInputImage(VkImageView view, VkImage image);

    // Output view — next pass reads from here
    VkImageView getOutputView()  const { return m_output.view; }
    VkImage     getOutputImage() const { return m_output.image; }

    void  setEnabled(bool v)      { m_enabled   = v; }
    bool  getEnabled() const      { return m_enabled; }
    void  setIntensity(float v)   { m_intensity  = glm::clamp(v, 0.0f, 2.0f); }
    float getIntensity() const    { return m_intensity; }
    void  setFrequency(float v)   { m_frequency  = glm::clamp(v, 0.5f, 50.0f); }
    float getFrequency() const    { return m_frequency; }
    void  setTime(float t)        { m_time       = t; }

private:
    struct HeatHazeParams {
        float    time;       //  0 (4)
        float    intensity;  //  4 (4)
        float    frequency;  //  8 (4)
        float    speed;      // 12 (4)
        uint32_t width;      // 16 (4)
        uint32_t height;     // 20 (4)
        float    padding0;   // 24 (4)
        float    padding1;   // 28 (4)
    };
    static_assert(sizeof(HeatHazeParams) == 32, "HeatHazeParams must be 32 bytes");

    bool createDescriptors();
    bool createPipelineResources();
    void updateDescriptors();

    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};
    VkSampler             m_sampler{VK_NULL_HANDLE};

    // Owned output buffer
    RenderTarget m_output;

    // External input (not owned)
    VkImageView m_inputView{VK_NULL_HANDLE};
    VkImage     m_inputImage{VK_NULL_HANDLE};
    bool        m_descriptorDirty{false};

    bool     m_enabled{false};
    float    m_intensity{0.5f};
    float    m_frequency{8.0f};
    float    m_time{0.0f};
    uint32_t m_width{1280};
    uint32_t m_height{720};
};

} // namespace ohao
