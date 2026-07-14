#pragma once

// Path Tracer — full Vulkan RT pipeline that replaces rasterization entirely.
//
// Traces camera rays through the scene, bouncing off surfaces to compute
// global illumination, soft shadows, and color bleeding in a single unified pass.
// Accumulates samples across frames for progressive refinement (reset on camera move).
//
// Two output images:
//   m_accumBuffer  — RGBA32F HDR accumulation across frames
//   m_outputImage  — RGBA8 tonemapped final output (ready for display)
//
// Usage:
//   PathTracer pt;
//   pt.init(device, physicalDevice, 1920, 1080);
//   pt.setMaterialAlbedos(albedos);
//   // per frame:
//   pt.render(cmd, accel, view, proj, lightPos, intensity, lightColor, lightRadius);
//   // on camera move:
//   pt.resetAccumulation();
//   // on window resize:
//   pt.resize(newWidth, newHeight);

#include "rt_acceleration_structure.hpp"
#include "render/rt/rt_settings.hpp"
#include "render/rt/rt_meta.hpp"
#include "render/rt/sampler_types.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "core/concepts.hpp"
#include "gpu/layout_meta.hpp"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
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
namespace ohao { class NrdCinematicPost; }   // NEW 4.G (replaces 4.E NrdTonemap)
namespace ohao { class AtrousDenoiser; }     // à-trous beauty denoiser (DenoiseMode::Atrous)
namespace ohao { class DlssRR; }             // DLSS Ray Reconstruction (DenoiseMode::DLSSRR, Phase 5)

namespace ohao {

// RTRenderProfile / RTRenderSettings / kRealtimeRTSettings / kOfflineRTSettings
// live in render/rt/rt_settings.hpp (shared with rt_meta traits).

struct PathTracerShaderSet {
    const char* raygenSpv{"bin/shaders/rt_pt_raygen.rgen.spv"};
    const char* missSpv{"bin/shaders/rt_pt_miss.rmiss.spv"};
    const char* closestHitSpv{"bin/shaders/rt_pt_closesthit.rchit.spv"};
    const char* anyHitSpv{"bin/shaders/rt_pt_anyhit.rahit.spv"};
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

    void render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& lightPos, float lightIntensity,
                const glm::vec3& lightColor, float lightRadius);

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
    [[nodiscard]] VkImageView getOutDiffRadianceAOV()      const { return m_outDiffRadianceView; }
    [[nodiscard]] VkImage     getOutDiffRadianceAOVImage() const { return m_outDiffRadianceImage; }
    [[nodiscard]] VkImageView getOutSpecRadianceAOV()      const { return m_outSpecRadianceView; }
    [[nodiscard]] VkImage     getOutSpecRadianceAOVImage() const { return m_outSpecRadianceImage; }
    [[nodiscard]] VkImageView getNrdComposedAOV()      const { return m_nrdComposedView; }
    [[nodiscard]] VkImage     getNrdComposedAOVImage() const { return m_nrdComposedImage; }
    // Sub-plan 4.E T1: tonemapped NRD output (RGBA8 UNORM, binding 30).
    [[nodiscard]] VkImageView getNrdTonemappedAOV()      const { return m_nrdTonemappedView; }
    [[nodiscard]] VkImage     getNrdTonemappedAOVImage() const { return m_nrdTonemappedImage; }

    // Set per-instance material albedo colors (must match TLAS instance order)
    void setMaterialAlbedos(const std::vector<glm::vec3>& albedos);
    void setMaterialData(const std::vector<glm::vec4>& materials);
    void setNormalBuffer(VkBuffer normalBuf, VkBuffer indexBuf, uint32_t vertexCount) {
        m_normalBuffer = normalBuf; m_indexBuffer = indexBuf; m_normalVertexCount = vertexCount;
    }
    void setUVBuffer(VkBuffer uvBuf) { m_uvBuffer = uvBuf; }
    void setMaterialBuffers(VkBuffer matIDBuf, VkBuffer matColorBuf) {
        m_matIDBuffer = matIDBuf; m_matColorBuffer = matColorBuf;
    }
    void setLightBuffer(VkBuffer lightBuf, uint32_t lightCount) {
        m_lightBuffer = lightBuf; m_lightCount = lightCount;
    }
    void setTextureArray(VkImageView view, VkSampler sampler, uint32_t count) {
        m_textureArrayView = view; m_textureSampler = sampler; m_textureArrayCount = count;
    }
    // Bindless textures — pass individual image views + samplers
    void setBindlessTextures(const std::vector<VkImageView>& views,
                             const std::vector<VkSampler>& samplers) {
        m_bindlessImageViews = views;
        m_bindlessSamplers = samplers;
        m_bindlessTextureCount = static_cast<uint32_t>(views.size());
    }
    void setBindlessTextures(std::span<const VkImageView> views,
                             std::span<const VkSampler> samplers) {
        m_bindlessImageViews.assign(views.begin(), views.end());
        m_bindlessSamplers.assign(samplers.begin(), samplers.end());
        m_bindlessTextureCount = static_cast<uint32_t>(views.size());
    }
    [[nodiscard]] std::vector<VkImageView> getBindlessImageViews() const { return m_bindlessImageViews; }
    [[nodiscard]] std::vector<VkSampler> getBindlessSamplers() const { return m_bindlessSamplers; }

    // Shader contract: envWidth == 0 means "no env map loaded" — the shader
    // must skip env importance sampling in this case. The renderer provides
    // 1-float dummy buffers so descriptor writes remain valid.
    void setEnvCDFBuffers(VkBuffer marginal, VkBuffer conditional,
                          uint32_t envWidth, uint32_t envHeight, float integral) {
        m_envMarginalCDFBuffer   = marginal;
        m_envConditionalCDFBuffer = conditional;
        m_envCDFWidth            = envWidth;
        m_envCDFHeight           = envHeight;
        m_envCDFIntegral         = integral;
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
    [[nodiscard]] float       getEnvCDFIntegral() const { return m_envCDFIntegral; }

    // Reset accumulation — call when camera moves so the buffer restarts
    void notifyViewChanged() { m_viewChangedThisFrame = true; }
    void resetAccumulation();

    void destroy();

    // Config
    void setMaxBounces(uint32_t bounces) noexcept { m_maxBounces = bounces; }

    /// Apply settings and enforce denoise-mode policy (AOVs / external denoise).
    void setRenderSettings(const RTRenderSettings& settings) {
        m_renderSettings = applyDenoisePolicy(settings);
        m_renderSettings.samplesPerFrame = clampSamplesPerFrame(m_renderSettings.samplesPerFrame);
        m_maxBounces = m_renderSettings.maxBounces;
    }

    template<RTRenderProfile Profile, DenoiseMode Mode>
    void setFeatureSettings() {
        setRenderSettings(makeFeatureSettings<Profile, Mode>());
    }

    [[nodiscard]] const RTRenderSettings& getRenderSettings() const noexcept { return m_renderSettings; }
    [[nodiscard]] DenoiseMode getDenoiseMode() const noexcept { return m_renderSettings.denoiseMode; }
    [[nodiscard]] uint32_t getFrameIndex() const noexcept { return m_historyFrameCount; }

private:
    [[nodiscard]] bool createImages();
    [[nodiscard]] bool createMaterialBuffer();
    [[nodiscard]] bool createDescriptorResources();
    [[nodiscard]] bool createRTPipeline();
    [[nodiscard]] bool createShaderBindingTable();
    [[nodiscard]] bool loadFunctionPointers();

    void destroyImages();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // Sub-plan 4.C: replaces 4.B scoped probe.
    // Member is unconditional (not guarded by OHAO_NRD_ENABLED) so the class
    // layout matches across ohao_renderer (macro defined) and ohao_gpu_vulkan
    // (macro NOT defined). Only the method bodies in path_tracer.cpp that
    // actually touch NrdDenoiser remain guarded by OHAO_NRD_ENABLED; when
    // NRD is off, this pointer stays null and unique_ptr's default dtor is a
    // no-op on a null held pointer (no need for complete type).
    std::unique_ptr<NrdDenoiser> m_nrdDenoiser;
    std::unique_ptr<NrdCompositor> m_nrdCompositor;   // NEW 4.D
    std::unique_ptr<NrdCinematicPost> m_cinematicPost; // NEW 4.G (replaces 4.E NrdTonemap)
    // À-trous beauty denoiser (DenoiseMode::Atrous). Unconditional member (not
    // OHAO_NRD-guarded) so the class layout is identical across ohao_renderer
    // and ohao_gpu_vulkan; dtor is out-of-line in path_tracer.cpp which sees
    // the complete type via atrous_denoise.hpp.
    std::unique_ptr<AtrousDenoiser> m_atrousDenoiser;

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
    static constexpr uint32_t m_maxBindlessTextures = 1024;
    VkBuffer m_normalBuffer = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkBuffer m_uvBuffer = VK_NULL_HANDLE;
    VkBuffer m_matIDBuffer = VK_NULL_HANDLE;
    VkBuffer m_matColorBuffer = VK_NULL_HANDLE;
    VkBuffer m_lightBuffer = VK_NULL_HANDLE;
    uint32_t m_lightCount = 0;

    // Env CDF storage buffers (bindings 17, 18)
    VkBuffer m_envMarginalCDFBuffer   = VK_NULL_HANDLE;
    VkBuffer m_envConditionalCDFBuffer = VK_NULL_HANDLE;
    uint32_t m_envCDFWidth   = 0;
    uint32_t m_envCDFHeight  = 0;
    float    m_envCDFIntegral = 0.0f;

    // Sub-plan 4.F T1: HDR env map for NRD tonemap's sky composite. Borrowed —
    // the renderer owns the VkImage/VkImageView lifetime.
    VkImageView m_envMapView    = VK_NULL_HANDLE;
    VkSampler   m_envMapSampler = VK_NULL_HANDLE;

    // Bindless textures — individual sampler2D entries
    std::vector<VkImageView> m_bindlessImageViews;
    std::vector<VkSampler> m_bindlessSamplers;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    uint32_t m_bindlessTextureCount = 0;

    // Legacy (kept for compatibility during migration)
    VkImageView m_textureArrayView = VK_NULL_HANDLE;
    VkSampler m_textureSampler = VK_NULL_HANDLE;
    uint32_t m_textureArrayCount = 0;
    uint32_t m_normalVertexCount = 0;
    uint32_t m_sampleIndex = 0;
    uint32_t m_historyFrameCount = 0;
    bool m_viewChangedThisFrame = false;

    // Accumulation buffer — RGBA32F HDR
    VkImage m_accumBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_accumMemory = VK_NULL_HANDLE;
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

    // Feature 4.E/4.G: NRD final LDR output (RGBA8 UNORM) at binding 30.
    // Written by NrdCinematicPost::dispatchComposite (4.G chain replaced 4.E's
    // single-pass tonemap). NOT in PT's RT descriptor layout — only in the
    // cinematic compute set.
    //
    // Sub-plan 4.J: binding 30 now holds the FINAL LDR after DoF gather.
    // The composite shader writes to the new intermediate (binding 32,
    // m_preDofLdrImage) and the DoF compute pass reads that + depth AOV
    // (binding 20) → produces binding 30.
    VkImage        m_nrdTonemappedImage     = VK_NULL_HANDLE;
    VkDeviceMemory m_nrdTonemappedMemory    = VK_NULL_HANDLE;
    VkImageView    m_nrdTonemappedView      = VK_NULL_HANDLE;
    bool           m_nrdTonemapFirstFrame   = true;  // gates UNDEFINED→GENERAL on binding 30

    // Feature 4.J: pre-DoF LDR (RGBA8 UNORM) at binding 32.
    // Composite shader's outLDR is now wired to this view; DoF compute reads
    // it + depth and writes the final m_nrdTonemappedImage (binding 30).
    // Not in PT's RT descriptor layout — only used inside the cinematic chain.
    VkImage        m_preDofLdrImage     = VK_NULL_HANDLE;
    VkDeviceMemory m_preDofLdrMemory    = VK_NULL_HANDLE;
    VkImageView    m_preDofLdrView      = VK_NULL_HANDLE;
    bool           m_preDofFirstFrame   = true;  // gates UNDEFINED→GENERAL on binding 32

    // Feature 4.G: bloom mip chain (RGBA16F) — fed by cinematic_bloom_extract +
    // cinematic_bloom_blur, consumed by cinematic_composite. Mip 0 = half-res,
    // mip 1 = quarter-res, mip 2 = eighth-res. Each carries a usage of
    // STORAGE (for extract/blur writes) + SAMPLED (for composite reads), so we
    // toggle their layouts UNDEFINED→GENERAL on first dispatch, then
    // GENERAL→SHADER_READ_ONLY before the composite pass each frame.
    VkImage        m_bloomMipImages[3]      = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory m_bloomMipMemory[3]      = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    m_bloomMipViews[3]       = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSampler      m_bloomSampler           = VK_NULL_HANDLE;  // shared bilinear sampler
    uint32_t       m_bloomMipWidth[3]       = {0, 0, 0};
    uint32_t       m_bloomMipHeight[3]      = {0, 0, 0};
    bool           m_bloomFirstFrame[3]     = {true, true, true};

    // Surface history ping-pong for realtime validation (xyz = first-hit world pos, w = hitDist)
    VkImage m_surfaceHistoryImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory m_surfaceHistoryMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView m_surfaceHistoryViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool m_surfaceHistoryInitialized[2] = {false, false};
    uint32_t m_surfaceHistoryWriteIndex = 0;

    // Shading history ping-pong for realtime validation (xyz = first-hit normal, w = roughness)
    VkImage m_shadingHistoryImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory m_shadingHistoryMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView m_shadingHistoryViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool m_shadingHistoryInitialized[2] = {false, false};
    uint32_t m_shadingHistoryWriteIndex = 0;

    // Material buffer (per-instance albedo, vec4 SSBO)
    VkBuffer m_materialBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_materialMemory = VK_NULL_HANDLE;
    std::vector<glm::vec4> m_materialData;

    // RT Pipeline
    VkPipeline m_rtPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    // Shader Binding Table
    VkBuffer m_sbtBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_sbtMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};  // empty, not used

    // Push constants — 256 bytes (Vulkan max on most RT GPUs)
    struct PTPushConstants {
        glm::mat4 invView;              // 64 bytes
        glm::mat4 invProj;              // 64 bytes
        glm::mat4 prevViewProj;         // 64 bytes — for temporal reprojection
        glm::uvec4 params;              // 16 bytes  (x=width, y=height, z=sampleIndex, w=maxBounces)
        glm::uvec4 control;             // x=flags, y=historyFrameCount, z=viewChanged, w=envCDFWidth
                                        // control.w = envCDFWidth. If 0, shader must skip env importance sampling.
        glm::vec4 tuning;               // x=fireflyClamp, y=envCDFHeight, z=envIntegral, w=unused
        glm::vec4 jitter;               // 4.F T4: xy=Halton(2,3) pixel offset (0 outside NRD mode), zw=pad
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
