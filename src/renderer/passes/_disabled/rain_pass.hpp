#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Procedural rain pass.
//
// Runs as a compute shader after the sky pass and before particles.
// Reads the lighting HDR image (in GENERAL layout), additively composites
// animated rain streaks, and writes back.  No owned output — it modifies
// the external HDR framebuffer in-place.
//
// Usage:
//   rain.setHDROutput(lightingPass->getOutputView(), lightingPass->getOutputImage());
//   rain.setEnabled(true);
//   rain.setIntensity(0.8f);
//   rain.setWindX(-0.10f);
//   rain.setTime(totalTime);
//   rain.execute(cmd);
class RainPass : public RenderPassBase {
public:
    RainPass()  = default;
    ~RainPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;

    // frameIndex unused — pass is stateless between frames
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;

    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "RainPass"; }
    bool reloadShader(const std::string& spvPath) override;

    // --- External image connection ---
    // Must be called before execute() and after any resize.
    // The image must support VK_IMAGE_USAGE_STORAGE_BIT.
    void setHDROutput(VkImageView view, VkImage image);

    // --- Parameters ---
    void  setEnabled(bool v)     { m_enabled   = v; }
    bool  getEnabled() const     { return m_enabled; }

    void  setIntensity(float v)  { m_intensity  = glm::clamp(v, 0.0f, 1.0f); }
    float getIntensity() const   { return m_intensity; }

    // Horizontal wind drift (-1 = left, 0 = straight down, +1 = right).
    void  setWindX(float v)      { m_windX      = glm::clamp(v, -1.0f, 1.0f); }
    float getWindX() const       { return m_windX; }

    void  setTime(float t)       { m_time       = t; }

private:
    // Push constants — must match rain.comp layout exactly (16 bytes)
    struct RainParams {
        float time;       //  0 (4)
        float intensity;  //  4 (4)
        float windX;      //  8 (4)
        float aspect;     // 12 (4)
    };
    static_assert(sizeof(RainParams) == 16, "RainParams must be 16 bytes");

    // Helpers
    bool createDescriptors();
    bool createPipelineResources();
    void updateDescriptors();

    // ---- Vulkan resources ----
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    // ---- External HDR target (not owned) ----
    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkImage     m_hdrImage{VK_NULL_HANDLE};
    bool        m_descriptorDirty{false};

    // ---- State ----
    bool     m_enabled{false};
    float    m_intensity{1.0f};
    float    m_windX{-0.08f};
    float    m_time{0.0f};
    uint32_t m_width{1280};
    uint32_t m_height{720};
};

} // namespace ohao
