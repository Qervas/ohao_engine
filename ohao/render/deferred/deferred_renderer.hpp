#pragma once

// Core passes only — stripped to essentials
#include "gbuffer_pass.hpp"
#include "gpu_culling.hpp"
#include "light_clusters.hpp"
#include "deferred_lighting_pass.hpp"
#include "csm_pass.hpp"
#include "post_processing_pipeline.hpp"
#include "gizmo_pass.hpp"
#include "render/hud/hud_pass.hpp"
#include "sky_pass.hpp"
#include "render/vfx/vfx_renderer.hpp"
#include "ssr_pass.hpp"
#include "sss_pass.hpp"
#include "render/rt/rt_shadow_technique.hpp"
#include "render/rt/rt_gi_technique.hpp"
#include "render/rt/rt_alpha_tables.hpp"
#include "render/graph/render_graph.hpp"
#include "render/scene/render_scene_types.hpp"
#include "core/common_types.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>
#include <queue>
#include <span>
#include <string>
#include <vector>
#include <string_view>
#include <algorithm>
#include <array>

namespace ohao::render { class RenderScene; class GpuPassTimer; }

namespace ohao {

// Full deferred rendering pipeline that orchestrates all passes
// This is the main entry point for AAA-quality rendering
class DeferredRenderer {
public:
    DeferredRenderer() = default;
    ~DeferredRenderer();

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    // VK_KHR_ray_query is enabled on the device (before initialize): RT shadows trace with
    // inline ray queries, and without them the frame keeps CSM.
    void setRayQuerySupported(bool on) noexcept { m_rayQuery = on; }
    // VK_EXT_mesh_shader is enabled (before initialize): the G-buffer's meshlet path runs task
    // and mesh shaders, else its compute + vertex-pulling fallback.
    void setMeshShaderSupported(bool on) noexcept { m_meshShader = on; }
    void cleanup();
    [[nodiscard]] bool isInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

    // Main render function - call this once per frame
    void render(VkCommandBuffer cmd, uint32_t frameIndex);

    // Resize handling
    void onResize(uint32_t width, uint32_t height);

    // Restart everything that carries over from frame to frame (the clock behind RT GI's
    // seeds and the sky, RT GI's and TAA's history, the previous view-projection), so the
    // frames that follow render as a fresh renderer's would. The device must be idle.
    void resetTemporalState();

    // RT shadows and RT GI seed every sample from (render seed, frame index since the last
    // resetTemporalState, pixel, sample): frame k after a reset is the same in every run.
    void setRenderSeed(uint32_t seed) noexcept { m_renderSeed = seed; }
    [[nodiscard]] uint32_t framesSinceReset() const noexcept { return m_framesSinceReset; }
    // The renderer's number of the frame about to be recorded (RT shadows' dropped-pixel count).
    void setFrameNumber(uint64_t frame) noexcept { m_frameNumber = frame; }

    // Realtime (the budgeted frame) or reference (RT shadows: every light, 8 rays, the plain
    // mean of the frames since the last reset). Change it only between captures that reset.
    void setQuality(RenderQuality quality) noexcept { m_quality = quality; }
    [[nodiscard]] RenderQuality quality() const noexcept { return m_quality; }
    // Realtime RT shadows: the lights a pixel traces at most (0-4), the most important first.
    void setShadowLightsPerPixel(uint32_t n) noexcept { m_shadowLightsPerPixel = std::min(n, 4u); }
    [[nodiscard]] uint32_t shadowLightsPerPixel() const noexcept { return m_shadowLightsPerPixel; }

    // render.settings switches (between frames). RT GI on or off (off: no indirect light), its
    // realtime trace at half or full resolution and its rays a pixel (1-32); SSR and SSS on or
    // off; the CSM fallback's cascades (1-4) and size (512-4096; a new size rebuilds the map).
    void setRTGIEnabled(bool on) noexcept { m_rtgiWanted = on; }
    [[nodiscard]] bool rtGIEnabled() const noexcept { return m_rtgiWanted; }
    [[nodiscard]] bool rtGIAvailable() const noexcept { return m_useRTGI && m_rtGI != nullptr; }
    void setRTGIRealtime(bool fullResolution, uint32_t rays);
    [[nodiscard]] bool rtGIFullResolution() const noexcept { return m_giFull; }
    [[nodiscard]] uint32_t rtGIRays() const noexcept { return m_giRays; }
    void setSSREnabled(bool on) noexcept { m_ssrWanted = on; }
    [[nodiscard]] bool ssrEnabled() const noexcept { return m_ssrWanted; }
    void setSSSEnabled(bool on) noexcept { m_sssWanted = on; }
    [[nodiscard]] bool sssEnabled() const noexcept { return m_sssWanted; }
    void configureCSM(uint32_t cascades, uint32_t size);
    [[nodiscard]] uint32_t csmCascades() const noexcept { return m_csmPass ? m_csmPass->cascades() : 4u; }
    [[nodiscard]] uint32_t csmSize() const noexcept { return m_csmPass ? m_csmPass->size() : 2048u; }

    // Scene configuration. The G-buffer and CSM passes draw the RenderScene, RT
    // shadows/GI trace its TLAS, and RT GI takes its bounce light from its lights.
    void setRenderScene(const render::RenderScene* scene);

    // Camera configuration
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       const glm::vec3& position, float nearPlane, float farPlane);

    // Light configuration
    void setDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    // This frame's raster lights (the light table, in order; the caller keeps them alive until
    // render() returns), their version (unchanged lights are not uploaded again; 0: always) and
    // the ambient intensity.
    void setLights(std::span<const LightData> lights, uint64_t version, float ambient);
    // Clustered lighting on (the default) or every pixel summing every light (tests: the same
    // image when no froxel overflows).
    void setClusteredLighting(bool on) noexcept { m_clusteredLighting = on; }
    // The farthest view distance the froxel slices cover: the camera's far plane (a pixel past
    // the last slice's box would miss the lights there).
    void setClusterFarPlane(float farPlane) noexcept { m_clusterFar = farPlane; }
    [[nodiscard]] LightClusters* lightClusters() noexcept { return m_lightClusters.get(); }
    // The lights RT shadows trace this frame, <= kMaxShadowLights, light i into visibility
    // channel i (the light UBO's params.w points each raster light at its channel).
    void setShadowLights(std::span<const render::LightDesc> lights) { m_shadowLights.assign(lights.begin(), lights.end()); }
    [[nodiscard]] glm::vec3 getLightDirection() const { return m_lightDirection; }
    [[nodiscard]] float getNightFactor() const { return m_nightFactor; }
    [[nodiscard]] glm::vec3 getMoonDirection() const { return m_moonDirection; }

    // Texture manager (forwarded to GBufferPass)
    void setTextureManager(BindlessTextureManager* texManager);

    // Post-processing configuration
    [[nodiscard]] PostProcessingPipeline* getPostProcessing() { return m_postProcessing.get(); }
    [[nodiscard]] const GBufferPass* getGBufferPass() const { return m_gbufferPass.get(); }
    // GPU culling: its settings (frustum, occlusion) and, for tests, its captured lists.
    [[nodiscard]] GpuCulling* culling() noexcept { return m_culling.get(); }
    [[nodiscard]] const SSSPass* getSSSPass() const { return m_sssPass.get(); }
    [[nodiscard]] const SSRPass* getSSRPass() const { return m_ssrPass.get(); }
    [[nodiscard]] const RTShadowTechnique* getRTShadow() const { return m_rtShadow.get(); }
    [[nodiscard]] RTShadowTechnique* rtShadow() { return m_rtShadow.get(); }

    // Times every pass render() records, by its render/frame/pass_registry.hpp name (the
    // renderer's per-slot timer; null: untimed).
    void setPassTimer(render::GpuPassTimer* timer) noexcept { m_passTimer = timer; }

    // Wireframe mode
    void setWireframeEnabled(bool enabled);
    [[nodiscard]] bool getWireframeEnabled() const { return m_wireframeEnabled; }

    // The HUD the next frames draw over the final image (sub-project 7; empty: none, no pass).
    void setHud(std::span<const render::HudItem> items);

    // Gizmo controls
    void setGizmoEnabled(bool enabled);
    [[nodiscard]] bool getGizmoEnabled() const { return m_gizmoEnabled; }
    void setGizmoMode(GizmoMode mode);
    void setGizmoTransform(const glm::mat4& model);
    void setGizmoHighlightedAxis(GizmoAxis axis);

    // Sky configuration
    void setSkyEnabled(bool enabled);

    // RT shadows and GI
    [[nodiscard]] RTGITechnique* getRT_GI() { return m_rtGI.get(); }
    void setRTShadowsEnabled(bool enabled) { m_useRTShadows = enabled; }
    [[nodiscard]] bool getRTShadowsEnabled() const { return m_useRTShadows; }
    // The environment: `source` (RGBA32F, one level) for RT GI's misses; the prefiltered levels
    // and irradiance (EnvPrefilter) for lighting's IBL, null without them; `intensity`
    // (Environment.intensity) scales both, as the path tracer's miss does.
    void setEnvMap(VkImageView source, VkImageView prefiltered, VkImageView irradiance, VkSampler iblSampler,
                   float intensity);

    [[nodiscard]] bool getSkyEnabled() const { return m_skyEnabled; }
    void setSunDirection(const glm::vec3& dir);
    void setSkyTurbidity(float t);
    void setSkyIntensity(float i);
    void setSkyGroundColor(const glm::vec3& c);


    // The effects and blended surfaces (sub-project 12): the RenderScene's VfxFrame and
    // kDrawTransparent draws, expanded, sorted and drawn by the transparent pass. Null when its
    // pipelines could not be made (no ray queries or bindless textures). Tests read its stats.
    [[nodiscard]] VfxRenderer* vfxRenderer() noexcept { return m_vfx.get(); }
    void setDeltaTime(float dt) { m_deltaTime = dt; }

    // Get final output for display/readback
    [[nodiscard]] VkImageView getFinalOutput() const;
    [[nodiscard]] VkImage getFinalOutputImage() const;
    // True when the final image is the post stack's RGBA8 output; false when it is the
    // lighting pass's RGBA16F HDR image (tonemapping off), which a byte copy cannot read back.
    [[nodiscard]] bool finalOutputIsRGBA8() const;

    // Get SSAO output for deferred lighting
    [[nodiscard]] VkImageView getSSAOOutput() const;

    // Get jitter offset for TAA
    [[nodiscard]] glm::vec2 getJitterOffset(uint32_t frameIndex) const;

    // === Hot-reload (runtime shader swap) ===
    [[nodiscard]] bool reloadShaderForPass(std::string_view passName, std::string_view spvPath);

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Render passes
    std::unique_ptr<GBufferPass> m_gbufferPass;
    std::unique_ptr<CSMPass> m_csmPass;
    std::unique_ptr<GpuCulling> m_culling;   // the G-buffer's and CSM's draw lists

    // RT shadow technique (replaces CSM when available)
    std::unique_ptr<RTShadowTechnique> m_rtShadow;
    bool m_useRTShadows{false};
    bool m_rayQuery{false};
    bool m_meshShader{false};
    std::unique_ptr<RTGITechnique> m_rtGI;
    // RenderScene's tables for the RT traces' alpha tests (set 1; the bindless textures are set 2).
    rtc::AlphaTables m_alphaTables;
    bool m_alphaTablesReady{false};
    bool m_useRTGI{false};
    bool m_rtgiWanted{true};
    bool m_giFull{false};
    uint32_t m_giRays{2};
    bool m_ssrWanted{true};
    bool m_sssWanted{true};
    VkImageView m_envView{VK_NULL_HANDLE};   // setEnvMap: what RT GI's escaping rays see
    float m_envIntensity{1.0f};              // setEnvMap: Environment.intensity
    std::unique_ptr<DeferredLightingPass> m_lightingPass;
    std::unique_ptr<SSRPass> m_ssrPass;
    std::unique_ptr<SSSPass> m_sssPass;
    std::unique_ptr<PostProcessingPipeline> m_postProcessing;
    std::unique_ptr<GizmoPass> m_gizmoPass;
    std::unique_ptr<HudPass> m_hudPass;
    std::unique_ptr<SkyPass>   m_skyPass;

    // Scene the passes draw (owned by VulkanRenderer)
    const render::RenderScene* m_renderScene{nullptr};

    // Texture manager (set before initialize)
    BindlessTextureManager* m_textureManager{nullptr};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Camera data
    glm::mat4 m_view{1.0f};
    glm::mat4 m_proj{1.0f};
    // Last frame's unjittered view-projection, for motion vectors. Until a frame has been
    // rendered (a fresh renderer, or after resetTemporalState) there is none, and the
    // frame uses its own: no motion into the first frame. glm leaves a default-constructed
    // mat4 uninitialized, and TAA's first frame blends towards the cleared history by the
    // motion it sees, so an unset matrix made the first image depend on stale memory.
    glm::mat4 m_prevViewProj{1.0f};
    bool m_prevViewProjValid{false};
    glm::vec3 m_cameraPos{0.0f};
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};

    // Light data
    glm::vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
    std::span<const LightData> m_lights;
    uint64_t m_lightsVersion{0};
    float m_ambient{0.35f};
    bool m_clusteredLighting{true};
    float m_clusterFar{1000.0f};
    std::unique_ptr<LightClusters> m_lightClusters;   // the light table and the froxel lists
    std::vector<render::LightDesc> m_shadowLights;

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


    // Effects and blended surfaces: vfx.expand, vfx.sort and the transparent pass.
    std::unique_ptr<VfxRenderer> m_vfx;
    VkImageView m_envPrefiltered{VK_NULL_HANDLE};   // setEnvMap: the transparent pass's IBL
    VkImageView m_envIrradiance{VK_NULL_HANDLE};
    float m_deltaTime{0.016f};
    float m_totalTime{0.0f};   // the sky's star seed
    uint32_t m_renderSeed{0};
    uint32_t m_framesSinceReset{0};   // render() calls since the last resetTemporalState
    uint64_t m_frameNumber{0};
    RenderQuality m_quality{RenderQuality::Realtime};
    uint32_t m_shadowLightsPerPixel{4};

    // Render graph for centralized barrier tracking
    RenderGraph m_renderGraph;

    // Imported texture handles (valid after importGraphTextures())
    TextureHandle m_graphDepthHandle;
    TextureHandle m_graphNormalHandle;
    TextureHandle m_graphAlbedoHandle;
    TextureHandle m_graphShadowHandle;
    TextureHandle m_graphLightingHandle;

    // (Re-)import all per-pass render targets into the graph.
    // Called once after initialize() and again after each onResize().
    void importGraphTextures();

    render::GpuPassTimer* m_passTimer{nullptr};   // VulkanRenderer's, per frame slot
};

} // namespace ohao
