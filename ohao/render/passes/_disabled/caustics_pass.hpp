#pragma once

#include "render_pass_base.hpp"
#include <glm/glm.hpp>

namespace ohao {

// CausticsPass — pre-lighting caustic light projection (step 2.8).
//
// Runs as a compute shader after the GBuffer and terrain/foliage/decal passes
// but BEFORE SSAO/lighting.  Reads the GBuffer depth to reconstruct world
// positions of submerged geometry, then additively writes animated caustic
// light patterns (blue-cyan dancing patterns) into the GBuffer albedo image.
// The deferred lighting pass then shades the caustic-lit albedo normally,
// producing physically plausible underwater light scattering.
//
// Usage:
//   caustics.setGBufferImages(depthView, albedoImage, albedoView);
//   caustics.setCausticsTexture(view, sampler);   // optional — uses dummy if null
//   caustics.setWaterLevel(-2.0f);
//   caustics.setEnabled(true);
//   caustics.setScreenSize(width, height);
//   caustics.setInvViewProj(invViewProj);
//   caustics.setTime(totalTime);
//   caustics.execute(cmd, frameIndex);
class CausticsPass : public RenderPassBase {
public:
    CausticsPass()  = default;
    ~CausticsPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "CausticsPass"; }
    bool reloadShader(const std::string& spvPath) override;

    // --- External resource connections (not owned) ---

    // GBuffer depth (SAMPLED) and albedo (STORAGE image for R/W)
    void setGBufferImages(VkImageView depthView, VkImage albedoImage, VkImageView albedoView);
    void setDepthSampler(VkSampler sampler);

    // Caustics lookup texture (animated atlas or procedural)
    // Pass VK_NULL_HANDLE to fall back to a 1×1 white dummy.
    void setCausticsTexture(VkImageView view, VkSampler sampler);

    // --- Parameters ---
    void setEnabled(bool v)               { m_enabled = v; }
    bool getEnabled() const               { return m_enabled; }
    void setWaterLevel(float v)           { m_waterLevel = v; }
    void setTime(float t)                 { m_time = t; }
    void setCausticsIntensity(float v)    { m_causticsIntensity = glm::clamp(v, 0.0f, 2.0f); }
    float getCausticsIntensity() const    { return m_causticsIntensity; }
    void setCausticsScale(float v)        { m_causticsScale = glm::clamp(v, 0.01f, 0.5f); }
    float getCausticsScale() const        { return m_causticsScale; }
    void setInvViewProj(const glm::mat4& m) { m_invViewProj = m; }
    void setScreenSize(uint32_t w, uint32_t h) { m_width = w; m_height = h; }

private:
    // Push constants — 96 bytes, matching water_caustics.comp
    struct CausticsPC {
        glm::mat4 invViewProj;       // 64
        glm::vec2 screenSize;        //  8
        float     waterLevel;        //  4
        float     time;              //  4 — 80
        float     causticsScale;     //  4
        float     causticsIntensity; //  4
        float     pad0;              //  4
        float     pad1;              //  4 — 96
    };
    static_assert(sizeof(CausticsPC) == 96, "CausticsPC must be 96 bytes");

    bool createDescriptors();
    bool createPipeline();
    void updateDescriptors();

    // Lazily create a 1×1 R8 dummy image when no caustics texture supplied
    bool createDummy();

    // ---- Vulkan resources (owned) ----
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descSet{VK_NULL_HANDLE};

    VkSampler      m_ownDepthSampler{VK_NULL_HANDLE};   // NEAREST, for depth read
    VkSampler      m_ownLinearSampler{VK_NULL_HANDLE};  // LINEAR REPEAT, for caustics tex

    VkImage        m_dummyImage{VK_NULL_HANDLE};         // 1×1 fallback caustics tex
    VkImageView    m_dummyView{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyMem{VK_NULL_HANDLE};

    // ---- External resources (not owned) ----
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkSampler   m_depthSamplerExt{VK_NULL_HANDLE};
    VkImage     m_albedoImage{VK_NULL_HANDLE};
    VkImageView m_albedoView{VK_NULL_HANDLE};
    VkImageView m_causticsView{VK_NULL_HANDLE};
    VkSampler   m_causticsSampler{VK_NULL_HANDLE};

    // ---- State ----
    bool      m_enabled{false};
    float     m_waterLevel{0.0f};
    float     m_time{0.0f};
    float     m_causticsIntensity{0.5f};
    float     m_causticsScale{0.08f};
    bool      m_descDirty{true};
    glm::mat4 m_invViewProj{1.0f};
    uint32_t  m_width{1920};
    uint32_t  m_height{1080};
};

} // namespace ohao
