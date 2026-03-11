#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Procedural snow pass.
//
// Runs as a compute shader after the rain pass and before particles.
// Reads the lighting HDR image (in GENERAL layout), additively composites
// animated snowflakes + blizzard streaks, and writes back.
//
// Usage:
//   snow.setHDROutput(lightingPass->getOutputView(), lightingPass->getOutputImage());
//   snow.setEnabled(true);
//   snow.setIntensity(0.6f);
//   snow.setWindX(-0.12f);
//   snow.setTime(totalTime);
//   snow.execute(cmd);
class SnowPass : public RenderPassBase {
public:
    SnowPass()  = default;
    ~SnowPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SnowPass"; }
    bool reloadShader(const std::string& spvPath) override;

    // --- External image connection ---
    void setHDROutput(VkImageView view, VkImage image);

    // --- Parameters ---
    void  setEnabled(bool v)    { m_enabled   = v; }
    bool  getEnabled() const    { return m_enabled; }
    void  setIntensity(float v) { m_intensity = glm::clamp(v, 0.0f, 1.0f); }
    float getIntensity() const  { return m_intensity; }
    void  setWindX(float v)     { m_windX     = glm::clamp(v, -1.0f, 1.0f); }
    float getWindX() const      { return m_windX; }
    void  setTime(float t)      { m_time      = t; }

private:
    struct SnowParams {
        float time;       //  0 (4)
        float intensity;  //  4 (4)
        float windX;      //  8 (4)
        float aspect;     // 12 (4)
    };
    static_assert(sizeof(SnowParams) == 16, "SnowParams must be 16 bytes");

    bool createDescriptors();
    bool createPipelineResources();
    void updateDescriptors();

    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkImage     m_hdrImage{VK_NULL_HANDLE};
    bool        m_descriptorDirty{false};

    bool     m_enabled{false};
    float    m_intensity{1.0f};
    float    m_windX{-0.08f};
    float    m_time{0.0f};
    uint32_t m_width{1280};
    uint32_t m_height{720};
};

} // namespace ohao
