#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include "utils/common_types.hpp"

namespace ohao {

class Scene;
class Camera;
class Actor;

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

// Per-object push constants (model matrix + material)
struct ObjectPushConstants {
    alignas(16) glm::mat4 model;
    alignas(16) glm::vec3 baseColor;
    alignas(4) float metallic;
    alignas(4) float roughness;
    alignas(4) float ao;
    alignas(8) glm::vec2 padding;
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
 * OffscreenRenderer - Renders OHAO scenes to a pixel buffer without a window
 *
 * Used for embedding OHAO rendering in external applications (like Godot)
 *
 * Usage:
 *   OffscreenRenderer renderer(800, 600);
 *   renderer.initialize();
 *   renderer.setScene(scene);
 *   renderer.render();
 *   const uint8_t* pixels = renderer.getPixels(); // RGBA format
 */
class OffscreenRenderer {
public:
    OffscreenRenderer(uint32_t width, uint32_t height);
    ~OffscreenRenderer();

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

    // Pixel access (RGBA format, 4 bytes per pixel)
    const uint8_t* getPixels() const { return m_pixelBuffer.data(); }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    size_t getPixelBufferSize() const { return m_pixelBuffer.size(); }

    // Physics
    void updatePhysics(float deltaTime);

    // Scene buffer management (call after modifying scene)
    bool updateSceneBuffers();

    // Check if scene has renderable meshes
    bool hasSceneMeshes() const { return m_hasSceneMeshes; }

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
    void renderShadowPass();
    glm::mat4 calculateLightSpaceMatrix(const LightData& light);
    void cleanupShadowResources();

    // Phase 1: Rendering pipeline setup
    bool createDescriptorSetLayout();
    bool createDescriptorPool();
    bool createDescriptorSets();
    bool createUniformBuffer();
    bool createLightBuffer();   // Phase 3: Light uniform buffer
    bool createVertexBuffer();  // Creates demo triangle
    VkShaderModule loadShaderModule(const std::string& filepath);
    void updateUniformBuffer();
    void updateLightBuffer();   // Phase 3: Collect and update lights

    // Phase 2: Scene mesh rendering
    void renderSceneObjects();  // Draw all scene meshes with push constants

    // Rendering internals
    void recordCommandBuffer();
    void copyFramebufferToPixelBuffer();

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

    // Sync
    VkFence m_renderFence{VK_NULL_HANDLE};

    // Pixel buffer (CPU accessible)
    std::vector<uint8_t> m_pixelBuffer;

    // Scene and camera
    Scene* m_scene{nullptr};
    std::unique_ptr<Camera> m_camera;

    // Queue family index
    uint32_t m_graphicsQueueFamily{0};

    // Shader base path
    std::string m_shaderBasePath;

    // Initialized flag
    bool m_initialized{false};
};

} // namespace ohao
