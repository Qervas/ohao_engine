#pragma once

#include <optional>
#include <array>
#include "gpu/vulkan/swapchain.hpp"
#include "gpu/vulkan/present_target.hpp"
#include <atomic>
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
#include "render/rt/rt_render_pipeline.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/rt_profile_renderer.hpp"
#include "render/scene/render_scene_types.hpp"
#include "render/scene/model_upload.hpp"

namespace ohao {

namespace render { class RenderScene; }

class Camera;
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

// Light uniform buffer
struct LightUniformBuffer {
    LightData lights[MAX_LIGHTS];
    alignas(4) int numLights;
    alignas(4) float ambientIntensity;
    alignas(4) float shadowBias;
    alignas(4) float shadowStrength;
};

/**
 * VulkanRenderer - Renders OHAO scenes headless into a pixel buffer, or to a window
 * when initialized with a PresentTarget (see ohao/runtime/LiveHost).
 *
 * The scene is renderScene(): an ecs::World with installRenderExtract feeds it
 * (examples/cornell_box.cpp). Headless usage:
 *   VulkanRenderer renderer(800, 600);
 *   renderer.initialize();
 *   ecs::installRenderExtract(world, renderer.renderScene());
 *   world.sync(); renderer.render();
 *   const uint8_t* pixels = renderer.getPixels(); // RGBA format
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
    // The window's drawable size changed; the swapchain is rebuilt next frame.
    void setPresentExtent(VkExtent2D extent);
    // Headless readback copies recorded since initialize (windowed mode: always 0).
    [[nodiscard]] uint64_t readbackCopyCount() const noexcept { return m_readbackCopies; }
    // Readback on request: the next frame render() records also leaves its final RGBA8
    // image in that frame slot's persistently mapped staging buffer (a window frame copies
    // only when asked; a headless frame always does). readback() then waits for that
    // frame's fence (never the queue) and returns its pixels, width * height * 4 bytes,
    // valid until the next render() or resize(); empty when the frame was not rendered
    // (minimized, an out-of-date swapchain) or produced no RGBA8 image.
    void requestReadback() noexcept { m_readbackRequested = true; }
    [[nodiscard]] std::span<const uint8_t> readback();
    // Window frames that copied for a request (the unrequested count stays readbackCopyCount).
    [[nodiscard]] uint64_t requestedReadbackCount() const noexcept { return m_requestedReadbacks; }
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

    // Raster light UBO of a frame slot, and its host mapping (tests).
    [[nodiscard]] VkBuffer lightBuffer(uint32_t slot) const noexcept {
        return slot < MAX_FRAMES_IN_FLIGHT ? m_lightBuffers[slot] : VK_NULL_HANDLE;
    }
    [[nodiscard]] const LightUniformBuffer* lightUniforms(uint32_t slot) const noexcept {
        return slot < MAX_FRAMES_IN_FLIGHT ? static_cast<const LightUniformBuffer*>(m_lightBufferMapped[slot])
                                           : nullptr;
    }

    // Vulkan handles for out-of-tree consumers (render_bench's GPU calibration kernel). Work
    // submitted on the queue must finish before the next render() (the renderer does not
    // synchronize with it).
    [[nodiscard]] VkDevice getDevice() const noexcept { return m_device; }
    [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const noexcept { return m_physicalDevice; }
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
    void setEnvironmentMap(std::string_view path) { m_envMapPath = std::string(path); }
    /// Scale HDRI contribution (inverse / relight). Default 1.0. Written into the
    /// RT light header (offset 8) every RT frame — no env re-load.
    void setEnvIntensityScale(float s) noexcept {
        m_envIntensityScale = std::max(0.0f, s);
    }
    [[nodiscard]] float getEnvIntensityScale() const noexcept { return m_envIntensityScale; }
    void notifyCameraChanged();
    void resetAccumulation();
    /// The render's one seed: the path tracers' base sample index after an accumulation
    /// reset, and the deferred path's SSAO noise. Kept, so renderers created later get it.
    void setRenderSeed(uint32_t seed);
    [[nodiscard]] uint32_t getRenderSeed() const noexcept { return m_renderSeed; }
    [[nodiscard]] uint32_t getPathTracerFrameIndex() const;

    // Read back HDR buffers for OIDN denoising
    [[nodiscard]] bool readbackHDRBuffers(std::vector<float>& beauty, std::vector<float>& albedo,
                            std::vector<float>& normal, uint32_t& w, uint32_t& h);

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

    // Realtime/DLSS: genuine per-frame sample count. The realtime raygen traces
    // N decorrelated paths per pixel in ONE render() dispatch and averages them,
    // so DLSS/SVGF get a clean N-spp image before denoising. Set by a realtime
    // host. Clamped to [1, 64]. Survives the per-frame RT
    // settings reset (re-injected in applyRTRenderSettings).
    void        setRealtimeSamplesPerFrame(uint32_t n);
    [[nodiscard]] uint32_t    getRealtimeSamplesPerFrame() const { return m_realtimeSamplesPerFrame; }

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
    // Sub-plan 4.E: NRD tonemapped output (RGBA8 UNORM)
    [[nodiscard]] VkImage     getNrdTonemappedAOVImage() const;

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
    // Sub-plan 4.E T1: NRD tonemapped output readback (RGBA8 UNORM, 4 bytes/pixel)
    [[nodiscard]] bool readbackNrdTonemapped(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 diffuse albedo AOV (4 bytes per pixel).
    [[nodiscard]] bool readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 specular color AOV (4 bytes per pixel).
    [[nodiscard]] bool readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Returns pointer to RGBA8 tonemapped pixels. If denoiseMode != None,
    // the buffer is lazily denoised on the first call after render();
    // subsequent calls return the cached result until the next render().
    [[nodiscard]] const uint8_t* getPixels() const;
    /// Non-owning view of the same buffer as getPixels() (RGBA8).
    [[nodiscard]] std::span<const uint8_t> getPixelSpan() const {
        const uint8_t* p = getPixels();
        if (!p) return {};
        return std::span<const uint8_t>{p, m_pixelBuffer.size()};
    }
    [[nodiscard]] uint32_t getWidth() const noexcept { return m_width; }
    [[nodiscard]] uint32_t getHeight() const noexcept { return m_height; }
    [[nodiscard]] size_t getPixelBufferSize() const noexcept { return m_pixelBuffer.size(); }
    [[nodiscard]] VkExtent2D getExtent() const noexcept {
        return VkExtent2D{.width = m_width, .height = m_height};
    }


    // Read GPU terrain heightmap back to CPU (blocking — one-time call at setup).
    // Returns false if no procedural terrain exists. outRes = square resolution.
    [[nodiscard]] bool readTerrainHeights(std::vector<float>& outData, uint32_t& outRes);

private:
    // Vulkan setup (no window required)
    [[nodiscard]] bool createInstance();
    [[nodiscard]] bool pickPhysicalDevice();
    [[nodiscard]] bool createLogicalDevice();
    [[nodiscard]] bool createCommandPool();

    glm::mat4 calculateLightSpaceMatrix(const LightData& light);

    // Phase 1: Rendering pipeline setup
    [[nodiscard]] bool createLightBuffer();   // one light UBO per frame slot
    void destroyLightBuffers();
    [[nodiscard]] bool initializeFrameResources();  // Multi-frame resource initialization
    [[nodiscard]] VkShaderModule loadShaderModule(std::string_view filepath);
    // Writes `slot`'s light UBO from m_renderScene's raster lights (+ default sun) and
    // returns its mapping. Call only after that slot's fence wait.
    LightUniformBuffer* updateLightBuffer(uint32_t slot);

    void prepareRTSceneForFrame(const IRTRenderPipeline& pipeline);


    // Rendering internals
    void renderRTPipeline(const IRTRenderPipeline& pipeline);
public:
private:
    const IRTRenderPipeline* getRTPipeline(RenderMode mode) const;
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

    // Light uniform buffers, one per frame slot (host-visible, persistently mapped)
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> m_lightBuffers{};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT> m_lightBufferMemory{};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> m_lightBufferMapped{};

    // Path tracer profiles (the acceleration structures belong to m_renderScene)
    RTRealtimePipeline m_rtRealtimePipeline;
    RTOfflinePipeline m_rtOfflinePipeline;
    std::unique_ptr<RTRealtimeRenderer> m_rtRealtimeRenderer;
    std::unique_ptr<RTOfflineRenderer> m_rtOfflineRenderer;
    // Environment map (env_upload.cpp): HDR image in the bindless table, its CDFs,
    // and the RT bindless view/sampler arrays rebuilt each RT frame.
    void uploadEnvironmentMap();          // once per path (render()); stamps env index + scale into the RenderScene
    void bindRTTextures(IRTRendererProfile& renderer);
    void destroyEnvCDFBuffers();
    [[nodiscard]] bool envMapLoaded() const noexcept {
        return m_envMapImageView != VK_NULL_HANDLE && m_envMapTexIdx != 0xFFFFFFFFu;
    }
    std::vector<VkImageView> m_rtTextureViews;
    std::vector<VkSampler> m_rtTextureSamplers;
    VkBuffer m_envMarginalCDFBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_envMarginalCDFMemory{VK_NULL_HANDLE};
    VkBuffer m_envConditionalCDFBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_envConditionalCDFMemory{VK_NULL_HANDLE};
    std::string m_envMapPath;
    std::string m_envMapLoadedPath;  // last successfully uploaded env (skip re-load)
    std::string m_envMapAttemptedPath;  // path render() last handed to uploadEnvironmentMap
    bool m_envMapChecked{false};        // render() ran uploadEnvironmentMap at least once
    VkImage m_envMapImage{VK_NULL_HANDLE};
    VkDeviceMemory m_envMapMemory{VK_NULL_HANDLE};
    VkImageView m_envMapImageView{VK_NULL_HANDLE};  // owned by the bindless table once registered
    VkSampler m_envSampler{VK_NULL_HANDLE};         // the old RT texture sampler (bilinear, base level)
    void destroyEnvMapImage();
    uint32_t m_envMapTexIdx{0xFFFFFFFFu};           // bindless index in light header
    uint32_t m_envMapWidth{0};
    uint32_t m_envMapHeight{0};
    float m_envMapIntegral{0.0f};
    float m_envIntensityScale{1.0f}; // light header offset 8
    uint32_t m_renderSeed{0};        // setRenderSeed; applied to renderers as they are created

    // Light-buffer shadow flag: marks the first directional/spot light as the
    // shadow caster that the deferred CSM pass renders for.
    bool m_shadowsEnabled{true};

    // Presentation (windowed mode only).
    [[nodiscard]] bool createPresentation();
    [[nodiscard]] bool recreatePresentation();
    // Returns whether `staging` received the frame (a requested readback).
    bool blitDeferredOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkBuffer staging);
    std::optional<PresentTarget> m_presentTarget;
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> m_swapchain;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_imageAcquired{};
    bool m_swapchainDirty{false};
    uint64_t m_readbackCopies{0};
    // Readback on request (requestReadback / readback).
    bool m_readbackRequested{false};
    bool m_readbackReady{false};     // m_readbackSlot's staging buffer holds a requested frame
    uint32_t m_readbackSlot{0};
    uint64_t m_requestedReadbacks{0};
    // Marks the frame being recorded on m_currentFrame as the requested one when it wrote
    // its staging buffer (`copied`), and clears the request.
    void finishReadbackRequest(bool copied) noexcept;

    // Validation messenger state (see validationEnabled()).
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    ValidationLog m_validationMessages;
    bool m_validationEnabled{false};

    // Pixel buffer (CPU accessible)
    std::vector<uint8_t> m_pixelBuffer;

    // Denoise state
    DenoiseMode                  m_denoiseMode{DenoiseMode::None};
    bool                         m_denoiseModeOverridden{false}; // true = setDenoiseMode() called; blocks applyRTRenderSettings from resetting it
    // Realtime per-frame sample count (setRealtimeSamplesPerFrame). Re-injected into
    // m_rtSettings each frame in applyRTRenderSettings so the per-frame reset in
    // prepareRTSceneForFrame doesn't clobber it (same pattern as aniso/SSS).
    uint32_t                     m_realtimeSamplesPerFrame{1};
    mutable std::vector<uint8_t> m_denoisedPixelBuffer;
    mutable bool                 m_denoiseCacheValid{false};

    // Camera
    std::unique_ptr<Camera> m_camera;

    // Queue family index
    uint32_t m_graphicsQueueFamily{0};

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

    // Deferred rendering methods
    [[nodiscard]] bool initializeDeferredRenderer();
    void applyRTRenderSettings();
    void renderDeferred();
    // Returns whether the frame's staging buffer received the final image.
    bool copyDeferredOutputToPixelBuffer(VkCommandBuffer cmd);

    RTRenderSettings m_rtSettings{kOfflineRTSettings};
};

} // namespace ohao
