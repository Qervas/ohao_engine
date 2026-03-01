#pragma once

#include "gbuffer_pass.hpp"
#include "deferred_lighting_pass.hpp"
#include "csm_pass.hpp"
#include "post_processing_pipeline.hpp"
#include "overlay_pass.hpp"
#include "gizmo_pass.hpp"
#include "sky_pass.hpp"
#include "cloud_pass.hpp"
#include "rain_pass.hpp"
#include "renderer/particles/particle_system.hpp"
#include "renderer/graph/render_graph.hpp"
#include "utils/common_types.hpp"
#include <memory>
#include <unordered_map>
#include <queue>

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

    // Grid overlay
    void setGridEnabled(bool enabled) { m_gridEnabled = enabled; }
    bool getGridEnabled() const { return m_gridEnabled; }

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

    // Ground wetness — temporal integration driven by rain state.
    // Surfaces accumulate wetness at wetRate/s and dry at dryRate/s.
    // Surface wetness is readable so scripts can react (footstep sounds, etc.)
    float getSurfaceWetness() const { return m_wetness; }
    void  setWetnessRate(float r)   { m_wetRate  = glm::clamp(r, 0.0f, 10.0f); }
    void  setDryingRate(float r)    { m_dryRate  = glm::clamp(r, 0.0f, 10.0f); }

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

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Render passes
    std::unique_ptr<GBufferPass> m_gbufferPass;
    std::unique_ptr<CSMPass> m_csmPass;
    std::unique_ptr<DeferredLightingPass> m_lightingPass;
    std::unique_ptr<PostProcessingPipeline> m_postProcessing;
    std::unique_ptr<OverlayPass> m_overlayPass;
    std::unique_ptr<GizmoPass> m_gizmoPass;
    std::unique_ptr<SkyPass>   m_skyPass;
    std::unique_ptr<CloudPass> m_cloudPass;
    std::unique_ptr<RainPass>  m_rainPass;

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
    bool m_gridEnabled{true};
    bool m_gizmoEnabled{false};

    // Sky state
    bool      m_skyEnabled{true};
    glm::vec3 m_skyGroundColor{0.08f, 0.07f, 0.06f};
    float     m_skyTurbidity{2.5f};
    float     m_skyIntensity{1.0f};
    glm::vec3 m_skySunDirection{0.3f, 0.9f, 0.3f};

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

    // Ground wetness (temporal integration — driven from rain state each frame)
    float m_wetness{0.0f};   // current surface wetness [0, 1]
    float m_wetRate{0.03f};  // units/sec to accumulate (default: ~33s to max)
    float m_dryRate{0.005f}; // units/sec to dry        (default: ~200s to dry)

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
