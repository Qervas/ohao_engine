#pragma once

#include "i_wave_sim.hpp"
#include "../render_pass_base.hpp"
#include <glm/glm.hpp>

namespace ohao {

// Tessendorf FFT ocean simulation.
//
// Each call to simulate() dispatches 4 compute passes:
//   1. spectrum_init  — Phillips h0(k) (once per wind change)
//   2. spectrum_hkt   — per-frame h(k,t) animation
//   3. fft_butterfly  — 1-D in-place IFFT (horizontal then vertical)
//   4. fft_normal     — normal map + foam from displacement gradient
//
// Produces two SHADER_READ_ONLY textures that WaterPass samples in
// water_fft.vert via bindings 9 and 10.
class FFTOceanSim final : public IWaveSim, public RenderPassBase {
public:
    FFTOceanSim()  = default;
    ~FFTOceanSim() override { cleanup(); }

    // ── IWaveSim / RenderPassBase interface ─────────────────────────────────
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void simulate(VkCommandBuffer cmd, float time, float dt) override;
    bool providesTextures() const override { return true; }
    VkImageView getDisplacementView() const override { return m_hktView; }
    VkImageView getNormalView()       const override { return m_normalView; }

    // Required by RenderPassBase (not used — FFTOceanSim is compute-only)
    void execute(VkCommandBuffer /*cmd*/, uint32_t /*frameIndex*/) override {}
    const char* getName() const override { return "FFTOceanSim"; }

    // ── Wind / ocean parameters ──────────────────────────────────────────────
    // Changing wind speed or direction marks the Phillips spectrum dirty,
    // causing a one-time re-dispatch of spectrum_init on the next simulate().
    void setWindSpeed(float s)    { m_windSpeed = glm::max(s, 0.1f); m_spectrumDirty = true; }
    void setWindDirection(float x, float z);

    void setPatchSize(float s)    { m_patchSize    = glm::max(s, 10.0f); }
    void setAmplitude(float a)    { m_amplitude    = glm::max(a, 0.0001f); m_spectrumDirty = true; }
    void setChoppiness(float c)   { m_choppiness   = glm::clamp(c, 0.0f, 4.0f); }
    void setNormalStrength(float v){ m_normalStrength = glm::max(v, 0.1f); }

    float getWindSpeed()     const { return m_windSpeed; }
    glm::vec2 getWindDir()   const { return glm::vec2(m_windDirX, m_windDirZ); }
    float getPatchSize()     const { return m_patchSize; }
    float getAmplitude()     const { return m_amplitude; }
    float getChoppiness()    const { return m_choppiness; }

private:
    static constexpr uint32_t N = 256;  // FFT resolution (must be power-of-2, ≤ local_size_x)

    // ── Owned textures ───────────────────────────────────────────────────────
    // h0: Phillips spectrum (static between wind changes)
    VkImage        m_h0Image{VK_NULL_HANDLE};
    VkDeviceMemory m_h0Memory{VK_NULL_HANDLE};
    VkImageView    m_h0View{VK_NULL_HANDLE};
    VkSampler      m_h0Sampler{VK_NULL_HANDLE};
    VkImageLayout  m_h0Layout{VK_IMAGE_LAYOUT_UNDEFINED};

    // hkt: per-frame frequency-domain data; also holds IFFT result in-place
    VkImage        m_hktImage{VK_NULL_HANDLE};
    VkDeviceMemory m_hktMemory{VK_NULL_HANDLE};
    VkImageView    m_hktView{VK_NULL_HANDLE};
    VkSampler      m_hktSampler{VK_NULL_HANDLE};
    VkImageLayout  m_hktLayout{VK_IMAGE_LAYOUT_UNDEFINED};

    // normalMap: normal + foam written by fft_normal.comp
    VkImage        m_normalImage{VK_NULL_HANDLE};
    VkDeviceMemory m_normalMemory{VK_NULL_HANDLE};
    VkImageView    m_normalView{VK_NULL_HANDLE};
    VkSampler      m_normalSampler{VK_NULL_HANDLE};
    VkImageLayout  m_normalLayout{VK_IMAGE_LAYOUT_UNDEFINED};

    // ── Compute pipeline 0: spectrum_init ────────────────────────────────────
    VkDescriptorSetLayout m_initDSL{VK_NULL_HANDLE};
    VkDescriptorPool      m_initPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_initDS{VK_NULL_HANDLE};
    VkPipelineLayout      m_initPL{VK_NULL_HANDLE};
    VkPipeline            m_initPipeline{VK_NULL_HANDLE};

    // ── Compute pipeline 1: spectrum_hkt ─────────────────────────────────────
    VkDescriptorSetLayout m_hktDSL{VK_NULL_HANDLE};
    VkDescriptorPool      m_hktPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_hktDS{VK_NULL_HANDLE};
    VkPipelineLayout      m_hktPL{VK_NULL_HANDLE};
    VkPipeline            m_hktPipeline{VK_NULL_HANDLE};

    // ── Compute pipeline 2: fft_butterfly ────────────────────────────────────
    VkDescriptorSetLayout m_bflyDSL{VK_NULL_HANDLE};
    VkDescriptorPool      m_bflyPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_bflyDS{VK_NULL_HANDLE};
    VkPipelineLayout      m_bflyPL{VK_NULL_HANDLE};
    VkPipeline            m_bflyPipeline{VK_NULL_HANDLE};

    // ── Compute pipeline 3: fft_normal ───────────────────────────────────────
    VkDescriptorSetLayout m_normDSL{VK_NULL_HANDLE};
    VkDescriptorPool      m_normPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_normDS{VK_NULL_HANDLE};
    VkPipelineLayout      m_normPL{VK_NULL_HANDLE};
    VkPipeline            m_normPipeline{VK_NULL_HANDLE};

    // ── State ────────────────────────────────────────────────────────────────
    bool  m_spectrumDirty{true};  // re-run spectrum_init on next simulate()
    float m_windSpeed{8.0f};
    float m_windDirX{1.0f};
    float m_windDirZ{0.0f};
    float m_patchSize{500.0f};    // world metres per tile
    float m_amplitude{0.0006f};   // Phillips A constant
    float m_choppiness{1.4f};     // horizontal displacement scale
    float m_normalStrength{1.0f};
    float m_foamThreshold{0.12f};

    // ── Private helpers ──────────────────────────────────────────────────────
    bool createTextures();
    bool createSpectrumInitPipeline();
    bool createSpectrumHktPipeline();
    bool createButterflyPipeline();
    bool createNormalPipeline();

    void transitionTo(VkCommandBuffer cmd, VkImage image, VkImageLayout& current,
                      VkImageLayout newLayout);
    void computeBarrier(VkCommandBuffer cmd);
};

} // namespace ohao
