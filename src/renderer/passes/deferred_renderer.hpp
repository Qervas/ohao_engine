#pragma once

#include "gbuffer_pass.hpp"
#include "deferred_lighting_pass.hpp"
#include "csm_pass.hpp"
#include "post_processing_pipeline.hpp"
#include "gizmo_pass.hpp"
#include "sky_pass.hpp"
#include "cloud_pass.hpp"
#include "cloud/cloud_shadow_pass.hpp"
#include "rain_pass.hpp"
#include "snow_pass.hpp"
#include "sand_pass.hpp"
#include "god_rays_pass.hpp"
#include "aurora_pass.hpp"
#include "rainbow_pass.hpp"
#include "terrain_pass.hpp"
#include "water_pass.hpp"
#include "waves/fft_ocean_sim.hpp"
#include "caustics_pass.hpp"
#include "ripple_pass.hpp"
#include "underwater_pass.hpp"
#include "decal_pass.hpp"
#include "foliage_pass.hpp"
#include "renderer/particles/particle_system.hpp"
#include "renderer/graph/render_graph.hpp"
#include "utils/common_types.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>
#include <queue>
#include <string>

namespace ohao {

class Scene;

// Full deferred rendering pipeline that orchestrates all passes
// This is the main entry point for AAA-quality rendering
class DeferredRenderer {
public:
    DeferredRenderer() = default;
    ~DeferredRenderer();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();

    // Main render function - call this once per frame
    void render(VkCommandBuffer cmd, uint32_t frameIndex);

    // Resize handling
    void onResize(uint32_t width, uint32_t height);

    // Scene configuration
    void setScene(Scene* scene);

    // Geometry buffers (from OffscreenRenderer)
    void setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer,
                            const std::unordered_map<uint64_t, MeshBufferInfo>* bufferMap);

    // Camera configuration
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       const glm::vec3& position, float nearPlane, float farPlane);

    // Light configuration
    void setDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void setLightBuffer(VkBuffer lightBuffer, uint32_t lightCount);
    glm::vec3 getLightDirection() const { return m_lightDirection; }
    float getNightFactor() const { return m_nightFactor; }
    glm::vec3 getMoonDirection() const { return m_moonDirection; }

    // IBL configuration (optional)
    void setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                        VkImageView brdfLUT, VkSampler iblSampler);

    // Texture manager (forwarded to GBufferPass)
    void setTextureManager(BindlessTextureManager* texManager);

    // Post-processing configuration
    PostProcessingPipeline* getPostProcessing() { return m_postProcessing.get(); }

    // Wireframe mode
    void setWireframeEnabled(bool enabled);
    bool getWireframeEnabled() const { return m_wireframeEnabled; }

    // Gizmo controls
    void setGizmoEnabled(bool enabled);
    bool getGizmoEnabled() const { return m_gizmoEnabled; }
    void setGizmoMode(GizmoMode mode);
    void setGizmoTransform(const glm::mat4& model);
    void setGizmoHighlightedAxis(GizmoAxis axis);

    // Sky configuration
    void setSkyEnabled(bool enabled);
    bool getSkyEnabled() const { return m_skyEnabled; }
    void setSunDirection(const glm::vec3& dir);
    void setSkyTurbidity(float t);
    void setSkyIntensity(float i);
    void setSkyGroundColor(const glm::vec3& c);

    // Cloud configuration
    void setCloudEnabled(bool e);
    bool getCloudEnabled() const { return m_cloudEnabled; }
    void setCloudCoverage(float v);
    float getCloudCoverage() const { return m_cloudCoverage; }
    void setCloudDensity(float v);
    float getCloudDensity() const { return m_cloudDensity; }
    void setCloudAltMin(float v);
    float getCloudAltMin() const { return m_cloudAltMin; }
    void setCloudAltMax(float v);
    float getCloudAltMax() const { return m_cloudAltMax; }
    void setCloudSpeed(float v);
    float getCloudSpeed() const { return m_cloudSpeed; }

    // Rain configuration
    void  setRainEnabled(bool e);
    bool  getRainEnabled() const  { return m_rainEnabled; }
    void  setRainIntensity(float v);
    float getRainIntensity() const { return m_rainIntensity; }
    void  setRainWindX(float v);
    float getRainWindX() const    { return m_rainWindX; }

    // Snow configuration
    void  setSnowEnabled(bool e)      { m_snowEnabled   = e; }
    bool  getSnowEnabled() const      { return m_snowEnabled; }
    void  setSnowIntensity(float v)   { m_snowIntensity = glm::clamp(v, 0.0f, 1.0f); }
    float getSnowIntensity() const    { return m_snowIntensity; }
    void  setSnowWindX(float v)       { m_snowWindX     = glm::clamp(v, -1.0f, 1.0f); }
    float getSnowWindX() const        { return m_snowWindX; }
    float getSnowAccumulation() const { return m_snowAccumulation; }
    void  setSnowAccumRate(float r)   { m_snowAccumRate = glm::clamp(r, 0.0f, 10.0f); }
    void  setSnowMeltRate(float r)    { m_snowMeltRate  = glm::clamp(r, 0.0f, 10.0f); }

    // Sand / Sandstorm configuration
    void  setSandEnabled(bool e)      { m_sandEnabled   = e; }
    bool  getSandEnabled() const      { return m_sandEnabled; }
    void  setSandIntensity(float v)   { m_sandIntensity = glm::clamp(v, 0.0f, 1.0f); }
    float getSandIntensity() const    { return m_sandIntensity; }
    void  setSandWindX(float v)       { m_sandWindX     = glm::clamp(v, -1.0f, 1.0f); }
    float getSandWindX() const        { return m_sandWindX; }

    // God Rays configuration
    void  setGodRaysEnabled(bool e)       { m_godRaysEnabled   = e; }
    bool  getGodRaysEnabled() const       { return m_godRaysEnabled; }
    void  setGodRaysIntensity(float v)    { m_godRaysIntensity = glm::clamp(v, 0.0f, 2.0f); }
    float getGodRaysIntensity() const     { return m_godRaysIntensity; }

    // Aurora configuration
    void  setAuroraEnabled(bool e)    { m_auroraEnabled   = e; }
    bool  getAuroraEnabled() const    { return m_auroraEnabled; }
    void  setAuroraIntensity(float v) { m_auroraIntensity = glm::clamp(v, 0.0f, 1.0f); }
    float getAuroraIntensity() const  { return m_auroraIntensity; }
    void  setAuroraHue(float v)       { m_auroraHue       = glm::clamp(v, 0.0f, 1.0f); }

    // Rainbow configuration
    void  setRainbowEnabled(bool e)   { m_rainbowEnabled  = e; }
    bool  getRainbowEnabled() const   { return m_rainbowEnabled; }

    // ── Terrain — multi-tile streaming ───────────────────────────────────────
    // A TerrainTile describes one tile in a tiled terrain grid.
    // physicsHandle stores the Jolt body handle for that tile's heightfield (uint32_t).
    struct TerrainTile {
        float    offsetX{0.0f};
        float    offsetZ{0.0f};
        bool     active{false};
        uint32_t physicsHandle{UINT32_MAX};  // BodyHandle = uint32_t (physics_backend.hpp)
    };
    static constexpr int MAX_TERRAIN_TILES = 9;  // 3×3 grid

    // Add a terrain tile at world offset (x, z). Returns tile index or -1 if full.
    int  addTerrainTile(float offsetX, float offsetZ);
    void clearTerrainTiles();
    void setTerrainTileCullRadius(float r) { m_terrainTileCullRadius = r; }

    // ── Terrain ──────────────────────────────────────────────────────────────
    void  setTerrainEnabled(bool v)       { m_terrainEnabled = v; if (m_terrainPass) m_terrainPass->setEnabled(v); }
    bool  getTerrainEnabled() const       { return m_terrainEnabled; }
    void  setTerrainHeightScale(float v)  { m_terrainHeightScale = v; }
    float getTerrainHeightScale() const   { return m_terrainHeightScale; }
    void  setTerrainSize(float v)         { m_terrainSize = v; }
    float getTerrainSize() const          { return m_terrainSize; }
    // Load heightmap / splatmap / layer textures from path (uses BindlessTextureManager)
    void  setTerrainHeightmapPath(const std::string& path);
    void  setTerrainSplatMapPath(const std::string& path);
    void  setTerrainLayerAlbedo(uint32_t layer, const std::string& path);
    void  setTerrainLayerNormal(uint32_t layer, const std::string& path);
    // Procedural generation API
    void  setTerrainType(int type);           // 0=external, 1-6=procedural types
    void  setTerrainGenFrequency(float f);
    void  setTerrainGenOctaves(int n);
    void  setTerrainGenOffset(glm::vec2 off);
    void  setTerrainGenResolution(uint32_t r);
    void  setTerrainMacroVariationPath(const std::string& path);
    void  generateTerrain();                  // triggers GPU gen on next frame
    TerrainPass* getTerrainPass() { return m_terrainPass.get(); }

    // ── Water ────────────────────────────────────────────────────────────────
    void  setWaterEnabled(bool v)         { m_waterEnabled = v; if (m_waterPass) m_waterPass->setEnabled(v); }
    bool  getWaterEnabled() const         { return m_waterEnabled; }
    void  setWaterLevel(float v)          { m_waterLevel = v; }
    float getWaterLevel() const           { return m_waterLevel; }
    void  setWaterSize(float v)           { m_waterSize = v; }
    float getWaterSize() const            { return m_waterSize; }
    void  setWaterFoamIntensity(float v)  { m_waterFoamIntensity = glm::clamp(v, 0.0f, 1.0f); }
    float getWaterFoamIntensity() const   { return m_waterFoamIntensity; }
    void  setWaterWaveAmplitude(float v)  { m_waterWaveAmplitude = glm::clamp(v, 0.0f, 2.0f); }
    float getWaterWaveAmplitude() const   { return m_waterWaveAmplitude; }
    void  setWaterNormalMap1(const std::string& path);
    void  setWaterNormalMap2(const std::string& path);
    void  setWaterSceneColor(VkImageView view);    // HDR scene color for refraction
    void  setWaterSSROutput(VkImageView view);      // SSR output for reflections
    void  setWaterSunDirection(const glm::vec3& dir, float intensity);
    void  setWaterColors(const glm::vec3& shallow, const glm::vec3& deep);

    // ── Caustics ─────────────────────────────────────────────────────────────
    void  setCausticsEnabled(bool v)         { m_causticsEnabled = v; }
    bool  getCausticsEnabled() const         { return m_causticsEnabled; }
    void  setCausticsIntensity(float v);
    float getCausticsIntensity() const       { return m_causticsIntensity; }
    void  setCausticsTexturePath(const std::string& path);

    // ── Water ripples ─────────────────────────────────────────────────────────
    void  setWaterRipplesEnabled(bool v)     { m_waterRipplesEnabled = v; }
    bool  getWaterRipplesEnabled() const     { return m_waterRipplesEnabled; }
    void  addWaterRipple(float worldX, float worldZ, float strength);
    void  clearWaterRipples();

    // ── Enhanced water ────────────────────────────────────────────────────────
    void  setWaterSSSStrength(float v);
    float getWaterSSSStrength() const        { return m_waterSSSStrength; }
    void  setWaterFoamTexturePath(const std::string& path);

    // ── Wave simulation mode ──────────────────────────────────────────────────
    // 0 = Gerstner (default — 4 inline waves in water.vert, zero GPU overhead)
    // 1 = FFT Tessendorf (256×256 compute simulation, organic non-repeating surface)
    void  setWaveMode(int mode);
    int   getWaveMode() const               { return m_waveMode; }
    void  setFFTWindSpeed(float s);
    float getFFTWindSpeed() const           { return m_fftWindSpeed; }
    void  setFFTWindDirection(float x, float z);
    void  setFFTPatchSize(float s);
    float getFFTPatchSize() const           { return m_fftPatchSize; }
    void  setFFTChoppiness(float c);
    float getFFTChoppiness() const          { return m_fftChoppiness; }
    void  setFFTNormalStrength(float v);
    float getFFTNormalStrength() const      { return m_fftNormalStrength; }

    // ── Underwater ────────────────────────────────────────────────────────────
    void  setUnderwaterEnabled(bool v)       { m_underwaterEnabled = v; }
    bool  getUnderwaterEnabled() const       { return m_underwaterEnabled; }
    void  setUnderwaterFogColor(const glm::vec3& c);
    void  setUnderwaterFogDensity(float v);
    float getUnderwaterFogDensity() const    { return m_underwaterFogDensity; }
    void  setUnderwaterChromStrength(float v);
    float getUnderwaterChromStrength() const { return m_underwaterChromStrength; }

    // ── Water ripple fine-tuning ───────────────────────────────────────────────
    void  setWaterRippleDamping(float v);
    float getWaterRippleDamping() const      { return m_waterRippleDamping; }
    void  setWaterRippleSpeed(float v);
    float getWaterRippleSpeed() const        { return m_waterRippleSpeed; }

    // ── Caustics scale ────────────────────────────────────────────────────────
    void  setCausticsScale(float v);
    float getCausticsScale() const           { return m_causticsScale; }

    // ── Underwater distort params ─────────────────────────────────────────────
    void  setUnderwaterDistortFrequency(float v);
    float getUnderwaterDistortFrequency() const { return m_underwaterDistortFreq; }
    void  setUnderwaterDistortSpeed(float v);
    float getUnderwaterDistortSpeed() const  { return m_underwaterDistortSpeed; }

    // ── Water grid resolution (adaptive LOD) ─────────────────────────────────
    void  setWaterGridResolution(int n);
    int   getWaterGridResolution() const     { return m_waterGridN; }

    // ── Decals ───────────────────────────────────────────────────────────────
    void     setDecalsEnabled(bool v)     { m_decalsEnabled = v; }
    bool     getDecalsEnabled() const     { return m_decalsEnabled; }
    uint32_t addDecal(const glm::vec3& pos, const glm::vec3& normal,
                      const glm::vec3& size, const std::string& albedoPath,
                      float opacity = 0.9f, const glm::vec4& tint = glm::vec4(1.0f));
    void     removeDecal(uint32_t handle);
    void     clearDecals();

    // ── Foliage ──────────────────────────────────────────────────────────────
    void  setFoliageEnabled(bool v)       { m_foliageEnabled = v; }
    bool  getFoliageEnabled() const       { return m_foliageEnabled; }
    void  setFoliageCullDistance(float v) { m_foliageCullDistance = v; }
    float getFoliageCullDistance() const  { return m_foliageCullDistance; }
    void  setGrassTexturePath(const std::string& path);
    void  addFoliageCluster(const glm::vec3& center, float radius, float density);
    void  clearFoliage();

    // Ground wetness — temporal integration driven by rain state.
    // Surfaces accumulate wetness at wetRate/s and dry at dryRate/s.
    // Surface wetness is readable so scripts can react (footstep sounds, etc.)
    float getSurfaceWetness() const { return m_wetness; }
    void  setWetnessRate(float r)   { m_wetRate  = glm::clamp(r, 0.0f, 10.0f); }
    void  setDryingRate(float r)    { m_dryRate  = glm::clamp(r, 0.0f, 10.0f); }

    // Frost — accumulates when snowAccumulation > 0.6, melts when snow clears.
    float getFrostCover() const     { return m_frostCover; }
    void  setFrostAccumRate(float r){ m_frostAccumRate = glm::clamp(r, 0.0f, 10.0f); }
    void  setFrostMeltRate(float r) { m_frostMeltRate  = glm::clamp(r, 0.0f, 10.0f); }

    // Lightning — auto-triggered when rain intensity >= autoThreshold.
    // Also exposed for scripted control (cutscenes, horror events, etc.)
    void  setLightningEnabled(bool v)       { m_lightningEnabled = v; }
    bool  getLightningEnabled() const       { return m_lightningEnabled; }
    void  setLightningInterval(float s)     { m_lightningInterval = glm::max(s, 0.5f); }
    float getLightningInterval() const      { return m_lightningInterval; }
    void  setLightningBrightness(float v)   { m_lightningBrightness = glm::clamp(v, 0.1f, 10.0f); }
    void  triggerLightning()                { m_lightningTimerForce = true; }
    float getFlashIntensity() const         { return m_flashIntensity; }
    // Drain pending signal (checked by OhaoViewport::_process to emit lightning_struck)
    bool  consumeLightningPending()         { bool p = m_lightningPending; m_lightningPending = false; return p; }

    // Particle system
    void spawnParticles(const glm::vec3& position, ParticleType type,
                        const glm::vec3& direction = glm::vec3(0.0f, 1.0f, 0.0f));
    void setDeltaTime(float dt) { m_deltaTime = dt; }

    // Get final output for display/readback
    VkImageView getFinalOutput() const;
    VkImage getFinalOutputImage() const;

    // Get SSAO output for deferred lighting
    VkImageView getSSAOOutput() const;

    // Get jitter offset for TAA
    glm::vec2 getJitterOffset(uint32_t frameIndex) const;

    // === Introspection (MCP AI agent support) ===
    nlohmann::json getPipelineInfo() const;
    nlohmann::json getPerfStats() const;

    // === Hot-reload (runtime shader swap) ===
    bool reloadShaderForPass(const std::string& passName, const std::string& spvPath);

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Render passes
    std::unique_ptr<GBufferPass> m_gbufferPass;
    std::unique_ptr<CSMPass> m_csmPass;
    std::unique_ptr<DeferredLightingPass> m_lightingPass;
    std::unique_ptr<PostProcessingPipeline> m_postProcessing;
    std::unique_ptr<GizmoPass> m_gizmoPass;
    std::unique_ptr<SkyPass>   m_skyPass;
    std::unique_ptr<CloudPass> m_cloudPass;
    std::unique_ptr<CloudShadowPass> m_cloudShadowPass;
    std::unique_ptr<RainPass>    m_rainPass;
    std::unique_ptr<SnowPass>   m_snowPass;
    std::unique_ptr<SandPass>   m_sandPass;
    std::unique_ptr<GodRaysPass> m_godRaysPass;
    std::unique_ptr<AuroraPass>  m_auroraPass;
    std::unique_ptr<RainbowPass> m_rainbowPass;
    std::unique_ptr<TerrainPass>    m_terrainPass;
    std::unique_ptr<WaterPass>      m_waterPass;
    std::unique_ptr<FFTOceanSim>    m_fftOceanSim;
    std::unique_ptr<CausticsPass>   m_causticsPass;
    std::unique_ptr<RipplePass>     m_ripplePass;
    std::unique_ptr<UnderwaterPass> m_underwaterPass;
    std::unique_ptr<DecalPass>      m_decalPass;
    std::unique_ptr<FoliagePass>    m_foliagePass;

    // Scene reference
    Scene* m_scene{nullptr};

    // Texture manager (set before initialize)
    BindlessTextureManager* m_textureManager{nullptr};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Camera data
    glm::mat4 m_view;
    glm::mat4 m_proj;
    glm::mat4 m_prevViewProj;
    glm::vec3 m_cameraPos;
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};

    // Light data
    glm::vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
    VkBuffer m_lightBuffer{VK_NULL_HANDLE};
    uint32_t m_lightCount{0};

    // Debug modes
    bool m_wireframeEnabled{false};
    bool m_gizmoEnabled{false};

    // Sky state
    bool      m_skyEnabled{true};
    glm::vec3 m_skyGroundColor{0.08f, 0.07f, 0.06f};
    float     m_skyTurbidity{2.5f};
    float     m_skyIntensity{1.0f};
    glm::vec3 m_skySunDirection{0.3f, 0.9f, 0.3f};

    // Night sky state (derived from sun direction each frame)
    float     m_nightFactor{0.0f};
    glm::vec3 m_moonDirection{0.0f, 0.5f, 0.3f};

    // Cloud state
    bool  m_cloudEnabled{false};
    float m_cloudCoverage{0.5f};
    float m_cloudDensity{0.45f};
    float m_cloudAltMin{1500.0f};
    float m_cloudAltMax{8000.0f};
    float m_cloudSpeed{1.0f};

    // Rain state
    bool  m_rainEnabled{false};
    float m_rainIntensity{1.0f};
    float m_rainWindX{-0.08f};

    // Snow state
    bool  m_snowEnabled{false};
    float m_snowIntensity{1.0f};
    float m_snowWindX{-0.08f};

    // Snow accumulation (temporal — like wetness but for blizzard)
    float m_snowAccumulation{0.0f}; // current ground cover [0, 1]
    float m_snowAccumRate{0.02f};   // units/sec to accumulate (~50s to max)
    float m_snowMeltRate{0.003f};   // units/sec to melt      (~333s to melt)

    // Ground wetness (temporal integration — driven from rain state each frame)
    float m_wetness{0.0f};   // current surface wetness [0, 1]
    float m_wetRate{0.03f};  // units/sec to accumulate (default: ~33s to max)
    float m_dryRate{0.005f}; // units/sec to dry        (default: ~200s to dry)

    // Frost (temporal — accumulates when snow > 0.6, melts when snow clears)
    float m_frostCover{0.0f};
    float m_frostAccumRate{0.005f};
    float m_frostMeltRate{0.002f};

    // Sand state
    bool  m_sandEnabled{false};
    float m_sandIntensity{1.0f};
    float m_sandWindX{-0.08f};

    // God rays state
    bool  m_godRaysEnabled{true};
    float m_godRaysIntensity{1.0f};

    // Aurora state
    bool  m_auroraEnabled{false};
    float m_auroraIntensity{0.5f};
    float m_auroraHue{0.0f};

    // Rainbow state
    bool  m_rainbowEnabled{true}; // auto-enabled when rain is active

    // Water state
    bool  m_waterEnabled{false};
    float m_waterLevel{0.0f};
    float m_waterSize{1024.0f};
    float m_waterFoamIntensity{0.6f};
    float m_waterWaveAmplitude{0.3f};
    // Normal map paths — stored so they can be reloaded after resize
    std::string m_waterNormalMap1Path;
    std::string m_waterNormalMap2Path;

    // Wave mode (0=Gerstner, 1=FFT)
    int   m_waveMode{0};
    float m_fftWindSpeed{8.0f};
    float m_fftWindDirX{1.0f};
    float m_fftWindDirZ{0.0f};
    float m_fftPatchSize{500.0f};
    float m_fftChoppiness{1.4f};
    float m_fftNormalStrength{1.0f};

    // Caustics state
    bool        m_causticsEnabled{false};
    float       m_causticsIntensity{0.5f};
    std::string m_causticsTexturePath;

    // Water ripple state
    bool  m_waterRipplesEnabled{false};

    // Enhanced water
    float       m_waterSSSStrength{0.35f};
    std::string m_waterFoamTexturePath;

    // Underwater state
    bool      m_underwaterEnabled{true};
    glm::vec3 m_underwaterFogColor{0.04f, 0.14f, 0.28f};
    float     m_underwaterFogDensity{0.12f};
    float     m_underwaterChromStrength{0.006f};
    float     m_underwaterDistortFreq{12.0f};
    float     m_underwaterDistortSpeed{1.2f};

    // Water ripple fine-tuning
    float     m_waterRippleDamping{0.005f};
    float     m_waterRippleSpeed{8.0f};

    // Caustics scale
    float     m_causticsScale{0.08f};

    // Water grid LOD
    int       m_waterGridN{64};

    // Wind (derived from rain/sand wind state; used by foliage and future passes)
    glm::vec3 m_windDirection{1.0f, 0.0f, 0.0f};
    float     m_windStrength{0.1f};

    // Terrain state
    bool        m_terrainEnabled{false};
    float       m_terrainHeightScale{100.0f};
    float       m_terrainSize{1024.0f};

    // Multi-tile terrain streaming
    std::vector<TerrainTile> m_terrainTiles;
    float                    m_terrainTileCullRadius{1200.0f};
    std::string m_terrainHeightmapPath;
    std::string m_terrainSplatMapPath;
    std::array<std::string, 4> m_terrainLayerAlbedoPaths;
    std::array<std::string, 4> m_terrainLayerNormalPaths;
    VkSampler   m_terrainLayerSampler{VK_NULL_HANDLE}; // owned, created on first use

    // Decals state
    bool m_decalsEnabled{true};

    // Foliage state
    bool        m_foliageEnabled{false};
    float       m_foliageCullDistance{120.0f};
    std::string m_grassTexturePath;

    // Lightning flash state machine
    bool  m_lightningEnabled{false};
    float m_lightningInterval{8.0f};    // base seconds between strikes
    float m_lightningBrightness{3.5f};  // peak flash HDR value
    float m_lightningTimer{0.0f};       // countdown to next strike
    float m_flashIntensity{0.0f};       // current flash (drives post-processing)
    float m_flickerTimer{0.0f};         // countdown to secondary flicker
    bool  m_flickerFired{true};         // secondary flicker already happened this strike
    bool  m_lightningPending{false};    // drain flag for GDScript signal
    bool  m_lightningTimerForce{false}; // manual trigger from GDScript

    // Particle system
    std::unique_ptr<ParticleSystem> m_particleSystem;
    VkRenderPass m_particleRenderPass{VK_NULL_HANDLE};
    VkFramebuffer m_particleFramebuffer{VK_NULL_HANDLE};
    float m_deltaTime{0.016f};
    float m_totalTime{0.0f};
    std::queue<ParticleEmitterConfig> m_pendingEmits;

    bool createParticleRenderPass();
    bool createParticleFramebuffer();

    // Render graph for centralized barrier tracking
    RenderGraph m_renderGraph;

    // Imported texture handles (valid after importGraphTextures())
    TextureHandle m_graphDepthHandle;
    TextureHandle m_graphNormalHandle;
    TextureHandle m_graphAlbedoHandle;
    TextureHandle m_graphShadowHandle;
    TextureHandle m_graphSSAOHandle;
    TextureHandle m_graphSSGIHandle;
    TextureHandle m_graphLightingHandle;

    // (Re-)import all per-pass render targets into the graph.
    // Called once after initialize() and again after each onResize().
    void importGraphTextures();
};

} // namespace ohao
