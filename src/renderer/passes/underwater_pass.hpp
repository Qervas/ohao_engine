#pragma once

#include "render_pass_base.hpp"
#include <glm/glm.hpp>

namespace ohao {

// UnderwaterPass — post-process effect for when the camera is submerged (step 4.65).
//
// Only dispatched when camera.y < waterLevel.  Applies three layered effects:
//   1. Sinusoidal lens warp  — simulates caustic light rippling through water
//   2. Chromatic aberration  — RGB channel separation at screen edges
//   3. Exponential depth fog — configurable blue-tinted water absorption
//   4. Edge vignette         — darkens borders proportional to depth
//
// Reads the HDR lighting image via a COMBINED_IMAGE_SAMPLER (sampled) and
// writes back via a STORAGE_IMAGE (same backing memory, different view).
// This mirrors the pattern used by HeatHazePass.
//
// Usage:
//   underwater.setHDRTarget(readView, image, writeView);
//   underwater.setEnabled(cameraPos.y < waterLevel);
//   underwater.setWaterDepth(waterLevel - cameraPos.y);
//   underwater.setTime(totalTime);
//   underwater.execute(cmd, frameIndex);
class UnderwaterPass : public RenderPassBase {
public:
    UnderwaterPass()  = default;
    ~UnderwaterPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "UnderwaterPass"; }

    // --- External resource connections (not owned) ---

    // HDR lighting image.  readView must be COMBINED_IMAGE_SAMPLER-capable;
    // writeView must be STORAGE_IMAGE-capable (same image, different view usage).
    void setHDRTarget(VkImageView readView, VkImage image, VkImageView writeView);

    // --- Parameters ---
    void setEnabled(bool v)            { m_enabled        = v; }
    bool getEnabled() const            { return m_enabled; }
    void setWaterDepth(float v)        { m_waterDepth     = glm::max(v, 0.0f); }
    float getWaterDepth() const        { return m_waterDepth; }
    void setTime(float t)              { m_time           = t; }
    void setFogColor(const glm::vec3& c)  { m_fogColor    = c; }
    void setFogDensity(float v)        { m_fogDensity     = glm::clamp(v, 0.0f, 1.0f); }
    float getFogDensity() const        { return m_fogDensity; }
    void setChromStrength(float v)     { m_chromStrength  = glm::clamp(v, 0.0f, 0.05f); }
    float getChromStrength() const     { return m_chromStrength; }
    void setDistortStrength(float v)   { m_distortStrength = glm::clamp(v, 0.0f, 0.02f); }
    float getDistortStrength() const   { return m_distortStrength; }
    void setDistortFrequency(float v)  { m_distortFreq  = glm::clamp(v, 1.0f, 40.0f); }
    float getDistortFrequency() const  { return m_distortFreq; }
    void setDistortSpeed(float v)      { m_distortSpeed = glm::clamp(v, 0.1f, 10.0f); }
    float getDistortSpeed() const      { return m_distortSpeed; }

private:
    // Push constants — 48 bytes, matching water_underwater.comp
    struct UnderwaterPC {
        glm::vec2 screenSize;      //  8
        float     time;            //  4
        float     waterDepth;      //  4 — 16
        glm::vec3 fogColor;        // 12
        float     fogDensity;      //  4 — 32
        float     chromStrength;   //  4
        float     distortFreq;     //  4
        float     distortSpeed;    //  4
        float     distortStrength; //  4 — 48
    };
    static_assert(sizeof(UnderwaterPC) == 48, "UnderwaterPC must be 48 bytes");

    bool createDescriptors();
    bool createPipeline();
    void updateDescriptors();

    // ---- Vulkan resources (owned) ----
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descSet{VK_NULL_HANDLE};

    VkSampler m_linearSampler{VK_NULL_HANDLE};  // LINEAR CLAMP for HDR read

    // ---- External resources (not owned) ----
    VkImageView m_hdrReadView{VK_NULL_HANDLE};
    VkImage     m_hdrImage{VK_NULL_HANDLE};
    VkImageView m_hdrWriteView{VK_NULL_HANDLE};

    bool m_descDirty{true};

    // ---- State ----
    bool      m_enabled{false};
    float     m_waterDepth{0.0f};
    float     m_time{0.0f};
    glm::vec3 m_fogColor{0.04f, 0.14f, 0.28f};
    float     m_fogDensity{0.12f};
    float     m_chromStrength{0.006f};
    float     m_distortStrength{0.004f};
    float     m_distortFreq{12.0f};
    float     m_distortSpeed{1.2f};

    uint32_t m_width{1920};
    uint32_t m_height{1080};
};

} // namespace ohao
