#pragma once

// Core passes only — stripped to essentials
#include "gbuffer_pass.hpp"
#include "deferred_lighting_pass.hpp"
#include "csm_pass.hpp"
#include "post_processing_pipeline.hpp"
#include "gizmo_pass.hpp"
#include "sky_pass.hpp"
#include "render/particles/particle_system.hpp"
#include "ssr_pass.hpp"
#include "sss_pass.hpp"
#include "render/rt/rt_shadow_technique.hpp"
#include "render/rt/rt_gi_technique.hpp"
#include "render/graph/render_graph.hpp"
#include "core/common_types.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>
#include <queue>
#include <string>
#include <string_view>
#include <array>

namespace ohao {

class Scene;

// Full deferred rendering pipeline that orchestrates all passes
// This is the main entry point for AAA-quality rendering
class DeferredRenderer {
public:
    DeferredRenderer() = default;
    ~DeferredRenderer();

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();
    [[nodiscard]] bool isInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

    // Main render function - call this once per frame
    void render(VkCommandBuffer cmd, uint32_t frameIndex);

    // Resize handling
    void onResize(uint32_t width, uint32_t height);

    // Scene configuration
    void setScene(Scene* scene);

    // Geometry buffers (from VulkanRenderer)
    void setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer,
                            const std::unordered_map<uint64_t, MeshBufferInfo>* bufferMap);

    // Camera configuration
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       const glm::vec3& position, float nearPlane, float farPlane);

    // Light configuration
    void setDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void setLightBuffer(VkBuffer lightBuffer, uint32_t lightCount);
    [[nodiscard]] glm::vec3 getLightDirection() const { return m_lightDirection; }
    [[nodiscard]] float getNightFactor() const { return m_nightFactor; }
    [[nodiscard]] glm::vec3 getMoonDirection() const { return m_moonDirection; }

    // IBL configuration (optional)
    void setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                        VkImageView brdfLUT, VkSampler iblSampler);

    // Texture manager (forwarded to GBufferPass)
    void setTextureManager(BindlessTextureManager* texManager);

    // Post-processing configuration
    [[nodiscard]] PostProcessingPipeline* getPostProcessing() { return m_postProcessing.get(); }

    // Wireframe mode
    void setWireframeEnabled(bool enabled);
    [[nodiscard]] bool getWireframeEnabled() const { return m_wireframeEnabled; }

    // Gizmo controls
    void setGizmoEnabled(bool enabled);
    [[nodiscard]] bool getGizmoEnabled() const { return m_gizmoEnabled; }
    void setGizmoMode(GizmoMode mode);
    void setGizmoTransform(const glm::mat4& model);
    void setGizmoHighlightedAxis(GizmoAxis axis);

    // Sky configuration
    void setSkyEnabled(bool enabled);

    // RT shadows
    [[nodiscard]] RTGITechnique* getRT_GI() { return m_rtGI.get(); }
    void setRTShadowsEnabled(bool enabled) { m_useRTShadows = enabled; }
    [[nodiscard]] bool getRTShadowsEnabled() const { return m_useRTShadows; }
    void setAccelerationStructure(RTAccelerationStructure* accel) { m_rtAccel = accel; }
    void setEnvMap(VkImageView view, VkSampler sampler);

private:
    // stored here for RT passes to access
    RTAccelerationStructure* m_rtAccel{nullptr};
public:
    [[nodiscard]] bool getSkyEnabled() const { return m_skyEnabled; }
    void setSunDirection(const glm::vec3& dir);
    void setSkyTurbidity(float t);
    void setSkyIntensity(float i);
    void setSkyGroundColor(const glm::vec3& c);


    // Particle system
    void spawnParticles(const glm::vec3& position, ParticleType type,
                        const glm::vec3& direction = glm::vec3(0.0f, 1.0f, 0.0f));
    void setDeltaTime(float dt) { m_deltaTime = dt; }

    // Get final output for display/readback
    [[nodiscard]] VkImageView getFinalOutput() const;
    [[nodiscard]] VkImage getFinalOutputImage() const;

    // Get SSAO output for deferred lighting
    [[nodiscard]] VkImageView getSSAOOutput() const;

    // Get jitter offset for TAA
    [[nodiscard]] glm::vec2 getJitterOffset(uint32_t frameIndex) const;

    // === Introspection (MCP AI agent support) ===
    [[nodiscard]] nlohmann::json getPipelineInfo() const;
    [[nodiscard]] nlohmann::json getPerfStats() const;

    // === Hot-reload (runtime shader swap) ===
    [[nodiscard]] bool reloadShaderForPass(std::string_view passName, std::string_view spvPath);

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Render passes
    std::unique_ptr<GBufferPass> m_gbufferPass;
    std::unique_ptr<CSMPass> m_csmPass;

    // RT shadow technique (replaces CSM when available)
    std::unique_ptr<RTShadowTechnique> m_rtShadow;
    bool m_useRTShadows{false};
    std::unique_ptr<RTGITechnique> m_rtGI;
    bool m_useRTGI{false};
    std::unique_ptr<DeferredLightingPass> m_lightingPass;
    std::unique_ptr<SSRPass> m_ssrPass;
    std::unique_ptr<SSSPass> m_sssPass;
    std::unique_ptr<PostProcessingPipeline> m_postProcessing;
    std::unique_ptr<GizmoPass> m_gizmoPass;
    std::unique_ptr<SkyPass>   m_skyPass;

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


    // Particle system
    std::unique_ptr<ParticleSystem> m_particleSystem;
    VkRenderPass m_particleRenderPass{VK_NULL_HANDLE};
    VkFramebuffer m_particleFramebuffer{VK_NULL_HANDLE};
    float m_deltaTime{0.016f};
    float m_totalTime{0.0f};
    std::queue<ParticleEmitterConfig> m_pendingEmits;

    [[nodiscard]] bool createParticleRenderPass();
    [[nodiscard]] bool createParticleFramebuffer();

    // Render graph for centralized barrier tracking
    RenderGraph m_renderGraph;

    // Imported texture handles (valid after importGraphTextures())
    TextureHandle m_graphDepthHandle;
    TextureHandle m_graphNormalHandle;
    TextureHandle m_graphAlbedoHandle;
    TextureHandle m_graphShadowHandle;
    TextureHandle m_graphSSAOHandle;
    TextureHandle m_graphLightingHandle;

    // (Re-)import all per-pass render targets into the graph.
    // Called once after initialize() and again after each onResize().
    void importGraphTextures();

    // === GPU Timing ===
    static constexpr int GPU_TIMER_COUNT = 25;  // max passes to time
    VkQueryPool m_timestampPool{VK_NULL_HANDLE};
    float m_timestampPeriod{1.0f};  // nanoseconds per tick
    bool m_gpuTimingEnabled{false};
    std::array<float, GPU_TIMER_COUNT> m_passTimingsMs{};  // per-pass ms from last frame
    std::array<const char*, GPU_TIMER_COUNT> m_passTimingNames{};
    int m_passTimingCount{0};

    [[nodiscard]] bool initGpuTiming();
    void cleanupGpuTiming();
    void readbackGpuTimings();  // read previous frame's results
};

} // namespace ohao
