#pragma once

#include "render_pass_base.hpp"
#include <vector>
#include <cstdint>

namespace ohao {

// Volumetric cloud compute pass (AAA quality).
// Generates a half-res RGBA16F cloud buffer via ray-marching through a flat
// cloud slab (altMin..altMax in world Y).
// RGB = in-scattered cloud radiance; A = Beer-Lambert transmittance.
// The SkyPass composites this buffer over the sky: finalSky = sky * A + RGB.
//
// Features:
// - Perlin-Worley RGBA8 3D noise (R=PW base, G/B/A=Worley octaves)
// - Weather map (512² RGBA8: R=coverage, G=cloud type, B=precipitation)
// - Cloud type gradients (stratus / cumulus / cumulonimbus)
// - Curl noise distortion for organic edges
class CloudPass : public RenderPassBase {
public:
    CloudPass() = default;
    ~CloudPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;

    // If enabled=false, output is cleared to (0,0,0,1) — sky shows through.
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;

    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "CloudPass"; }
    bool reloadShader(const std::string& spvPath) override;

    // --- Inputs (set before execute) ---
    void setDepthBuffer(VkImageView depth);
    void setSunData(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void setCameraData(const glm::mat4& invViewProj, const glm::vec3& cameraPos);
    void setPrevViewProj(const glm::mat4& pvp) { m_prevViewProj = pvp; }
    void setFrameIndex(uint32_t fi) { m_frameIndex = fi; }
    void setLightningData(float intensity, float posX, float posZ);
    void setTime(float t) { m_time = t; }

    // --- Cloud parameters ---
    void  setEnabled(bool e)          { m_enabled = e; }
    bool  getEnabled() const          { return m_enabled; }
    void  setCoverage(float v)        { m_coverage  = glm::clamp(v, 0.0f, 1.0f); }
    float getCoverage() const         { return m_coverage; }
    void  setDensity(float v)         { m_density   = glm::clamp(v, 0.0f, 5.0f); }
    float getDensity() const          { return m_density; }
    void  setAbsorption(float v)      { m_absorption = glm::clamp(v, 0.001f, 2.0f); }
    float getAbsorption() const       { return m_absorption; }
    void  setAltMin(float v)          { m_altMin = v; }
    float getAltMin() const           { return m_altMin; }
    void  setAltMax(float v)          { m_altMax = v; }
    float getAltMax() const           { return m_altMax; }
    void  setSpeed(float v)           { m_speed = v; }
    float getSpeed() const            { return m_speed; }

    // --- Output ---
    // Cloud buffer in VK_IMAGE_LAYOUT_GENERAL (SHADER_READ is valid from GENERAL).
    VkImageView getOutputView()  const { return m_cloudOutput.view; }
    VkImage     getOutputImage() const { return m_cloudOutput.image; }

    // Noise / weather map access (for cloud shadow pass to reuse)
    VkImageView getNoiseView()   const { return m_noiseView; }
    VkSampler   getNoiseSampler()const { return m_noiseSampler; }
    VkImageView getWeatherView() const { return m_weatherView; }
    VkSampler   getWeatherSampler()const{ return m_weatherSampler; }

private:
    // CPU noise generation
    static std::vector<uint8_t> generatePerlinWorleyNoise(int size);
    static std::vector<uint8_t> generateWeatherMap(int size);
    static std::vector<uint8_t> generateBlueNoise(int size);

    // GPU resource creation
    bool createNoiseTexture(const std::vector<uint8_t>& data);
    bool createWeatherTexture(const std::vector<uint8_t>& data);
    bool createBlueNoiseTexture(const std::vector<uint8_t>& data);
    bool createCloudOutput();
    bool createHistoryBuffers();
    bool createDescriptors();
    bool createPipelineResources();
    void destroyCloudOutput();
    void destroyHistoryBuffers();
    void updateDescriptors();

    // ---- Vulkan resources ----

    // Half-res RGBA16F output image (STORAGE | SAMPLED, GENERAL layout)
    RenderTarget   m_cloudOutput;
    bool           m_cloudOutputReady{false};  // false = needs UNDEFINED→GENERAL transition

    // 3D Perlin-Worley noise texture (128^3, RGBA8_UNORM, DEVICE_LOCAL)
    // R = Perlin-Worley base, G/B/A = Worley octaves (4/8/16 cells)
    VkImage        m_noiseImage{VK_NULL_HANDLE};
    VkDeviceMemory m_noiseMemory{VK_NULL_HANDLE};
    VkImageView    m_noiseView{VK_NULL_HANDLE};
    VkSampler      m_noiseSampler{VK_NULL_HANDLE};

    // Staging buffer (HOST_VISIBLE, kept until cleanup to avoid async hazard)
    VkBuffer       m_noiseStagingBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_noiseStagingMemory{VK_NULL_HANDLE};
    bool           m_noiseUploaded{false};  // false = copy needed on first execute

    // 2D Weather map (512², RGBA8_UNORM: R=coverage, G=type, B=precip, A=unused)
    VkImage        m_weatherImage{VK_NULL_HANDLE};
    VkDeviceMemory m_weatherMemory{VK_NULL_HANDLE};
    VkImageView    m_weatherView{VK_NULL_HANDLE};
    VkSampler      m_weatherSampler{VK_NULL_HANDLE};
    VkBuffer       m_weatherStagingBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_weatherStagingMemory{VK_NULL_HANDLE};
    bool           m_weatherUploaded{false};

    // Depth sampler (depth buffer is externally owned)
    VkImageView    m_depthView{VK_NULL_HANDLE};
    VkSampler      m_depthSampler{VK_NULL_HANDLE};

    // Blue noise (128² R8_UNORM, for temporal dither)
    VkImage        m_blueNoiseImage{VK_NULL_HANDLE};
    VkDeviceMemory m_blueNoiseMemory{VK_NULL_HANDLE};
    VkImageView    m_blueNoiseView{VK_NULL_HANDLE};
    VkSampler      m_blueNoiseSampler{VK_NULL_HANDLE};
    VkBuffer       m_blueNoiseStagingBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_blueNoiseStagingMemory{VK_NULL_HANDLE};
    bool           m_blueNoiseUploaded{false};

    // Ping-pong history buffers (2× half-res RGBA16F for temporal reprojection)
    RenderTarget   m_historyBuffers[2];
    bool           m_historyReady[2]{false, false};
    uint32_t       m_currentHistory{0};  // index of current WRITE target (read from 1-current)
    VkSampler      m_historySampler{VK_NULL_HANDLE};

    // Compute pipeline
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_fullWidth{1920};
    uint32_t m_fullHeight{1080};
    uint32_t m_cloudWidth{960};
    uint32_t m_cloudHeight{540};

    // State
    bool m_enabled{true};

    // Per-frame inputs
    float     m_time{0.0f};
    glm::vec3 m_sunDir{0.3f, 0.9f, 0.3f};
    glm::vec3 m_sunColor{1.0f, 0.95f, 0.85f};
    float     m_sunIntensity{1.0f};
    glm::mat4 m_invViewProj{1.0f};
    glm::mat4 m_prevViewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};
    uint32_t  m_frameIndex{0};
    float     m_lightningIntensity{0.0f};
    float     m_lightningPosX{0.0f};
    float     m_lightningPosZ{0.0f};

    // Cloud parameters
    float m_coverage{0.65f};
    float m_density{0.35f};
    float m_absorption{0.12f};
    float m_altMin{1500.0f};
    float m_altMax{8000.0f};
    float m_speed{1.0f};

    // Push constants (224 bytes — must match cloud.comp layout exactly)
    struct CloudParams {
        glm::mat4 invViewProj;       //   0 (64)
        glm::vec3 sunDirection;      //  64 (12)
        float     time;              //  76  (4) -> 80
        glm::vec3 cameraPos;         //  80 (12)
        float     cloudCoverage;     //  92  (4) -> 96
        glm::vec2 outputSize;        //  96  (8)
        float     cloudAltMin;       // 104  (4)
        float     cloudAltMax;       // 108  (4) -> 112
        float     cloudDensity;      // 112  (4)
        float     cloudAbsorption;   // 116  (4)
        float     cloudSpeed;        // 120  (4)
        uint32_t  frameIndex;        // 124  (4) -> 128
        glm::vec3 sunColor;          // 128 (12)
        float     sunIntensity;      // 140  (4) -> 144
        glm::mat4 prevViewProj;      // 144 (64) -> 208
        float     lightningIntensity;// 208  (4)
        float     lightningPosX;     // 212  (4)
        float     lightningPosZ;     // 216  (4)
        float     pad;               // 220  (4) -> 224
    };
    static_assert(sizeof(CloudParams) == 224, "CloudParams must be 224 bytes");
};

} // namespace ohao
