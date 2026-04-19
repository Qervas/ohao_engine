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
#include "render/rt/sampler_types.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace ohao {

enum class RTRenderProfile {
    Realtime,
    Offline,
};

struct RTRenderSettings {
    RTRenderProfile profile{RTRenderProfile::Offline};
    uint32_t maxBounces{4};
    bool preferAccumulation{true};
    bool enableAuxiliaryAOVs{true};
    bool allowExternalDenoiser{true};
    bool enableInternalDenoise{false};
    bool enableFireflyClamp{false};
    float fireflyClampLuminance{10.0f};
    SamplerType samplerType{SamplerType::Sobol};
    DenoiseMode denoiseMode{DenoiseMode::None};
};

struct PathTracerShaderSet {
    const char* raygenSpv{"bin/shaders/rt_pt_raygen.rgen.spv"};
    const char* missSpv{"bin/shaders/rt_pt_miss.rmiss.spv"};
    const char* closestHitSpv{"bin/shaders/rt_pt_closesthit.rchit.spv"};
    const char* anyHitSpv{"bin/shaders/rt_pt_anyhit.rahit.spv"};
};

inline constexpr RTRenderSettings kRealtimeRTSettings{
    RTRenderProfile::Realtime,
    2,
    true,
    true,
    false,
    true,
    true,
    10.0f,
    SamplerType::PCG,
    DenoiseMode::None,    // realtime uses NRD/DLSS RR (Sub-plans 4-5), not OIDN
};

inline constexpr RTRenderSettings kOfflineRTSettings{
    RTRenderProfile::Offline,
    4,
    true,
    true,
    true,
    false,
    false,
    0.0f,
    SamplerType::Sobol,
    DenoiseMode::OIDN,    // offline default — matches Cycles
};

class PathTracer {
public:
    PathTracer() = default;
    ~PathTracer();

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height);
    void setShaderSet(const PathTracerShaderSet& shaderSet) { m_shaderSet = shaderSet; }

    void render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& lightPos, float lightIntensity,
                const glm::vec3& lightColor, float lightRadius);

    void resize(uint32_t width, uint32_t height);

    VkImage getOutputImage() const { return m_outputImage; }
    VkImage getAccumImage() const { return m_accumBuffer; }
    VkImage getAlbedoAOV() const { return m_albedoAOV; }
    VkImage getNormalAOV() const { return m_normalAOV; }
    VkImageView getOutputView() const { return m_outputView; }
    VkImageView getMotionVectorAOV() const { return m_motionVectorView; }
    VkImage getMotionVectorImage() const { return m_motionVectorImage; }
    VkImageView getDepthAOV()         const { return m_depthAOVView; }
    VkImage     getDepthAOVImage()    const { return m_depthAOVImage; }
    VkImageView getRoughnessAOV()     const { return m_roughnessAOVView; }
    VkImage     getRoughnessAOVImage() const { return m_roughnessAOVImage; }
    VkImageView getDiffuseRadianceAOV()      const { return m_diffuseRadianceView; }
    VkImage     getDiffuseRadianceAOVImage() const { return m_diffuseRadianceImage; }
    VkImageView getSpecularRadianceAOV()      const { return m_specularRadianceView; }
    VkImage     getSpecularRadianceAOVImage() const { return m_specularRadianceImage; }
    VkImageView getDiffAlbedoAOV()      const { return m_diffAlbedoView; }
    VkImage     getDiffAlbedoAOVImage() const { return m_diffAlbedoImage; }
    VkImageView getSpecColorAOV()       const { return m_specColorView; }
    VkImage     getSpecColorAOVImage()  const { return m_specColorImage; }

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
    std::vector<VkImageView> getBindlessImageViews() const { return m_bindlessImageViews; }
    std::vector<VkSampler> getBindlessSamplers() const { return m_bindlessSamplers; }

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

    // Reset accumulation — call when camera moves so the buffer restarts
    void notifyViewChanged() { m_viewChangedThisFrame = true; }
    void resetAccumulation();

    void destroy();

    // Config
    void setMaxBounces(uint32_t bounces) { m_maxBounces = bounces; }
    void setRenderSettings(const RTRenderSettings& settings) { m_renderSettings = settings; }
    const RTRenderSettings& getRenderSettings() const { return m_renderSettings; }
    uint32_t getFrameIndex() const { return m_historyFrameCount; }

private:
    bool createImages();
    bool createMaterialBuffer();
    bool createDescriptorResources();
    bool createRTPipeline();
    bool createShaderBindingTable();
    bool loadFunctionPointers();

    void destroyImages();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

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

    // Push constants — 240 bytes
    struct PTPushConstants {
        glm::mat4 invView;              // 64 bytes
        glm::mat4 invProj;              // 64 bytes
        glm::mat4 prevViewProj;         // 64 bytes — for temporal reprojection
        glm::uvec4 params;              // 16 bytes  (x=width, y=height, z=sampleIndex, w=maxBounces)
        glm::uvec4 control;             // x=flags, y=historyFrameCount, z=viewChanged, w=envCDFWidth
                                        // control.w = envCDFWidth. If 0, shader must skip env importance sampling.
        glm::vec4 tuning;               // x=fireflyClamp, y=envCDFHeight, z=envIntegral, w=unused
    };  // total = 240 bytes

    glm::mat4 m_prevViewProj{1.0f};  // stored from last frame

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
