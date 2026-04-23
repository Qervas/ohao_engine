#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include <glm/glm.hpp>
#include "core/common_types.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/rt/rt_acceleration_structure.hpp"
#include "render/rt/path_tracer.hpp"
#include "render/rt/rt_render_pipeline.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/rt_profile_renderer.hpp"
#include "render/rt/gpu_skinning.hpp"
#include "render/rt/animated_rt_manager.hpp"

namespace ohao {

class Scene;
class Camera;
class Actor;
class DeferredRenderer;

// Render mode selection
enum class RenderMode {
    Forward,      // Legacy forward rendering (8 light limit)
    Deferred,     // AAA deferred rendering (CSM, post-processing)
    RTRealtime,   // Path tracing tuned for interactive use
    RTOffline     // Path tracing tuned for reference/offline rendering
};

// Simple vertex structure for basic rendering (Phase 1 triangle)
struct SimpleVertex {
    glm::vec3 position;
    glm::vec3 color;
};

// Camera uniform buffer (view/proj matrices + view position)
struct CameraUniformBuffer {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::vec3 viewPos;
};

// Per-object push constants (matches GBuffer pass shader layout)
struct ObjectPushConstants {
    glm::mat4 model;
    glm::mat4 viewProj;
    glm::mat4 prevMVP;
    glm::vec4 materialParams;  // x=metallic, y=roughness, z=roughMetalTexIdx, w=albedoTexIdx
    glm::vec4 albedoColor;     // rgb=albedo, a=normalTexIdx
    glm::vec4 emissiveParams;  // x=emissiveTexIdx, y=emissiveStrength, z=unused, w=unused
};

// Light data for uniform buffer (matches shader layout)
// 128 bytes per light to match UnifiedLight in shaders
struct LightData {
    alignas(16) glm::vec4 position;      // xyz = position, w = type (0=dir, 1=point, 2=spot)
    alignas(16) glm::vec4 direction;     // xyz = direction, w = range
    alignas(16) glm::vec4 color;         // xyz = color, w = intensity
    alignas(16) glm::vec4 params;        // x = innerCone, y = outerCone, z = shadowMapIndex (-1=none), w = unused
    alignas(16) glm::mat4 lightSpaceMatrix; // Transform to light space for shadow mapping (64 bytes)
};

// Maximum lights supported
constexpr uint32_t MAX_LIGHTS = 8;

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
 * VulkanRenderer - Renders OHAO scenes to a pixel buffer without a window
 *
 * Used for embedding OHAO rendering in external applications (like Godot)
 *
 * Usage:
 *   VulkanRenderer renderer(800, 600);
 *   renderer.initialize();
 *   renderer.setScene(scene);
 *   renderer.render();
 *   const uint8_t* pixels = renderer.getPixels(); // RGBA format
 */
class VulkanRenderer {
public:
    VulkanRenderer(uint32_t width, uint32_t height);
    ~VulkanRenderer();

    // Lifecycle
    bool initialize();
    void shutdown();

    // Rendering
    void render();
    void resize(uint32_t width, uint32_t height);

    // Scene management
    void setScene(Scene* scene);
    Scene* getScene() const { return m_scene; }

    // Camera
    Camera& getCamera() { return *m_camera; }
    const Camera& getCamera() const { return *m_camera; }

    // Render mode
    void setRenderMode(RenderMode mode);
    RenderMode getRenderMode() const { return m_renderMode; }
    void setRTRenderSettings(const RTRenderSettings& settings);
    void setRTRenderProfile(RTRenderProfile profile);
    const RTRenderSettings& getRTRenderSettings() const { return m_rtSettings; }
    void setEnvironmentMap(const std::string& path) { m_envMapPath = path; }
    void notifyCameraChanged();
    void resetAccumulation();
    uint32_t getPathTracerFrameIndex() const;

    // Read back HDR buffers for OIDN denoising
    bool readbackHDRBuffers(std::vector<float>& beauty, std::vector<float>& albedo,
                            std::vector<float>& normal, uint32_t& w, uint32_t& h);

    // Deferred renderer access (for configuration)
    DeferredRenderer* getDeferredRenderer() { return m_deferredRenderer.get(); }

    // Texture manager access
    BindlessTextureManager* getTextureManager() { return m_textureManager.get(); }

    // RT acceleration structure access
    RTAccelerationStructure* getRT() { return m_rtAccel.get(); }
    bool isRTSupported() const { return m_rtAccel && m_rtAccel->isSupported(); }

    // Denoiser control
    void        setDenoiseMode(DenoiseMode mode);
    DenoiseMode getDenoiseMode() const { return m_denoiseMode; }

    // Returns the motion vector AOV image view from the active RT profile,
    // or VK_NULL_HANDLE if no RT profile is active.
    VkImageView getMotionVectorAOV() const;

    // Returns the motion vector AOV VkImage (needed for vkCmdCopyImageToBuffer).
    VkImage getMotionVectorImage() const;

    // Returns the depth / roughness AOV image views and images from the active
    // RT profile, or VK_NULL_HANDLE if no RT profile is active.
    VkImageView getDepthAOV()         const;
    VkImage     getDepthAOVImage()    const;
    VkImageView getRoughnessAOV()     const;
    VkImage     getRoughnessAOVImage() const;
    VkImageView getDiffuseRadianceAOV()      const;
    VkImage     getDiffuseRadianceAOVImage() const;
    VkImageView getSpecularRadianceAOV()      const;
    VkImage     getSpecularRadianceAOVImage() const;
    VkImageView getDiffAlbedoAOV()      const;
    VkImage     getDiffAlbedoAOVImage() const;
    VkImageView getSpecColorAOV()       const;
    VkImage     getSpecColorAOVImage()  const;
    VkImageView getNormalRoughnessAOV()      const;
    VkImage     getNormalRoughnessAOVImage() const;
    // Sub-plan 4.C: NRD denoised outputs (RGBA32F)
    VkImage     getOutDiffRadianceAOVImage() const;
    VkImage     getOutSpecRadianceAOVImage() const;

    // Debug: readback the motion vector AOV as raw uint16_t pairs (RG16F interleaved).
    // One 2-half pair per pixel; total = 2 * width * height values.
    // Returns false if no RT profile is active or readback fails.
    bool readbackMotionVector(std::vector<uint16_t>& mvRaw, uint32_t& width, uint32_t& height);

    // Debug: readback the depth AOV as raw float buffer (1 float per pixel).
    bool readbackDepthAOV(std::vector<float>& depthData, uint32_t& width, uint32_t& height);

    // Debug: readback R16F roughness AOV as native float (decoded from half in-helper).
    bool readbackRoughnessAOV(std::vector<float>& roughData, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA32F diffuse radiance AOV (4 floats per pixel, native).
    bool readbackDiffuseRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA32F specular radiance AOV (4 floats per pixel, native).
    bool readbackSpecularRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Sub-plan 4.C: NRD denoised output readback (RGBA32F)
    bool readbackDenoisedDiffuse(std::vector<float>& data, uint32_t& width, uint32_t& height);
    bool readbackDenoisedSpecular(std::vector<float>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 diffuse albedo AOV (4 bytes per pixel).
    bool readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 specular color AOV (4 bytes per pixel).
    bool readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Returns pointer to RGBA8 tonemapped pixels. If denoiseMode != None,
    // the buffer is lazily denoised on the first call after render();
    // subsequent calls return the cached result until the next render().
    const uint8_t* getPixels() const;
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    size_t getPixelBufferSize() const { return m_pixelBuffer.size(); }

    // Physics
    void updatePhysics(float deltaTime);

    // Scene buffer management (call after modifying scene)
    bool updateSceneBuffers();

    // Check if scene has renderable meshes
    bool hasSceneMeshes() const { return m_hasSceneMeshes; }

    // Read GPU terrain heightmap back to CPU (blocking — one-time call at setup).
    // Returns false if no procedural terrain exists. outRes = square resolution.
    bool readTerrainHeights(std::vector<float>& outData, uint32_t& outRes);

private:
    // Vulkan setup (no window required)
    bool createInstance();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createCommandPool();
    bool createOffscreenFramebuffer();
    bool createRenderPass();
    bool createPipeline();
    bool createSyncObjects();

    // Shadow mapping
    bool createShadowResources();
    bool createShadowRenderPass();
    bool createShadowPipeline();
    void renderShadowPass(VkCommandBuffer cmd, VkDescriptorSet descriptorSet);
    glm::mat4 calculateLightSpaceMatrix(const LightData& light);
    void cleanupShadowResources();

    // Phase 1: Rendering pipeline setup
    bool createDescriptorSetLayout();
    bool createDescriptorPool();
    bool createDescriptorSets();
    bool createUniformBuffer();
    bool createLightBuffer();   // Phase 3: Light uniform buffer
    bool createVertexBuffer();  // Creates demo triangle
    bool initializeFrameResources();  // Multi-frame resource initialization
    VkShaderModule loadShaderModule(const std::string& filepath);
    void updateUniformBuffer();
    void updateUniformBuffer(uint32_t frameIndex);  // Per-frame version
    void updateLightBuffer();   // Phase 3: Collect and update lights
    void updateLightBuffer(uint32_t frameIndex);    // Per-frame version
    void cacheEmissiveLights(); // Scan emissive materials → cached LightData (called once per scene change)
    bool updateAnimatedActorsForRT();
    void prepareRTSceneForFrame(const IRTRenderPipeline& pipeline, bool hasDynamicBLAS);

    // Phase 2: Scene mesh rendering
    void renderSceneObjects(VkCommandBuffer cmd);  // Draw all scene meshes with push constants

    // Rendering internals
    void recordCommandBuffer();
    void copyFramebufferToPixelBuffer();
    void renderMultiFrame();  // Multi-frame ring buffer rendering
    void renderRTPipeline(const IRTRenderPipeline& pipeline);
public:
private:
    void renderLegacy();      // Legacy single-frame rendering
    const IRTRenderPipeline* getRTPipeline(RenderMode mode) const;
    IRTRendererProfile* getRTRenderer(RenderMode mode);
    const IRTRendererProfile* getRTRenderer(RenderMode mode) const;
    void forEachRTRenderer(const std::function<void(IRTRendererProfile&)>& fn);

    // Cleanup
    void cleanupFramebuffer();
    void cleanupSwapchain();

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
    uint32_t m_currentFrame{0};

    // Legacy single command buffer (kept for compatibility during transition)
    VkCommandBuffer m_commandBuffer{VK_NULL_HANDLE};

    // Offscreen framebuffer
    VkImage m_colorImage{VK_NULL_HANDLE};
    VkDeviceMemory m_colorImageMemory{VK_NULL_HANDLE};
    VkImageView m_colorImageView{VK_NULL_HANDLE};
    VkImage m_depthImage{VK_NULL_HANDLE};
    VkDeviceMemory m_depthImageMemory{VK_NULL_HANDLE};
    VkImageView m_depthImageView{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    VkRenderPass m_renderPass{VK_NULL_HANDLE};

    // Shadow mapping resources
    VkImage m_shadowImage{VK_NULL_HANDLE};
    VkDeviceMemory m_shadowImageMemory{VK_NULL_HANDLE};
    VkImageView m_shadowImageView{VK_NULL_HANDLE};
    VkSampler m_shadowSampler{VK_NULL_HANDLE};
    VkFramebuffer m_shadowFramebuffer{VK_NULL_HANDLE};
    VkRenderPass m_shadowRenderPass{VK_NULL_HANDLE};
    VkPipeline m_shadowPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_shadowPipelineLayout{VK_NULL_HANDLE};
    VkShaderModule m_shadowVertShader{VK_NULL_HANDLE};
    VkShaderModule m_shadowFragShader{VK_NULL_HANDLE};
    bool m_shadowsEnabled{true};

    // Staging buffer for pixel readback
    VkBuffer m_stagingBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_stagingBufferMemory{VK_NULL_HANDLE};

    // Pipeline
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};

    // Shaders
    VkShaderModule m_vertShaderModule{VK_NULL_HANDLE};
    VkShaderModule m_fragShaderModule{VK_NULL_HANDLE};

    // Descriptor sets
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Uniform buffer (camera)
    VkBuffer m_uniformBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_uniformBufferMemory{VK_NULL_HANDLE};
    void* m_uniformBufferMapped{nullptr};

    // Light uniform buffer
    VkBuffer m_lightBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_lightBufferMemory{VK_NULL_HANDLE};
    void* m_lightBufferMapped{nullptr};

    // Vertex buffer (combined for all scene meshes)
    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_vertexBufferMemory{VK_NULL_HANDLE};
    uint32_t m_vertexCount{0};

    // Index buffer (combined for all scene meshes)
    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_indexBufferMemory{VK_NULL_HANDLE};
    uint32_t m_indexCount{0};

    // Mesh buffer info map (actor ID -> buffer offsets)
    std::unordered_map<uint64_t, MeshBufferInfo> m_meshBufferMap;

    // Flag to track if scene has renderable meshes
    bool m_hasSceneMeshes{false};

    // RT acceleration structure + path tracer
    std::unique_ptr<RTAccelerationStructure> m_rtAccel;
    RTRealtimePipeline m_rtRealtimePipeline;
    RTOfflinePipeline m_rtOfflinePipeline;
    std::unique_ptr<RTRealtimeRenderer> m_rtRealtimeRenderer;
    std::unique_ptr<RTOfflineRenderer> m_rtOfflineRenderer;
    bool m_rtAccelDirty{true};
    void buildAccelerationStructures();  // orchestrator — calls sub-functions below
    void createRTVertexIndexBuffers();    // copy raster buffers to device-local RT buffers
    void createRTNormalUVBuffers();       // extract normals + UVs from vertex data
    void uploadRTMaterialBuffers();       // per-triangle material IDs + per-material colors
    void uploadRTTextureArray();          // build bindless texture array for path tracer
    void uploadDeferredTextures();        // bridge model textures to deferred BindlessTextureManager
    void uploadLightBuffer();             // collect scene LightComponents → GPU SSBO + env map
    void buildBLASTLAS();                 // build BLAS per actor, TLAS with instance transforms
    // Separate RT-flagged copies of vertex/index data (device-local + device address)
    VkBuffer m_rtVertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_rtVertexMemory{VK_NULL_HANDLE};
    VkBuffer m_rtIndexBuffer{VK_NULL_HANDLE};
    VkBuffer m_rtNormalBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_rtNormalMemory{VK_NULL_HANDLE};
    VkBuffer m_rtUVBuffer{VK_NULL_HANDLE};        // per-vertex UVs for texture sampling
    VkDeviceMemory m_rtUVMemory{VK_NULL_HANDLE};
    VkBuffer m_rtMatIDBuffer{VK_NULL_HANDLE};     // per-triangle material index
    VkDeviceMemory m_rtMatIDMemory{VK_NULL_HANDLE};
    VkBuffer m_rtMatColorBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_rtMatColorMemory{VK_NULL_HANDLE};
    VkBuffer m_rtLightBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_rtLightMemory{VK_NULL_HANDLE};
    uint32_t m_rtLightCount{0};
    VkBuffer m_envMarginalCDFBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_envMarginalCDFMemory{VK_NULL_HANDLE};
    VkBuffer m_envConditionalCDFBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_envConditionalCDFMemory{VK_NULL_HANDLE};
    std::string m_envMapPath;
    VkImageView m_envMapImageView{VK_NULL_HANDLE};  // for deferred pipeline
    VkImage m_rtTextureArray{VK_NULL_HANDLE};
    VkDeviceMemory m_rtTextureArrayMemory{VK_NULL_HANDLE};
    VkImageView m_rtTextureArrayView{VK_NULL_HANDLE};
    VkSampler m_rtTextureSampler{VK_NULL_HANDLE};
    uint32_t m_rtTextureCount{0};
    VkDeviceMemory m_rtIndexMemory{VK_NULL_HANDLE};

    // Cached emissive mesh lights (computed once during updateSceneBuffers, used per-frame)
    std::vector<LightData> m_cachedEmissiveLights;

    // GPU compute skinning for animated BLAS rebuild
    std::unique_ptr<GPUSkinning> m_gpuSkinning;

    // Consolidated animated RT pipeline (skinning → BLAS → TLAS → material sync)
    std::unique_ptr<AnimatedRTManager> m_animatedRT;

    // Sync
    VkFence m_renderFence{VK_NULL_HANDLE};

    // Pixel buffer (CPU accessible)
    std::vector<uint8_t> m_pixelBuffer;

    // Denoise state
    DenoiseMode                  m_denoiseMode{DenoiseMode::None};
    bool                         m_denoiseModeOverridden{false}; // true = setDenoiseMode() called; blocks applyRTRenderSettings from resetting it
    mutable std::vector<uint8_t> m_denoisedPixelBuffer;
    mutable bool                 m_denoiseCacheValid{false};

    // Scene and camera
    Scene* m_scene{nullptr};
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

    // Shader base path
    std::string m_shaderBasePath;

    // Initialized flag
    bool m_initialized{false};

    // Render mode
    // Default to Forward until deferred output integration is complete
    RenderMode m_renderMode{RenderMode::Forward};

    // Deferred renderer (AAA quality)
    std::unique_ptr<DeferredRenderer> m_deferredRenderer;

    // Bindless texture manager
    std::unique_ptr<BindlessTextureManager> m_textureManager;

    // Deferred rendering methods
    bool initializeDeferredRenderer();
    void applyRTRenderSettings();
    void renderDeferred();
    void copyDeferredOutputToPixelBuffer(VkCommandBuffer cmd);

    RTRenderSettings m_rtSettings{kOfflineRTSettings};
};

} // namespace ohao
