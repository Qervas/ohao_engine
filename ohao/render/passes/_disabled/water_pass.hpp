#pragma once

#include "render_pass_base.hpp"
#include "waves/i_wave_sim.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ohao {

// Gerstner / FFT wave water pass.
// Forward-rendered after DeferredLighting (and sky/weather passes) — LOAD_OP_LOAD
// on the HDR lighting output.  The pass renders a tessellated flat grid displaced
// either by four inline Gerstner waves (default) or by FFT ocean displacement
// textures (set via setWaveSim()).
//
// Dual pipeline:
//   m_pipeline    — water.vert     + water.frag  (inline Gerstner math)
//   m_pipelineFFT — water_fft.vert + water.frag  (samples bindings 9 + 10)
//
// Resource connections (call before execute):
//   setHDROutput()       — HDR render target (LOAD_OP_LOAD; owned by DeferredLightingPass)
//   setDepthBuffer()     — GBuffer depth   (sampled in fragment for foam; also depth attachment)
//   setNormalMaps()      — two scrolling detail normal maps + shared sampler
//   setIBL()             — prefiltered env cube + BRDF LUT + shared IBL sampler
//   setMatrices()        — view-proj, inverse view-proj, camera world position
//   setWaveSim()         — optional FFT sim; null = Gerstner (default)
class WaterPass : public RenderPassBase {
public:
    WaterPass()  = default;
    ~WaterPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "WaterPass"; }

    // --- External resource connections (not owned by this pass) ---

    // HDR lighting output — LOAD_OP_LOAD target.
    // Must call after DeferredLightingPass is initialized and after each resize.
    void setHDROutput(VkImageView view, VkImage image);

    // GBuffer depth — sampled for foam + used as read-only depth attachment.
    void setDepthBuffer(VkImageView view, VkSampler sampler);

    // Two scrolling detail normal maps + a shared linear-repeat sampler.
    // Pass VK_NULL_HANDLE for nm1/nm2 to disable detail normal perturbation.
    void setNormalMaps(VkImageView nm1, VkImageView nm2, VkSampler sampler);

    // IBL prefiltered env cube + BRDF split-sum LUT + shared sampler.
    // Pass VK_NULL_HANDLE to fall back to a plain sky-colored reflection.
    void setIBL(VkImageView prefiltered, VkImageView brdfLUT, VkSampler sampler);

    // HDR scene color for underwater refraction
    void setSceneColor(VkImageView view);
    // SSR output for screen-space reflections — pass VK_NULL_HANDLE to disable
    void setSSROutput(VkImageView view);
    // Sun direction (world-space, unnormalized OK) and intensity
    void setSunDirection(const glm::vec3& dir, float intensity);
    // Deep and shallow water colors (linear HDR)
    void setWaterColors(const glm::vec3& shallow, const glm::vec3& deep);

    // Foam texture (binding 7) — animated bubble/foam noise
    // Pass VK_NULL_HANDLE to fall back to a 1×1 white dummy.
    void setFoamTexture(VkImageView view, VkSampler sampler);

    // GPU ripple height map (binding 8) — from RipplePass
    // Pass VK_NULL_HANDLE to disable ripple normal perturbation.
    void setRippleMap(VkImageView view);

    // Wave simulation backend.
    // Pass nullptr (or omit) to use the default inline Gerstner mode.
    // Pass an FFTOceanSim* to switch to the FFT pipeline; bindings 9+10 are
    // updated from the sim's displacement and normal views.
    void setWaveSim(IWaveSim* sim);

    // SSS and ripple strength — packed into unused .a channels of push constants
    void setWaterSSSStrength(float v)         { m_sssStrength = glm::clamp(v, 0.0f, 1.0f); }
    float getWaterSSSStrength() const         { return m_sssStrength; }
    void setRippleNormalStrength(float v)     { m_rippleNormalStrength = glm::clamp(v, 0.0f, 1.0f); }
    float getRippleNormalStrength() const     { return m_rippleNormalStrength; }

    // Camera matrices needed for push constants.
    void setMatrices(const glm::mat4& viewProj,
                     const glm::mat4& invViewProj,
                     const glm::vec3& camPos);

    // --- Water parameters ---
    void setEnabled(bool v)        { m_enabled       = v; }
    bool getEnabled() const        { return m_enabled; }

    void setWaterLevel(float v)    { m_waterLevel    = v; }
    float getWaterLevel() const    { return m_waterLevel; }

    void setWaterSize(float v)     { m_waterSize     = (v > 0.0f ? v : 1024.0f); }
    float getWaterSize() const     { return m_waterSize; }

    void setFoamIntensity(float v) { m_foamIntensity = glm::clamp(v, 0.0f, 1.0f); }
    float getFoamIntensity() const { return m_foamIntensity; }

    void setWaveAmplitude(float v) { m_waveAmplitude = glm::clamp(v, 0.0f, 2.0f); }
    float getWaveAmplitude() const { return m_waveAmplitude; }

    void setTime(float v)          { m_time = v; }

    // Adaptive grid resolution (exponential vertex spacing for camera-centered LOD).
    // Range [32, 256]; triggers GPU buffer rebuild on the next execute().
    void setGridResolution(int n);
    int  getGridResolution() const { return m_gridN; }

private:
    // Push constants (vert + frag): 2×mat4 + 5×vec4 = 208 bytes
    // .a channels of shallowColor/deepColor repurposed for SSS and ripple strength.
    struct WaterPC {
        glm::mat4 viewProj;    // 64
        glm::mat4 invViewProj; // 64
        glm::vec4 cameraPos;   // 16  — xyz=position, w=waterLevel
        glm::vec4 waterParams; // 16  — x=size, y=time, z=waveAmp, w=foamIntensity
        glm::vec4 sunParams;   // 16  — xyz=sunDir, w=sunIntensity
        glm::vec4 shallowColor;// 16  — rgb=shallow color, a=sssStrength (NEW)
        glm::vec4 deepColor;   // 16  — rgb=deep color,    a=rippleNormalStrength (NEW)
    };
    static_assert(sizeof(WaterPC) == 208, "WaterPC push constant must be exactly 208 bytes");

    // --- Private helpers ---
    bool createRenderPass();
    bool createFramebuffer();
    bool createDescriptors();
    bool createPipeline();   // creates both m_pipeline and m_pipelineFFT
    bool createGridMesh();
    void destroyFramebuffer();
    void destroyGridMesh();
    void updateDescriptors();

    // Own samplers created during initialize() for depth (NEAREST) and
    // a fallback linear sampler when the caller supplies VK_NULL_HANDLE normals/IBL.
    bool createSamplers();

    // --- Vulkan owned resources ---

    // Render pass (color=LOAD, depth=LOAD/DONT_CARE read-only)
    VkRenderPass  m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Graphics pipelines (dual — same layout, different vertex shaders)
    VkPipeline       m_pipeline{VK_NULL_HANDLE};     // Gerstner: water.vert + water.frag
    VkPipeline       m_pipelineFFT{VK_NULL_HANDLE};  // FFT: water_fft.vert + water.frag
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors (11 bindings — shared by both pipelines)
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    // Own samplers
    VkSampler m_depthSampler{VK_NULL_HANDLE};    // NEAREST, CLAMP — for scene depth
    VkSampler m_linearSampler{VK_NULL_HANDLE};   // LINEAR,  REPEAT — fallback for normals

    // Dummy 1×1 white texture used when caller hasn't supplied normal maps or IBL
    VkImage       m_dummyImage{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyMemory{VK_NULL_HANDLE};
    VkImageView    m_dummyView{VK_NULL_HANDLE};
    VkImage        m_dummyCubeImage{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyCubeMemory{VK_NULL_HANDLE};
    VkImageView    m_dummyCubeView{VK_NULL_HANDLE};
    // Grid mesh (N×N quads of vec2 XZ vertices)
    // m_gridN replaces static constexpr GRID_N; m_meshDirty triggers rebuild
    int            m_gridN{64};
    bool           m_meshDirty{false};
    VkBuffer       m_vertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_vertexMemory{VK_NULL_HANDLE};
    VkBuffer       m_indexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_indexMemory{VK_NULL_HANDLE};
    uint32_t       m_indexCount{0};

    // Staging buffer for grid mesh upload (freed after initial upload)
    VkBuffer       m_stagingVtx{VK_NULL_HANDLE};
    VkDeviceMemory m_stagingVtxMem{VK_NULL_HANDLE};
    VkBuffer       m_stagingIdx{VK_NULL_HANDLE};
    VkDeviceMemory m_stagingIdxMem{VK_NULL_HANDLE};

    // --- External views / images (NOT owned) ---
    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkImage     m_hdrImage{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkSampler   m_depthSamplerExt{VK_NULL_HANDLE};  // caller-supplied; may be null
    VkImageView m_normalMap1{VK_NULL_HANDLE};
    VkImageView m_normalMap2{VK_NULL_HANDLE};
    VkSampler   m_normalSampler{VK_NULL_HANDLE};
    VkImageView m_iblPrefiltered{VK_NULL_HANDLE};
    VkImageView m_iblBrdfLUT{VK_NULL_HANDLE};
    VkSampler   m_iblSampler{VK_NULL_HANDLE};

    VkImageView m_sceneColorView{VK_NULL_HANDLE};   // HDR scene color for refraction
    VkImageView m_ssrView{VK_NULL_HANDLE};           // SSR output (nullable)
    VkImageView m_foamTexView{VK_NULL_HANDLE};       // Foam noise texture (binding 7)
    VkSampler   m_foamSampler{VK_NULL_HANDLE};       // Foam sampler
    VkImageView m_rippleMapView{VK_NULL_HANDLE};     // GPU ripple height map (binding 8)

    // FFT wave sim views (bindings 9 + 10) — non-owning, updated by setWaveSim()
    IWaveSim*   m_waveSim{nullptr};
    VkImageView m_fftDisplacementView{VK_NULL_HANDLE};
    VkImageView m_fftNormalView{VK_NULL_HANDLE};

    // --- State ---
    bool      m_descriptorDirty{true};
    bool      m_enabled{false};
    float     m_waterLevel{0.0f};
    float     m_waterSize{1024.0f};
    float     m_foamIntensity{0.6f};
    float     m_waveAmplitude{0.3f};
    float     m_time{0.0f};
    float     m_sssStrength{0.35f};
    float     m_rippleNormalStrength{0.15f};

    glm::vec3 m_sunDir{-0.3f, 0.8f, -0.5f};
    float     m_sunIntensity{8.0f};
    glm::vec3 m_shallowColor{0.10f, 0.40f, 0.50f};
    glm::vec3 m_deepColor{0.02f, 0.12f, 0.22f};

    glm::mat4 m_viewProj{1.0f};
    glm::mat4 m_invViewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};

    uint32_t  m_width{1920};
    uint32_t  m_height{1080};

    bool      m_resourcesUploaded{false};
};

} // namespace ohao
