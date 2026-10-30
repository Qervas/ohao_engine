#pragma once

#include "render/display/display.hpp"
#include "render/sky/atmosphere.hpp"

#include <optional>
#include <array>
#include "gpu/vulkan/swapchain.hpp"
#include "gpu/vulkan/present_target.hpp"
#include "gpu/vulkan/present_waiter.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>
#include <functional>
#include <glm/glm.hpp>
#include "gpu/vulkan/vk_utils.hpp"
#include "core/common_types.hpp"
#include "core/concepts.hpp"
#include "gpu/layout_meta.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/frame/gpu_pass_timer.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/rt/rt_acceleration_structure.hpp"
#include "render/rt/path_tracer.hpp"
#include "render/rt/rt_profile_renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/rt_profile_renderer.hpp"
#include "render/scene/render_scene_types.hpp"
#include "render/scene/model_upload.hpp"
#include "render/rt/render_technique.hpp"
#include "render/deferred/light_clusters.hpp"
#include "render/deferred/gpu_culling.hpp"
#include "render/deferred/env_prefilter.hpp"
#include "render/hud/hud_list.hpp"

namespace ohao {

namespace render { class RenderScene; }

class Camera;
class CapturePreviewPass;
class DeferredRenderer;

// Render mode selection
enum class RenderMode {
    Deferred,     // Raster path: G-buffer, CSM, SSAO, post-processing (+ hybrid RT shadows/GI)
    RTRealtime,   // Path tracing tuned for interactive use
    RTOffline     // Path tracing tuned for reference/offline rendering
};

[[nodiscard]] constexpr bool isRTRenderMode(RenderMode m) noexcept {
    return m == RenderMode::RTRealtime || m == RenderMode::RTOffline;
}

[[nodiscard]] constexpr bool isRasterRenderMode(RenderMode m) noexcept {
    return m == RenderMode::Deferred;
}

// Simple vertex structure for basic rendering (Phase 1 triangle)
struct SimpleVertex {
    glm::vec3 position{};
    glm::vec3 color{};
};
static_assert(GpuPod<SimpleVertex>, "SimpleVertex must be GPU POD");

// Camera uniform buffer (view/proj matrices + view position)
struct CameraUniformBuffer {
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 proj{1.0f};
    alignas(16) glm::vec3 viewPos{0.0f};
};
static_assert(GpuPod<CameraUniformBuffer>, "CameraUniformBuffer must be GPU POD");

// LightData and MAX_LIGHTS live in render/scene/render_scene_types.hpp.

// Shadow map configuration
constexpr uint32_t SHADOW_MAP_SIZE = 2048;


/**
 * VulkanRenderer - Renders OHAO scenes headless into a pixel buffer, or to a window
 * when initialized with a PresentTarget (see ohao/runtime/LiveHost).
 *
 * The scene is renderScene(): an ecs::World with installRenderExtract feeds it
 * (examples/cornell_box.cpp). Headless usage:
 *   VulkanRenderer renderer(800, 600);
 *   renderer.initialize();
 *   ecs::installRenderExtract(world, renderer.renderScene());
 *   world.sync();
 *   renderer.requestReadback(); renderer.render();
 *   std::span<const uint8_t> pixels = renderer.readback();   // that frame, RGBA8
 */
class VulkanRenderer {
public:
    VulkanRenderer(uint32_t width, uint32_t height);
    ~VulkanRenderer();

    // Lifecycle
    [[nodiscard]] bool initialize();
    // Windowed: present into a swapchain on `target`'s surface. Frames end with a
    // blit into the acquired swapchain image; nothing is read back to the CPU.
    [[nodiscard]] bool initialize(const PresentTarget& target);
    [[nodiscard]] bool isPresenting() const noexcept { return m_swapchain != nullptr; }
    // Sub-project 9: the present mode the surface gave, whether presents are confirmed by the
    // presentation engine (VK_KHR_present_wait), and when each presented frame was presented
    // (gpu/vulkan/present_waiter.hpp), resolved since the last call, oldest first.
    [[nodiscard]] PresentMode presentMode() const noexcept {
        return m_swapchain ? m_swapchain->presentMode() : PresentMode::Fifo;
    }
    [[nodiscard]] bool presentWaitEnabled() const noexcept { return m_presentWaitEnabled; }
    [[nodiscard]] std::vector<PresentStamp> takePresentStamps() { return m_presentWaiter.take(); }
    // Late input sampling: returns once frame `frame` (a framesSubmitted() number) has been
    // presented, as the presentation engine confirms it (at most `timeout`), or without present
    // wait once its GPU work is done.
    void waitForPresentOf(uint64_t frame, std::chrono::microseconds timeout) {
        if (m_presentWaitEnabled) (void)m_presentWaiter.waitResolved(frame, timeout);
        else waitForSubmitted(frame);
    }
    // When the newest frame's command buffers were submitted (framesSubmitted()'s frame).
    [[nodiscard]] std::chrono::steady_clock::time_point lastSubmitTime() const noexcept { return m_lastSubmitAt; }
    // The window's drawable size changed; the swapchain is rebuilt next frame.
    void setPresentExtent(VkExtent2D extent);
    // The HUD the next deferred frames draw over their final image, before present and readback
    // (sub-project 7, the `hud` pass); an empty list draws none. Path-traced frames draw no HUD.
    void setHud(const render::HudList& hud);
    // The only way to read a frame. requestReadback() marks the next frame render()
    // records: it alone copies its final RGBA8 image into its slot's persistently mapped
    // staging buffer (no other frame, headless or windowed, copies anything). readback()
    // then waits for that frame's fence (never the queue) and returns its pixels,
    // width * height * 4 bytes, valid until the next render() or resize(); empty when the
    // frame was not rendered (minimized, an out-of-date swapchain) or produced no RGBA8
    // image. A path-traced frame with a CPU-side denoiser (OIDN, NRD) is denoised here,
    // from that frame's images.
    void requestReadback() noexcept {
        m_readbackRequested = true;
        m_previewRequest.reset();
    }
    [[nodiscard]] std::span<const uint8_t> readback();
    // An inline capture's form of the request (capture.preview): the next frame copies
    // nothing but a box-filtered preview of `rect` ([x, y, w, h], inside the frame) whose long
    // side is at most maxSide (render::previewExtent), made on the GPU and written straight
    // into its staging buffer. readbackPreview() then returns it (readback() returns nothing).
    // Deferred frames only (previewOnGpu()); in another mode the frame reads back in full.
    struct PreviewRequest {
        std::array<uint32_t, 4> rect{};
        uint32_t maxSide{0};
    };
    struct Preview {
        std::span<const uint8_t> pixels;   // width * height * 4, RGBA8, rows top down
        uint32_t width{0};
        uint32_t height{0};
    };
    [[nodiscard]] bool previewOnGpu() const noexcept {
        return m_previewPass != nullptr && m_renderMode == RenderMode::Deferred;
    }
    void requestPreviewReadback(const PreviewRequest& request) noexcept {
        m_readbackRequested = true;
        m_previewRequest = request;
    }
    [[nodiscard]] Preview readbackPreview();
    // Frames that copied into their staging buffer: every one of them was requested.
    [[nodiscard]] uint64_t requestedReadbackCount() const noexcept { return m_requestedReadbacks; }
    // Submitted frames whose fence has not signalled yet; asks the fences, never waits.
    [[nodiscard]] uint32_t framesInFlight();
    // Frames that reached the queue so far; waitForSubmitted(n) returns once the n-th has
    // finished on the GPU (at once for 0, or for a frame whose slot was reused since). A
    // capture waits so for the frames queued before the one it reads, then times that
    // frame's readback alone.
    [[nodiscard]] uint64_t framesSubmitted() const noexcept { return m_framesSubmitted; }
    void waitForSubmitted(uint64_t n);
    void shutdown();

    // Rendering
    void render();
    void resize(uint32_t width, uint32_t height);

    // Camera
    [[nodiscard]] Camera& getCamera() { return *m_camera; }
    // The newest finished frame's GPU time in ms (first pass begin to last pass end); 0
    // when timing is unavailable or no frame has finished.
    [[nodiscard]] float gpuFrameMs() const noexcept;
    // Per-pass GPU timings (render/frame/pass_registry.hpp names) of the newest finished
    // frames, at most n, oldest first. Collects every frame whose fence has signalled first,
    // without waiting for one that has not.
    [[nodiscard]] std::vector<render::FrameTiming> frameTimings(std::size_t n);

    // Validation (OHAO_VALIDATION=1 at initialize). validationEnabled() is false when
    // the variable is unset OR the Khronos layer is not installed -- callers must
    // treat that as "not checked", never as "clean".
    [[nodiscard]] bool validationEnabled() const noexcept { return m_validationEnabled; }
    [[nodiscard]] uint64_t validationMessageCount() const noexcept { return m_validationMessages.count.load(); }
    // The first messages, kept so a test can say WHICH rule was broken.
    [[nodiscard]] std::vector<std::string> validationMessages() const {
        std::lock_guard<std::mutex> lock(m_validationMessages.mutex);
        return m_validationMessages.first;
    }
    struct ValidationLog {
        std::atomic<uint64_t> count{0};
        mutable std::mutex mutex;
        std::vector<std::string> first;   // capped
    };
    [[nodiscard]] const Camera& getCamera() const { return *m_camera; }


    // Vulkan handles for out-of-tree consumers (render_bench's GPU calibration kernel). Work
    // submitted on the queue must finish before the next render() (the renderer does not
    // synchronize with it).
    [[nodiscard]] VkDevice getDevice() const noexcept { return m_device; }
    [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const noexcept { return m_physicalDevice; }
    // VK_EXT_mesh_shader is enabled (the meshlet path runs task and mesh shaders).
    [[nodiscard]] bool meshShadersEnabled() const noexcept { return m_meshShaderEnabled; }
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return m_graphicsQueue; }
    [[nodiscard]] uint32_t graphicsQueueFamily() const noexcept { return m_graphicsQueueFamily; }

    // Render mode
    void setRenderMode(RenderMode mode);
    [[nodiscard]] RenderMode getRenderMode() const { return m_renderMode; }
    // Lazy-create the PathTracer for `mode` (avoids dual full-res OOM at 4K).
    [[nodiscard]] bool ensureRTRenderer(RenderMode mode);
    void setRTRenderSettings(const RTRenderSettings& settings);
    void setRTRenderProfile(RTRenderProfile profile);
    [[nodiscard]] const RTRenderSettings& getRTRenderSettings() const { return m_rtSettings; }
    /// HDRI for the path tracer and deferred reflections, loaded at the start of the
    /// next render() when the path changed.
    // The environment: an HDRI (a path), else the physical sky (render/sky/atmosphere.hpp) lit by
    // the scene's sun when `sky` is on and there is a directional light, else none (black).
    void setEnvironmentMap(std::string_view path) { m_envMapPath = std::string(path); }
    void setEnvironmentSky(bool sky) noexcept { m_envSky = sky; }
    /// Scale HDRI contribution (inverse / relight). Default 1.0. Written into the
    /// RT light header (offset 8) every RT frame — no env re-load.
    void setEnvIntensityScale(float s) noexcept {
        m_envIntensityScale = std::max(0.0f, s);
    }
    [[nodiscard]] float getEnvIntensityScale() const noexcept { return m_envIntensityScale; }
    void notifyCameraChanged();
    void resetAccumulation();
    /// The render's one seed: the path tracers' base sample index after an accumulation
    /// reset, the deferred path's RT shadow and GI sample seed (with the frame index since
    /// the reset) and its SSAO noise. Kept, so renderers created later get it.
    void setRenderSeed(uint32_t seed);
    [[nodiscard]] uint32_t getRenderSeed() const noexcept { return m_renderSeed; }
    [[nodiscard]] uint32_t getPathTracerFrameIndex() const;

    // The hybrid deferred frame's quality (sub-project 2b spec, "Reference quality"):
    // realtime, or reference (RT shadows from every light, 8 rays a frame, averaged over the
    // frames since the last resetAccumulation). Captures reset; a live window does not
    // average a moving camera.
    void setRenderQuality(RenderQuality quality) noexcept { m_renderQuality = quality; }
    [[nodiscard]] RenderQuality renderQuality() const noexcept { return m_renderQuality; }
    // RT direct light in the deferred frame (when ray tracing is supported); off: lighting loops
    // over the lights and CSM shadows the sun.
    void setRTDirectEnabled(bool on) noexcept { m_rtDirectWanted = on; }
    [[nodiscard]] bool rtDirectEnabled() const noexcept { return m_rtDirectWanted; }

    // Every quality knob in one value (sub-project 2b spec, "The agent-facing surface":
    // render.settings). The defaults are the budgeted configuration. Set it between frames;
    // a GI resolution or CSM size change idles the device and rebuilds those images.
    // renderSettings() reads the knobs back from the passes that hold them.
    struct RenderSettings {
        RenderQuality quality{RenderQuality::Realtime};
        bool rtDirect{true};
        uint32_t rtDirectCandidates{4};  // light points a pixel draws a frame (1-32)
        uint32_t rtDirectSpatial{2};     // realtime: neighbours a pixel's reservoir reuses (0-8)
        bool rtGI{true};
        bool rtGIFullResolution{false};  // realtime: the trace at the frame's size, not half
        uint32_t rtGIRays{2};            // realtime: rays a pixel (1-32)
        bool ssao{false};
        bool ssr{true};
        bool sss{true};
        GpuCulling::Geometry geometry{GpuCulling::Geometry::Auto};
        bool occlusionCulling{true};
        uint32_t csmCascades{4};         // the CSM fallback (without RT direct light): 1-4
        uint32_t csmSize{2048};          // 512-4096
        float ev100{render::kDefaultEv100};   // the display transform's camera (EV100, ISO 100)
        render::DisplayCurve tonemap{render::DisplayCurve::AgX};
        bool autoExposure{false};          // a window's camera meters its frames (captures never do)
        float exposureCompensation{0.0f};  // EV added to what auto exposure meters
        // render.settings vfx (sub-project 12): presentation switches for A/B captures, all on.
        bool vfxParticles{true};         // the lit particles
        bool vfxSoft{true};              // their soft fade against the scene's depth
        bool vfxTracers{true};
        bool vfxDecals{true};
        bool vfxSmokeInGi{true};         // RT direct light and RT GI attenuate through the smoke proxies
    };
    void setRenderSettings(const RenderSettings& s);
    [[nodiscard]] RenderSettings renderSettings() const;
    // The deferred frame's constant ambient light, in place of the default (0.35); nullopt: the
    // default. A fallback only: lighting applies it to a frame with neither RT GI nor an
    // environment.
    void setAmbientIntensity(std::optional<float> ambient) noexcept { m_ambientOverride = ambient; }
    // Clustered lighting: the newest finished frame's froxel overflow (lights left out of
    // froxels past 256, the froxels that left any out, the largest froxel count).
    // Collects finished frames first.
    [[nodiscard]] std::optional<LightClusters::Overflow> lightsOverflow();
    // Auto exposure: the newest finished frame's metered EV100, if it metered.
    [[nodiscard]] std::optional<float> meteredEv100();
    // Seconds since the last frame (a window's), for auto exposure's adaptation speed.
    void setFrameDeltaTime(float dt);

    // Read back HDR buffers for OIDN denoising
    [[nodiscard]] bool readbackHDRBuffers(std::vector<float>& beauty, std::vector<float>& albedo,
                            std::vector<float>& normal, uint32_t& w, uint32_t& h);
    // The last rendered frame's linear radiance, RGBA32F, row 0 the top: deferred, the lit image
    // (lighting, sky, transparents, SSR) before bloom, TAA, exposure and tonemapping; the path
    // tracer, its accumulation before any denoiser. What the referee compares renderers by.
    // Waits for the device; empty without a frame. Call after the frame (a capture's last).
    [[nodiscard]] std::vector<float> readbackLinear();

    // Deferred renderer access (for configuration)
    [[nodiscard]] DeferredRenderer* getDeferredRenderer() { return m_deferredRenderer.get(); }

    // Texture manager access
    [[nodiscard]] BindlessTextureManager* getTextureManager() { return m_textureManager.get(); }

    // Ray tracing is available: the RenderScene built its acceleration structures.
    [[nodiscard]] bool isRTSupported() const;

    // Persistent GPU render world (meshes, materials, instances, lights). Usable
    // between initialize() and shutdown(); after shutdown() the object stays alive
    // but inert (every edit is ignored) until the renderer is destroyed.
    [[nodiscard]] render::RenderScene& renderScene() { return *m_renderScene; }

    // Denoiser control
    void        setDenoiseMode(DenoiseMode mode);
    [[nodiscard]] DenoiseMode getDenoiseMode() const { return m_denoiseMode; }

    // The path tracer's bounces in place of its profile's (a capture's `bounces`); nullopt: the
    // profile's. Re-applied every frame, as the per-frame reset to the profile would drop it.
    void setMaxBouncesOverride(std::optional<uint32_t> bounces) {
        m_maxBouncesOverride = bounces;
        applyRTRenderSettings();
    }
    [[nodiscard]] std::optional<uint32_t> maxBouncesOverride() const { return m_maxBouncesOverride; }

    // Returns the motion vector AOV image view from the active RT profile,
    // or VK_NULL_HANDLE if no RT profile is active.
    [[nodiscard]] VkImageView getMotionVectorAOV() const;

    // Returns the motion vector AOV VkImage (needed for vkCmdCopyImageToBuffer).
    [[nodiscard]] VkImage getMotionVectorImage() const;

    // Returns the depth / roughness AOV image views and images from the active
    // RT profile, or VK_NULL_HANDLE if no RT profile is active.
    [[nodiscard]] VkImageView getDepthAOV()         const;
    [[nodiscard]] VkImage     getDepthAOVImage()    const;
    [[nodiscard]] VkImageView getRoughnessAOV()     const;
    [[nodiscard]] VkImage     getRoughnessAOVImage() const;
    [[nodiscard]] VkImageView getDiffuseRadianceAOV()      const;
    [[nodiscard]] VkImage     getDiffuseRadianceAOVImage() const;
    [[nodiscard]] VkImageView getSpecularRadianceAOV()      const;
    [[nodiscard]] VkImage     getSpecularRadianceAOVImage() const;
    [[nodiscard]] VkImageView getDiffAlbedoAOV()      const;
    [[nodiscard]] VkImage     getDiffAlbedoAOVImage() const;
    [[nodiscard]] VkImageView getSpecColorAOV()       const;
    [[nodiscard]] VkImage     getSpecColorAOVImage()  const;
    [[nodiscard]] VkImageView getNormalRoughnessAOV()      const;
    [[nodiscard]] VkImage     getNormalRoughnessAOVImage() const;
    // Sub-plan 4.C: NRD denoised outputs (RGBA32F)
    [[nodiscard]] VkImage     getOutDiffRadianceAOVImage() const;
    [[nodiscard]] VkImage     getOutSpecRadianceAOVImage() const;
    // Sub-plan 4.D: NRD composed HDR output (RGBA32F)
    [[nodiscard]] VkImage     getNrdComposedAOVImage() const;

    // Debug: readback the motion vector AOV as raw uint16_t pairs (RG16F interleaved).
    // One 2-half pair per pixel; total = 2 * width * height values.
    // Returns false if no RT profile is active or readback fails.
    [[nodiscard]] bool readbackMotionVector(std::vector<uint16_t>& mvRaw, uint32_t& width, uint32_t& height);

    // Debug: readback the depth AOV as raw float buffer (1 float per pixel).
    [[nodiscard]] bool readbackDepthAOV(std::vector<float>& depthData, uint32_t& width, uint32_t& height);

    // Debug: readback R16F roughness AOV as native float (decoded from half in-helper).
    [[nodiscard]] bool readbackRoughnessAOV(std::vector<float>& roughData, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA32F diffuse radiance AOV (4 floats per pixel, native).
    [[nodiscard]] bool readbackDiffuseRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA32F specular radiance AOV (4 floats per pixel, native).
    [[nodiscard]] bool readbackSpecularRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Sub-plan 4.C: NRD denoised output readback (RGBA32F)
    [[nodiscard]] bool readbackDenoisedDiffuse(std::vector<float>& data, uint32_t& width, uint32_t& height);
    [[nodiscard]] bool readbackDenoisedSpecular(std::vector<float>& data, uint32_t& width, uint32_t& height);
    // Sub-plan 4.D: NRD composed HDR output readback (RGBA32F)
    [[nodiscard]] bool readbackNrdComposed(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 diffuse albedo AOV (4 bytes per pixel).
    [[nodiscard]] bool readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 specular color AOV (4 bytes per pixel).
    [[nodiscard]] bool readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    [[nodiscard]] uint32_t getWidth() const noexcept { return m_width; }
    [[nodiscard]] uint32_t getHeight() const noexcept { return m_height; }
    [[nodiscard]] VkExtent2D getExtent() const noexcept {
        return VkExtent2D{.width = m_width, .height = m_height};
    }


    // Read GPU terrain heightmap back to CPU (blocking — one-time call at setup).
    // Returns false if no procedural terrain exists. outRes = square resolution.
    [[nodiscard]] bool readTerrainHeights(std::vector<float>& outData, uint32_t& outRes);
    // Copies a whole RGBA32F (half: RGBA16F) image, left in `layout` by shaders of `stage`, to the
    // CPU as RGBA32F, and puts it back in that layout. Submits and waits; for readbacks only.
    [[nodiscard]] bool readImageRGBA(VkImage image, bool half, VkImageLayout layout, VkPipelineStageFlags stage,
                                     std::vector<float>& out);

private:
    // Vulkan setup (no window required)
    [[nodiscard]] bool createInstance();
    [[nodiscard]] bool pickPhysicalDevice();
    [[nodiscard]] bool createLogicalDevice();
    [[nodiscard]] bool createCommandPool();

    // Phase 1: Rendering pipeline setup
    [[nodiscard]] bool initializeFrameResources();  // Multi-frame resource initialization
    [[nodiscard]] VkShaderModule loadShaderModule(std::string_view filepath);
    // This frame's raster lights for the deferred renderer: the scene's light table (none
    // without lights, as in the path tracer); sets the ambient intensity.
    void setFrameLights();

    void prepareRTSceneForFrame(const RTRenderSettings& defaults);


    // Rendering internals
    void renderRTPipeline(const RTRenderSettings& defaults);
public:
private:
    // The profile defaults of an RT mode (kRealtimeRTSettings, kOfflineRTSettings), none for
    // deferred. The realtime profile's denoiser is DLSS Ray Reconstruction where the build has the
    // SDK and the device is NVIDIA's, NRD (vendor neutral) elsewhere.
    [[nodiscard]] std::optional<RTRenderSettings> rtDefaults(RenderMode mode) const;
    IRTRendererProfile* getRTRenderer(RenderMode mode);
    const IRTRendererProfile* getRTRenderer(RenderMode mode) const;
    void forEachRTRenderer(const std::function<void(IRTRendererProfile&)>& fn);


    // Dimensions
    uint32_t m_width;
    uint32_t m_height;

    // Vulkan handles
    VkInstance m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};

    // Multi-frame rendering (ring buffer)
    FrameResourceManager m_frameResources;
    // Per-pass GPU timestamps, one query pool per frame slot (render/frame/gpu_pass_timer.hpp).
    render::GpuPassTimer m_passTimer;
    uint32_t m_currentFrame{0};
    uint64_t m_framesStarted{0};   // stamped on log records (log::setFrame) as each frame starts
    uint64_t m_framesSubmitted{0};
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_slotSubmitted{};   // the submitted frame each slot holds

    // Path tracer profiles (the acceleration structures belong to m_renderScene)
    std::unique_ptr<RTRealtimeRenderer> m_rtRealtimeRenderer;
    std::unique_ptr<RTOfflineRenderer> m_rtOfflineRenderer;
    // Environment map (env_upload.cpp): HDR image in the bindless table, its CDF,
    // and the RT bindless view/sampler arrays rebuilt each RT frame.
    // What the environment is: "file:<path>[|sunless]", "sky:<sun>", or empty for none.
    [[nodiscard]] std::string environmentKey() const;
    void uploadEnvironmentMap();          // once per path (render()); stamps env index + scale into the RenderScene
    void bindRTTextures(IRTRendererProfile& renderer);
    // The bindless table as views and samplers (index = bindless index) in m_rtTextureViews/Samplers.
    void collectRTTextures();
    void destroyEnvSamplingBuffer();
    [[nodiscard]] bool envMapLoaded() const noexcept {
        return m_envMapImageView != VK_NULL_HANDLE && m_envMapTexIdx != 0xFFFFFFFFu;
    }
    std::vector<VkImageView> m_rtTextureViews;
    std::vector<VkSampler> m_rtTextureSamplers;
    VkBuffer m_envCdfBuffer{VK_NULL_HANDLE};   // the path tracer's 2D CDF (render/rt/env_cdf.hpp)
    VkDeviceMemory m_envCdfMemory{VK_NULL_HANDLE};
    std::string m_envMapPath;
    std::string m_envMapLoadedKey;      // the environment uploaded (environmentKey()); empty: none
    std::string m_envMapAttemptedKey;   // the key render() last handed to uploadEnvironmentMap
    bool m_envSky{true};                // no HDRI: the physical sky
    std::unique_ptr<render::Atmosphere> m_atmosphere;   // its tables, built once
    bool m_envMapChecked{false};        // render() ran uploadEnvironmentMap at least once
    // The environment lights without its sun while the scene has a directional light, which
    // stands for it (render/rt/env_sun.hpp): what render() asked for, and what is uploaded.
    bool m_envMapSunless{false};
    VkImage m_envMapImage{VK_NULL_HANDLE};
    VkDeviceMemory m_envMapMemory{VK_NULL_HANDLE};
    VkImageView m_envMapImageView{VK_NULL_HANDLE};  // owned by the bindless table once registered
    VkSampler m_envSampler{VK_NULL_HANDLE};         // the old RT texture sampler (bilinear, base level)
    // The deferred frame's IBL: the environment GGX-prefiltered by roughness and its
    // irradiance, rebuilt with each env upload (render/deferred/env_prefilter.hpp).
    std::unique_ptr<render::EnvPrefilter> m_envPrefilter;
    void destroyEnvMapImage();
    uint32_t m_envMapTexIdx{0xFFFFFFFFu};           // bindless index in light header
    uint32_t m_envMapWidth{0};
    uint32_t m_envMapHeight{0};
    float m_envIntensityScale{1.0f}; // light header offset 8
    uint32_t m_renderSeed{0};        // setRenderSeed; applied to renderers as they are created
    RenderQuality m_renderQuality{RenderQuality::Realtime};
    bool m_rtDirectWanted{true};

    // Light-buffer shadow flag: marks the first directional/spot light as the
    // shadow caster that the deferred CSM pass renders for.
    bool m_shadowsEnabled{true};

    // Presentation (windowed mode only).
    [[nodiscard]] bool createPresentation();
    [[nodiscard]] bool recreatePresentation();
    // Returns whether `staging` received the frame (a requested readback).
    bool blitDeferredOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkBuffer staging);
    // The path tracer's RGBA8 output (in TRANSFER_SRC_OPTIMAL) into the swapchain image, and
    // into `staging` too when one is given; returns whether it was. A headless frame's copy.
    bool blitRTOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkImage src, VkBuffer staging);
    void copyRTOutputToStaging(VkCommandBuffer cmd, VkImage src, VkBuffer staging);
    std::optional<PresentTarget> m_presentTarget;
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> m_swapchain;
    // VK_KHR_present_id + VK_KHR_present_wait were enabled (a window on a device with both).
    bool m_presentWaitEnabled{false};
    std::chrono::steady_clock::time_point m_lastSubmitAt{};
    PFN_vkWaitForPresentKHR m_waitForPresent{nullptr};
    PresentWaiter m_presentWaiter;   // stopped before the swapchain goes
    // Presents the window's swapchain image, tagging it with the frame's number when presents
    // are waited on, and hands the frame to the waiter. Returns the present's result.
    VkResult presentFrame(uint32_t imageIndex);
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_imageAcquired{};
    bool m_swapchainDirty{false};
    // Readback on request (requestReadback / readback).
    bool m_readbackRequested{false};
    bool m_readbackReady{false};     // m_readbackSlot's staging buffer holds a requested frame
    uint32_t m_readbackSlot{0};
    uint64_t m_requestedReadbacks{0};
    // A preview request (requestPreviewReadback) and, for the ready readback, its preview's
    // size (0 x 0: the staging buffer holds the full frame).
    std::optional<PreviewRequest> m_previewRequest;
    uint32_t m_previewWidth{0};
    uint32_t m_previewHeight{0};
    std::unique_ptr<CapturePreviewPass> m_previewPass;   // null when its pipeline failed: previews on the CPU
    // Records capture.preview for the frame on m_currentFrame; false when the frame has no RGBA8 image.
    bool recordPreview(VkCommandBuffer cmd, const PreviewRequest& request);
    // Marks the frame being recorded on m_currentFrame as the requested one when it wrote
    // its staging buffer (`copied`), and clears the request.
    void finishReadbackRequest(bool copied) noexcept;

    // Validation messenger state (see validationEnabled()).
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    ValidationLog m_validationMessages;
    bool m_validationEnabled{false};

    // Denoise state
    DenoiseMode                  m_denoiseMode{DenoiseMode::None};
    bool                         m_denoiseModeOverridden{false}; // true = setDenoiseMode() called; blocks applyRTRenderSettings from resetting it
    std::optional<uint32_t>      m_maxBouncesOverride;   // setMaxBouncesOverride
    // The requested frame, denoised on the CPU (OIDN) or read from NRD's tonemapped
    // output: what readback() returns for those modes, until the next render().
    std::vector<uint8_t>         m_denoisedPixelBuffer;
    bool                         m_denoiseCacheValid{false};
    // readback()'s CPU-denoiser path: fills m_denoisedPixelBuffer from the requested
    // frame's images (the device must be idle on that frame). False when it cannot.
    bool denoiseRequestedFrame();

    // Camera
    std::unique_ptr<Camera> m_camera;

    // Queue family index
    uint32_t m_graphicsQueueFamily{0};
    std::optional<float> m_ambientOverride;

    // Sub-plan 4.C T3b: stash the instance + device extension name lists we
    // enabled at vkCreateInstance / vkCreateDevice time so NRD's NRI device
    // wrapper can re-validate them (NRI doesn't have a way to query them
    // back from a raw VkDevice — it expects the app to provide the list).
    // Storage is owned by renderer.cpp's createInstance() / createLogicalDevice().
    std::vector<const char*> m_enabledInstanceExtensions;
    std::vector<const char*> m_enabledDeviceExtensions;
    // VK_KHR_ray_query + its rayQuery feature were enabled at vkCreateDevice. Only
    // then may a barrier name a non-RT shader stage with ACCELERATION_STRUCTURE_READ.
    bool m_rayQueryEnabled{false};
    // VK_EXT_mesh_shader (task and mesh shaders) was enabled: the G-buffer's meshlet path
    // draws through it, else through its compute + vertex-pulling fallback.
    bool m_meshShaderEnabled{false};

    // Shader base path
    std::string m_shaderBasePath;

    // Initialized flag
    bool m_initialized{false};

    RenderMode m_renderMode{RenderMode::Deferred};

    // Deferred renderer (AAA quality)
    std::unique_ptr<DeferredRenderer> m_deferredRenderer;

    // Bindless texture manager
    std::unique_ptr<BindlessTextureManager> m_textureManager;

    // Persistent GPU render world; recordUpdates runs at the start of every frame body.
    std::unique_ptr<render::RenderScene> m_renderScene;
    // The emissive triangle lights (frontier F3): one table for the path tracer and the deferred
    // frame's direct light, recorded by whichever draws a frame. Null without ray queries.
    std::unique_ptr<render::EmissiveLights> m_emissiveLights;

    // Deferred rendering methods
    [[nodiscard]] bool initializeDeferredRenderer();
    void applyRTRenderSettings();
    void renderDeferred();
    // A requested headless frame: its final RGBA8 image into the slot's staging buffer.
    // Returns whether the staging buffer received it.
    bool copyDeferredOutputToStaging(VkCommandBuffer cmd);

    RTRenderSettings m_rtSettings{kOfflineRTSettings};
};

} // namespace ohao
