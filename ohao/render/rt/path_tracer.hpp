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
};

inline constexpr RTRenderSettings kRealtimeRTSettings{
    RTRenderProfile::Realtime,
    2,
    true,
    false,
    false,
    true,
    true,
    10.0f,
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
};

class PathTracer {
public:
    PathTracer() = default;
    ~PathTracer();

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t width, uint32_t height);

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

    // Reset accumulation — call when camera moves so the buffer restarts
    void resetAccumulation();

    void destroy();

    // Config
    void setMaxBounces(uint32_t bounces) { m_maxBounces = bounces; }
    void setRenderSettings(const RTRenderSettings& settings) { m_renderSettings = settings; }
    const RTRenderSettings& getRenderSettings() const { return m_renderSettings; }
    uint32_t getFrameIndex() const { return m_frameIndex; }

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
    static constexpr uint32_t m_maxBindlessTextures = 1024;
    VkBuffer m_normalBuffer = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkBuffer m_uvBuffer = VK_NULL_HANDLE;
    VkBuffer m_matIDBuffer = VK_NULL_HANDLE;
    VkBuffer m_matColorBuffer = VK_NULL_HANDLE;
    VkBuffer m_lightBuffer = VK_NULL_HANDLE;
    uint32_t m_lightCount = 0;

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
    uint32_t m_frameIndex = 0;

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

    // Push constants — 176 bytes
    struct PTPushConstants {
        glm::mat4 invView;              // 64 bytes
        glm::mat4 invProj;              // 64 bytes
        glm::mat4 prevViewProj;         // 64 bytes — for temporal reprojection
        glm::uvec4 params;              // 16 bytes  (x=width, y=height, z=frameIndex, w=maxBounces)
        glm::uvec4 control;             // x=flags
        glm::vec4 tuning;               // x=firefly clamp luminance
    };  // total = 208 bytes

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
