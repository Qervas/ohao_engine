#pragma once

#include "render/display/display.hpp"
#include "render/rt/emissive_lights.hpp"

// Path Tracer — full Vulkan RT pipeline that replaces rasterization entirely.
//
// Traces camera rays through the scene, bouncing off surfaces to compute
// global illumination, soft shadows, and color bleeding in a single unified pass.
// Accumulates samples across frames for progressive refinement (reset on camera move).
//
// Two output images:
//   m_accumBuffer  — RGBA32F HDR accumulation across frames (the latest of a ping-pong pair)
//   m_outputImage  — RGBA8 tonemapped final output (ready for display)
//
// Usage:
//   PathTracer pt;
//   pt.init(device, physicalDevice, 1920, 1080);
//   // per frame, after RenderScene::recordUpdates:
//   pt.setSceneBuffers(renderScene);
//   pt.setBindlessTextures(views, samplers);
//   pt.render(cmd, frameSlot, view, proj);   // lights: the RenderScene light table (binding 11)
//   // on camera move:
//   pt.resetAccumulation();
//   // on window resize:
//   pt.resize(newWidth, newHeight);

#include "render/material/openpbr_tables_buffer.hpp"
#include "render/rt/path_guiding.hpp"
#include "render/rt/caustic_pass.hpp"
#include "render/rt/manifold_casters.hpp"
#include "render/rt/radiance_cache.hpp"
#include "rt_acceleration_structure.hpp"
#include "render/rt/rt_settings.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "core/concepts.hpp"
#include "gpu/layout_meta.hpp"
#include "render/frame/frame_resources.hpp"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <vector>
#include <cstdint>
#include <memory>
#include <span>

// Forward declaration kept unconditional (even when OHAO_NRD=OFF) so the
// PathTracer class layout is identical across translation units. The
// OHAO_NRD_ENABLED macro is PRIVATE to ohao_renderer, so conditional
// members in this header would violate ODR (ohao_gpu_vulkan sees a
// different layout than ohao_renderer).
namespace ohao { class NrdDenoiser; }
namespace ohao { class NrdCompositor; }      // NEW 4.D
namespace ohao { class DlssRR; }             // DLSS Ray Reconstruction (DenoiseMode::DLSSRR, Phase 5)
namespace ohao::render { class RenderScene; class GpuPassTimer; }

namespace ohao {

// RTRenderProfile / RTRenderSettings / kRealtimeRTSettings / kOfflineRTSettings
// live in render/rt/rt_settings.hpp; render/rt/rt_meta.hpp holds the denoise policy.

// A profile's shaders: SPIR-V files and their entry points (a Slang file with several stages
// names each). The default, rt/path_tracer.slang, is both profiles'.
struct PathTracerShaderSet {
    const char* raygenSpv{"bin/shaders/rt_path_tracer.slang.raygen.spv"};
    const char* missSpv{"bin/shaders/rt_path_tracer.slang.miss.spv"};
    const char* closestHitSpv{"bin/shaders/rt_path_tracer.slang.closestHit.spv"};
    const char* anyHitSpv{"bin/shaders/rt_path_tracer.slang.anyHit.spv"};
    const char* raygenEntry{"raygen"};
    const char* missEntry{"miss"};
    const char* closestHitEntry{"closestHit"};
    const char* anyHitEntry{"anyHit"};
    const char* cacheUpdateSpv{"bin/shaders/rt_path_tracer.slang.cacheUpdate.spv"};   // the radiance cache's update
    const char* cacheUpdateEntry{"cacheUpdate"};
    // Caustics (sub-project 24): raygen with the split compiled in, and the light pass.
    const char* raygenCausticsSpv{"bin/shaders/rt_path_tracer.slang.raygenCaustics.spv"};
    const char* raygenCausticsEntry{"raygenCaustics"};
    const char* causticTraceSpv{"bin/shaders/rt_path_tracer.slang.causticTrace.spv"};
    const char* causticTraceEntry{"causticTrace"};
    // Caustics step 2: the manifold pass, a program of its own.
    const char* manifoldSpv{"bin/shaders/rt_path_tracer.slang.manifoldConnect.spv"};
    const char* manifoldEntry{"manifoldConnect"};
    // Path guiding (sub-project 23): raygen and raygenCaustics with guiding compiled in.
    const char* raygenGuidedSpv{"bin/shaders/rt_path_tracer.slang.raygenGuided.spv"};
    const char* raygenGuidedEntry{"raygenGuided"};
    const char* raygenCausticsGuidedSpv{"bin/shaders/rt_path_tracer.slang.raygenCausticsGuided.spv"};
    const char* raygenCausticsGuidedEntry{"raygenCausticsGuided"};
};

class PathTracer {
public:
    // Both ctor and dtor are out-of-line so the unique_ptr<NrdDenoiser>
    // member does not instantiate std::default_delete here (requires
    // complete type of NrdDenoiser, which is only available in TUs that
    // include nrd_denoise.hpp).
    PathTracer();
    ~PathTracer();

    // Sub-plan 4.C T3b: NRD integration needs VkInstance + graphics queue
    // family index + the exact instance/device-extension lists used at
    // vkCreateDevice / vkCreateInstance time. Defaults keep backward
    // compatibility for callers that don't use NRD.
    [[nodiscard]] bool init(VkDevice device, VkPhysicalDevice physicalDevice,
                            uint32_t width, uint32_t height,
                            VkInstance instance = VK_NULL_HANDLE,
                            uint32_t graphicsQueueFamilyIndex = 0,
                            const std::vector<const char*>& instanceExtensions = {},
                            const std::vector<const char*>& deviceExtensions = {});
    void setShaderSet(const PathTracerShaderSet& shaderSet) { m_shaderSet = shaderSet; }

    // Traces the scene last given to setSceneBuffers, lit by its light table; does
    // nothing without a TLAS. `frameSlot` is the frame-in-flight slot whose fence was
    // just waited: its own descriptor set is rewritten and bound (earlier frames may
    // still read theirs).
    void render(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj);
    // Times render()'s stages (pt.cache.update, pt.cache.resolve, pt.trace, pt.denoise); the
    // caller ends the last one. Null: untimed.
    void setPassTimer(render::GpuPassTimer* timer) noexcept { m_passTimer = timer; }
    // The display transform (shaders/display/display.slang) of the output, NRD's and DLSS's frames:
    // a linear exposure (render::displayExposure) and the curve.
    void setDisplay(float exposure, render::DisplayCurve curve) noexcept {
        m_displayExposure = exposure;
        m_displayCurve = curve;
    }

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkImage getOutputImage() const { return m_outputImage; }
    [[nodiscard]] VkImage getAccumImage() const { return m_accumBuffer; }
    [[nodiscard]] VkImage getAlbedoAOV() const { return m_albedoAOV; }
    [[nodiscard]] VkImage getNormalAOV() const { return m_normalAOV; }
    [[nodiscard]] VkImageView getOutputView() const { return m_outputView; }
    [[nodiscard]] VkImageView getMotionVectorAOV() const { return m_motionVectorView; }
    [[nodiscard]] VkImage getMotionVectorImage() const { return m_motionVectorImage; }
    [[nodiscard]] VkImageView getDepthAOV()         const { return m_depthAOVView; }
    [[nodiscard]] VkImage     getDepthAOVImage()    const { return m_depthAOVImage; }
    [[nodiscard]] VkImageView getRoughnessAOV()     const { return m_roughnessAOVView; }
    [[nodiscard]] VkImage     getRoughnessAOVImage() const { return m_roughnessAOVImage; }
    [[nodiscard]] VkImageView getDiffuseRadianceAOV()      const { return m_diffuseRadianceView; }
    [[nodiscard]] VkImage     getDiffuseRadianceAOVImage() const { return m_diffuseRadianceImage; }
    [[nodiscard]] VkImageView getSpecularRadianceAOV()      const { return m_specularRadianceView; }
    [[nodiscard]] VkImage     getSpecularRadianceAOVImage() const { return m_specularRadianceImage; }
    [[nodiscard]] VkImageView getDiffAlbedoAOV()      const { return m_diffAlbedoView; }
    [[nodiscard]] VkImage     getDiffAlbedoAOVImage() const { return m_diffAlbedoImage; }
    [[nodiscard]] VkImageView getSpecColorAOV()       const { return m_specColorView; }
    [[nodiscard]] VkImage     getSpecColorAOVImage()  const { return m_specColorImage; }
    [[nodiscard]] VkImageView getNormalRoughnessAOV()      const { return m_normalRoughnessView; }
    [[nodiscard]] VkImage     getNormalRoughnessAOVImage() const { return m_normalRoughnessImage; }
    // DLSS-RR specular hit-distance guide (R32F; written by the tracer from frontier F1 T4).
    [[nodiscard]] VkImageView getSpecHitDistAOV()      const { return m_specHitDistView; }
    [[nodiscard]] VkImage     getSpecHitDistAOVImage() const { return m_specHitDistImage; }
    [[nodiscard]] VkImageView getOutDiffRadianceAOV()      const { return m_outDiffRadianceView; }
    [[nodiscard]] VkImage     getOutDiffRadianceAOVImage() const { return m_outDiffRadianceImage; }
    [[nodiscard]] VkImageView getOutSpecRadianceAOV()      const { return m_outSpecRadianceView; }
    [[nodiscard]] VkImage     getOutSpecRadianceAOVImage() const { return m_outSpecRadianceImage; }
    [[nodiscard]] VkImageView getNrdComposedAOV()      const { return m_nrdComposedView; }
    [[nodiscard]] VkImage     getNrdComposedAOVImage() const { return m_nrdComposedImage; }
    // Sub-plan 4.E T1: tonemapped NRD output (RGBA8 UNORM, binding 30).

    // Scene inputs from the RenderScene: bindings 4 (vertex arena), 5 (index arena),
    // 8 (mesh table), 9 (triangle material slots), 10 (material table), 11 (RT
    // lights), 36 (instances), 37 (material refs) and 0 (TLAS). Call every frame
    // after RenderScene::recordUpdates: growth and TLAS rebuilds replace handles.
    void setSceneBuffers(const render::RenderScene& scene);
    // Bindless textures (binding 12): views[i] is bindless index i, one sampler per
    // view. Every one of the bindingCapacity() entries is written each frame; entries
    // past views.size() repeat views[0]. The shaders treat an index >= 4096 as no texture.
    void setBindlessTextures(std::span<const VkImageView> views,
                             std::span<const VkSampler> samplers) {
        m_bindlessImageViews.assign(views.begin(), views.end());
        m_bindlessSamplers.assign(samplers.begin(), samplers.end());
        m_bindlessTextureCount = static_cast<uint32_t>(std::min(views.size(), samplers.size()));
    }
    // Size of binding 12: the texture table's capacity. Call before init(); init
    // clamps it to the device's update-after-bind sampled-image limits (logged).
    void setBindlessCapacity(uint32_t capacity) noexcept { m_bindlessCapacity = std::max(1u, capacity); }
    // The emissive triangle lights (the renderer's table, which this tracer records on its frames) and
    // the bindless texture table's set they read. Without them it traces nothing.
    void setEmissiveLights(render::EmissiveLights* lights, VkDescriptorSet textures) noexcept {
        m_emissive = lights;
        m_textureSet = textures;
    }
    [[nodiscard]] uint32_t bindlessCapacity() const noexcept { return m_bindlessCapacity; }
    void setRadianceCacheCapacity(uint32_t entries) noexcept { m_radianceCacheCapacity = entries; }
    static constexpr uint32_t kDefaultBindlessCapacity = 4096;

    // The environment's 2D CDF (binding 17, render/rt/env_cdf.hpp) and the map's size;
    // envWidth 0: no environment to sample (the renderer still binds a one-float buffer).
    void setEnvSampling(VkBuffer cdf, uint32_t envWidth, uint32_t envHeight) {
        m_envCdfBuffer = cdf;
        m_envWidth = envWidth;
        m_envHeight = envHeight;
    }

    // Sub-plan 4.F T1: expose HDR env map view + sampler to the NRD tonemap
    // compositor so it can composite the lit sky for miss-ray pixels. The
    // renderer owns the view/sampler lifetime; PathTracer only holds borrowed
    // handles. Pass {NULL, NULL} to indicate "no env loaded" — tonemap will
    // set envIntensity=0 and sky pixels stay black (pre-4.F behavior).
    void setEnvMapResource(VkImageView view, VkSampler sampler) {
        m_envMapView    = view;
        m_envMapSampler = sampler;
    }
    [[nodiscard]] VkImageView getEnvMapView()    const { return m_envMapView; }
    [[nodiscard]] VkSampler   getEnvMapSampler() const { return m_envMapSampler; }

    // The view moved: the trace's running mean restarts at the next frame; a temporal denoiser's
    // history does not (it reprojects through the motion vectors).
    void notifyViewChanged() { m_viewChangedThisFrame = true; }
    void resetAccumulation();
    /// Base sample index after reset: the tracer's random streams are seeded by m_sampleIndex.
    // Takes effect at the next resetAccumulation, or now when nothing has accumulated since
    // the last one (a fresh tracer starts its sample index at the seed).
    void setRenderSeed(uint32_t seed) noexcept {
        if (m_sampleIndex == m_renderSeed) m_sampleIndex = seed;
        m_renderSeed = seed;
    }
    [[nodiscard]] uint32_t getRenderSeed() const noexcept { return m_renderSeed; }

    void destroy();

    // Config
    void setMaxBounces(uint32_t bounces) noexcept { m_maxBounces = bounces; }

    /// Apply settings + denoise policy. Out-of-line: may reallocate images when
    /// DLSS render scale changes.
    void setRenderSettings(const RTRenderSettings& settings);

    [[nodiscard]] const RTRenderSettings& getRenderSettings() const noexcept { return m_renderSettings; }
    [[nodiscard]] DenoiseMode getDenoiseMode() const noexcept { return m_renderSettings.denoiseMode; }
    [[nodiscard]] uint32_t getFrameIndex() const noexcept { return m_historyFrameCount; }

private:
    [[nodiscard]] bool createImages();
    [[nodiscard]] bool createDescriptorResources();
    [[nodiscard]] bool createRTPipeline();
    [[nodiscard]] bool createShaderBindingTable();
    // A program: raygen, the miss, the hit group and a second raygen (none when extraSpv is null), on the shared layout.
    [[nodiscard]] VkPipeline buildRtPipeline(const char* rgenSpv, const char* rgenEntry, const char* extraSpv,
                                             const char* extraEntry, const char* cacheName);
    [[nodiscard]] bool buildShaderBindingTable(VkPipeline pipeline, VkBuffer& sbtBuffer, VkDeviceMemory& sbtMemory,
                                               VkStridedDeviceAddressRegionKHR& rgenRegion,
                                               VkStridedDeviceAddressRegionKHR& missRegion,
                                               VkStridedDeviceAddressRegionKHR& hitRegion,
                                               VkStridedDeviceAddressRegionKHR* extraRegion);
    // Caustics' program, made the first time a frame needs it (minutes cold, as the default one).
    [[nodiscard]] bool ensureCausticsPipeline();
    // Caustics step 2's program (manifoldConnect), made the first time a frame runs the manifold pass.
    [[nodiscard]] bool ensureManifoldPipeline();
    // Path guiding's program beside the default one's raygen (caustics false: raygenGuided) or caustics' (true:
    // raygenCausticsGuided with causticTrace), made the first time a frame guides.
    [[nodiscard]] bool ensureGuidedProgram(bool caustics);
public:
    // Caustics' counters once a frame has run the light pass (render.stats `caustics`).
    [[nodiscard]] std::optional<render::CausticsStats> causticsStats() const {
        if (!m_causticsRan) return std::nullopt;
        return render::CausticsStats{m_caustics.casterCount(), m_caustics.clamped()};
    }
private:
    [[nodiscard]] bool loadFunctionPointers();

    void destroyImages();

    // Derive internal RENDER res (m_width/m_height) from OUTPUT res (m_outW/m_outH).
    // DLSSRR may scale down; every other mode renders 1:1.
    void computeRenderResolution();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    // m_width/m_height = internal RENDER resolution (raygen + AOVs).
    // m_outW/m_outH = OUTPUT/display resolution (beauty + DLSS COLOR_OUT).
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_outW = 0;
    uint32_t m_outH = 0;
    float    m_dlssRenderScale = 1.0f;

    // Sub-plan 4.C: replaces 4.B scoped probe.
    // Member is unconditional (not guarded by OHAO_NRD_ENABLED) so the class
    // layout matches across ohao_renderer (macro defined) and ohao_gpu_vulkan
    // (macro NOT defined). Only the method bodies in path_tracer.cpp that
    // actually touch NrdDenoiser remain guarded by OHAO_NRD_ENABLED; when
    // NRD is off, this pointer stays null and unique_ptr's default dtor is a
    // no-op on a null held pointer (no need for complete type).
    std::unique_ptr<NrdDenoiser> m_nrdDenoiser;
    std::unique_ptr<NrdCompositor> m_nrdCompositor;   // NEW 4.D

    // DLSS Ray Reconstruction (DenoiseMode::DLSSRR, Phase 5). Guarded by
    // OHAO_DLSS_ENABLED — which CMake defines on BOTH ohao_renderer and
    // ohao_gpu_vulkan (the latter instantiates PathTracer's layout via
    // rt_profile_renderer.hpp), so the class layout stays ODR-consistent and a
    // -DOHAO_DLSS=OFF build carries none of these members. NGX init + feature
    // creation happen lazily on the first DLSSRR-mode frame (see render()).
#ifdef OHAO_DLSS_ENABLED
    std::unique_ptr<DlssRR> m_dlssRR;
    VkInstance m_dlssInstance     = VK_NULL_HANDLE;  // stashed at init() for lazy NGX init
    bool       m_dlssInitAttempted = false;          // one-shot latch (success or failure)
    // DLSS-RR COLOR_OUT — denoised HDR-linear result DLSS writes (RGBA16F). Then
    // tonemapped into m_outputImage (RGBA8) for the standard beauty readback.
    VkImage        m_dlssColorOutImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_dlssColorOutMemory = VK_NULL_HANDLE;
    VkImageView    m_dlssColorOutView   = VK_NULL_HANDLE;
    bool           m_dlssColorOutFirstFrame = true;  // gates UNDEFINED→GENERAL transition
#endif

    // Config
    uint32_t m_maxBounces = 4;  // 4 bounces: diminishing returns in indoor scenes
    RTRenderSettings m_renderSettings{kOfflineRTSettings};
    PathTracerShaderSet m_shaderSet{};
    // RenderScene inputs (setSceneBuffers); borrowed, never destroyed here.
    VkAccelerationStructureKHR m_tlas = VK_NULL_HANDLE;
    VkBuffer m_vertexArena = VK_NULL_HANDLE;
    VkBuffer m_indexArena = VK_NULL_HANDLE;
    VkBuffer m_meshTable = VK_NULL_HANDLE;
    VkBuffer m_triangleSlots = VK_NULL_HANDLE;
    VkBuffer m_materialTable = VK_NULL_HANDLE;
    VkBuffer m_instances = VK_NULL_HANDLE;
    VkBuffer m_materialRefs = VK_NULL_HANDLE;
    VkBuffer m_lightBuffer = VK_NULL_HANDLE;

    // The environment's 2D CDF (binding 17) and the map's size.
    VkBuffer m_envCdfBuffer = VK_NULL_HANDLE;
    uint32_t m_envWidth = 0;
    uint32_t m_envHeight = 0;
    // OpenPBR's energy tables, binding 38: written once at init.
    render::OpenPBRTablesBuffer m_openpbrTables;
    // The radiance cache (bindings 39-41): full size for the realtime profile, which ends its paths
    // in it, one cell otherwise (the shaders bind it either way). setRadianceCacheCapacity before init.
    render::RadianceCache m_radianceCache;
    // Path guiding (bindings 46, 49, 50; render/rt/path_guiding.hpp): placeholders until a frame
    // first guides, then the grid and a pixel's proposals.
    render::PathGuiding m_guiding;
    // Caustics' light pass (bindings 51-53; render/rt/caustic_pass.hpp).
    render::CausticPass m_caustics;
    bool m_causticsRan = false;
    // Caustics step 2's casters and requests (bindings 54-57; render/rt/manifold_casters.hpp).
    render::ManifoldCasters m_manifold;
    bool m_manifoldRan = false;
    const render::RenderScene* m_scene = nullptr;   // setSceneBuffers': its casters, when a frame has caustics
    // The emissive triangle lights (bindings 42-45), from the scene's emissive instances.
    render::EmissiveLights* m_emissive = nullptr;   // the renderer's (setEmissiveLights)
    VkDescriptorSet m_textureSet = VK_NULL_HANDLE;   // the bindless texture table's set (the collect pass's)
    std::vector<render::RenderScene::EmissiveInstance> m_emissiveInstances;
    uint64_t m_emissiveVersion = 0;
    uint32_t m_instanceSlotEnd = 0;
    float m_analyticLightPower = 0.0f;
    render::GpuPassTimer* m_passTimer{nullptr};   // VulkanRenderer's
    float m_displayExposure{render::displayExposure(render::kDefaultEv100)};
    render::DisplayCurve m_displayCurve{render::DisplayCurve::AgX};
    uint32_t m_radianceCacheCapacity = 1;

    // Sub-plan 4.F T1: HDR env map for NRD tonemap's sky composite. Borrowed —
    // the renderer owns the VkImage/VkImageView lifetime.
    VkImageView m_envMapView    = VK_NULL_HANDLE;
    VkSampler   m_envMapSampler = VK_NULL_HANDLE;

    // Bindless textures — individual sampler2D entries
    std::vector<VkImageView> m_bindlessImageViews;
    std::vector<VkSampler> m_bindlessSamplers;
    uint32_t m_bindlessTextureCount = 0;
    uint32_t m_bindlessCapacity = kDefaultBindlessCapacity;   // binding 12 size (clamped in init)

    uint32_t m_sampleIndex = 0;
    uint32_t m_renderSeed = 0;  // resetAccumulation() restores sample index to this
    uint32_t m_historyFrameCount = 0;   // frames since resetAccumulation: the denoisers' history
    uint32_t m_accumFrames = 0;         // frames in the trace's running mean (restarts on a moved view)
    bool m_viewChangedThisFrame = false;
    bool m_hasPreviousView = false;     // m_prevViewProj is a rendered frame's (motion vectors are valid)

    // Accumulation — RGBA32F HDR, two images ping-ponged per frame: the trace writes
    // this frame's (binding 1) and reads the previous frame's (binding 3, read-only), so
    // no invocation reads a pixel another one is writing (temporal reprojection reads a
    // neighbour). m_accumBuffer / m_accumView name the image the latest render() wrote.
    VkImage m_accumImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory m_accumMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView m_accumViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool m_accumInitialized[2] = {false, false};
    uint32_t m_accumWriteIndex = 0;
    VkImage m_accumBuffer = VK_NULL_HANDLE;
    VkImageView m_accumView = VK_NULL_HANDLE;

    // Output image — RGBA8 tonemapped
    VkImage m_outputImage = VK_NULL_HANDLE;
    VkDeviceMemory m_outputMemory = VK_NULL_HANDLE;
    VkImageView m_outputView = VK_NULL_HANDLE;

    // AOV buffers for denoiser guide (RGBA32F)
    VkImage m_albedoAOV = VK_NULL_HANDLE;
    VkDeviceMemory m_albedoAOVMemory = VK_NULL_HANDLE;
    VkImageView m_albedoAOVView = VK_NULL_HANDLE;
    VkImage m_normalAOV = VK_NULL_HANDLE;
    VkDeviceMemory m_normalAOVMemory = VK_NULL_HANDLE;
    VkImageView m_normalAOVView = VK_NULL_HANDLE;

    // Feature 3.A: camera motion vector AOV (RG16F)
    VkImage        m_motionVectorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_motionVectorMemory = VK_NULL_HANDLE;
    VkImageView    m_motionVectorView = VK_NULL_HANDLE;

    // Feature 3.B: view-space depth AOV (R32F)
    VkImage        m_depthAOVImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthAOVMemory = VK_NULL_HANDLE;
    VkImageView    m_depthAOVView = VK_NULL_HANDLE;

    // Feature 3.B: perceptual roughness AOV (R8 UNORM)
    VkImage        m_roughnessAOVImage = VK_NULL_HANDLE;
    VkDeviceMemory m_roughnessAOVMemory = VK_NULL_HANDLE;
    VkImageView    m_roughnessAOVView = VK_NULL_HANDLE;

    // Feature 3.C (raw since 3.C.6): diffuse radiance (RGBA32F since 3.C.5)
    VkImage        m_diffuseRadianceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_diffuseRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_diffuseRadianceView = VK_NULL_HANDLE;

    // Feature 3.C (raw since 3.C.6): specular radiance (RGBA32F since 3.C.5)
    VkImage        m_specularRadianceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_specularRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_specularRadianceView = VK_NULL_HANDLE;

    // Feature 3.C.6: diffuse albedo AOV for NRD remodulate (RGBA8 UNORM)
    VkImage        m_diffAlbedoImage = VK_NULL_HANDLE;
    VkDeviceMemory m_diffAlbedoMemory = VK_NULL_HANDLE;
    VkImageView    m_diffAlbedoView = VK_NULL_HANDLE;

    // Feature 3.C.6: specular color / F0 AOV for NRD remodulate (RGBA8 UNORM)
    VkImage        m_specColorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_specColorMemory = VK_NULL_HANDLE;
    VkImageView    m_specColorView = VK_NULL_HANDLE;

    // Feature 4.B: NRD REBLUR IN_NORMAL_ROUGHNESS — oct-encoded normal (RG) + roughness (B)
    VkImage        m_normalRoughnessImage = VK_NULL_HANDLE;
    VkDeviceMemory m_normalRoughnessMemory = VK_NULL_HANDLE;
    VkImageView    m_normalRoughnessView = VK_NULL_HANDLE;

    // DLSS-RR: specular hit-distance guide (R32F, world-space ray length; frontier F1 T4).
    VkImage        m_specHitDistImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_specHitDistMemory = VK_NULL_HANDLE;
    VkImageView    m_specHitDistView   = VK_NULL_HANDLE;

    // Feature 4.C: NRD denoised diffuse output (RGBA32F)
    VkImage        m_outDiffRadianceImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_outDiffRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_outDiffRadianceView   = VK_NULL_HANDLE;

    // Feature 4.C: NRD denoised specular output (RGBA32F)
    VkImage        m_outSpecRadianceImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_outSpecRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_outSpecRadianceView   = VK_NULL_HANDLE;

    // Feature 4.D: NRD composed HDR output (RGBA32F) at binding 29.
    // NOT in PathTracer's RT descriptor layout — only in NrdCompositor's
    // compute descriptor set.
    VkImage        m_nrdComposedImage     = VK_NULL_HANDLE;
    VkDeviceMemory m_nrdComposedMemory    = VK_NULL_HANDLE;
    VkImageView    m_nrdComposedView      = VK_NULL_HANDLE;
    bool           m_nrdComposeFirstFrame = true;  // gates UNDEFINED→GENERAL transition on binding 29

    // The camera ray's emission (RGBA16F, binding 13): NRD's channels leave it out; nrd_compose adds it.
    VkImage        m_emissionAOVImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_emissionAOVMemory = VK_NULL_HANDLE;
    VkImageView    m_emissionAOVView   = VK_NULL_HANDLE;
    bool           m_emissionInitialized = false;

    // RT Pipeline
    VkPipeline m_rtPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // One set per frame in flight: render() rewrites only its frame slot's set.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};

    // Shader Binding Table
    VkBuffer m_sbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_sbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};  // empty, not used
    VkStridedDeviceAddressRegionKHR m_cacheRgenRegion{};   // the radiance cache's update raygen

    // Caustics' program (ensureCausticsPipeline): raygenCaustics and the light pass, causticTrace.
    VkPipeline m_causticsPipeline = VK_NULL_HANDLE;
    bool m_causticsPipelineFailed = false;
    VkBuffer m_causticsSbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_causticsSbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_causticsRgenRegion{};
    VkStridedDeviceAddressRegionKHR m_causticsMissRegion{};
    VkStridedDeviceAddressRegionKHR m_causticsHitRegion{};
    VkStridedDeviceAddressRegionKHR m_causticTraceRegion{};
    // Path guiding's programs (ensureGuidedProgram): [0] raygenGuided alone (the radiance cache's update stays the
    // default program's), [1] raygenCausticsGuided and causticTrace. Neither default raygen carries guiding's code.
    struct GuidedProgram {
        VkPipeline pipeline = VK_NULL_HANDLE;
        bool failed = false;
        VkBuffer sbtBuffer = VK_NULL_HANDLE;
        VkDeviceMemory sbtMemory = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR rgen{}, miss{}, hit{}, extra{};
    };
    GuidedProgram m_guided[2];
    // Caustics step 2's program (ensureManifoldPipeline): manifoldConnect.
    VkPipeline m_manifoldPipeline = VK_NULL_HANDLE;
    bool m_manifoldPipelineFailed = false;
    VkBuffer m_manifoldSbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_manifoldSbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_manifoldRgenRegion{};
    VkStridedDeviceAddressRegionKHR m_manifoldMissRegion{};
    VkStridedDeviceAddressRegionKHR m_manifoldHitRegion{};

    // Push constants — 256 bytes (Vulkan max on most RT GPUs)
    // The stages PTPushConstants is pushed to: the range and every vkCmdPushConstants of it name all of them
    // (VUID-vkCmdPushConstants-offset-01796), the compute stage included for the wavefront's kernels.
    static constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                                      VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                                      VK_SHADER_STAGE_COMPUTE_BIT;
    struct PTPushConstants {
        glm::mat4 invView;              // 64 bytes
        glm::mat4 invProj;              // 64 bytes
        glm::mat4 prevViewProj;         // 64 bytes — for temporal reprojection
        glm::uvec4 params;              // 16 bytes  (x=width, y=height, z=sampleIndex, w=maxBounces)
        glm::uvec4 control;             // x=flags, y=frames in the running mean, z=motion vectors valid, w=envCDFWidth
                                        // control.w = envCDFWidth. If 0, shader must skip env importance sampling.
        glm::vec4 tuning;               // x=fireflyClamp, y=envCDFHeight, z=envIntegral, w=unused
        glm::vec4 jitter;               // xy=Halton(2,3) pixel offset (0 without NRD or DLSS RR), zw=0
    };  // total = 256 bytes
    OHAO_ASSERT_GPU_LAYOUT(PTPushConstants, layout::kPTPushConstantsBytes);

    glm::mat4 m_prevViewProj{1.0f};  // stored from last frame

    // Sub-plan 4.E T2: previous-frame view + proj matrices for NRD's temporal
    // reprojection. Captured at end of each render() call; consumed by the
    // NEXT frame's NrdCameraInputs. Distinct from m_prevViewProj (3.A motion
    // vectors use the combined VP matrix; NRD wants V and P separately).
    glm::mat4 m_prevViewMatrix{1.0f};
    glm::mat4 m_prevProjMatrix{1.0f};

    // Sub-plan 4.F T4: TAA-style pixel jitter for NRD temporal sample diversity.
    // Halton(2,3) sequence (period 16) generates ±0.5 sub-pixel offsets per frame
    // when DenoiseMode::NRD is active; fed to NRD's cameraJitter/cameraJitterPrev
    // so NRD properly undoes the sub-pixel shift during temporal reprojection.
    glm::vec2 m_jitterCurrent{0.0f};
    glm::vec2 m_jitterPrev{0.0f};
    uint32_t  m_haltonIndex{0};

    // Function pointers (loaded dynamically for RT extensions)
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddressFn = nullptr;

    // Helpers
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
};

} // namespace ohao
