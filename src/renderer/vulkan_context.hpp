#pragma once
#include "ui/window/window.hpp"
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <renderer/rhi/vk/ohao_vk_instance.hpp>
#include <renderer/rhi/vk/ohao_vk_buffer.hpp>
#include <renderer/rhi/vk/ohao_vk_command_manager.hpp>
#include <renderer/rhi/vk/ohao_vk_descriptor.hpp>
#include <renderer/rhi/vk/ohao_vk_device.hpp>
#include <renderer/rhi/vk/ohao_vk_framebuffer.hpp>
#include <renderer/rhi/vk/ohao_vk_image.hpp>
#include <renderer/rhi/vk/ohao_vk_physical_device.hpp>
#include <renderer/rhi/vk/ohao_vk_pipeline.hpp>
#include <renderer/rhi/vk/ohao_vk_render_pass.hpp>
#include <renderer/rhi/vk/ohao_vk_shader_module.hpp>
#include <renderer/rhi/vk/ohao_vk_surface.hpp>
#include <renderer/rhi/vk/ohao_vk_swapchain.hpp>
#include <renderer/rhi/vk/ohao_vk_sync_objects.hpp>
#include <renderer/rhi/vk/ohao_vk_uniform_buffer.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "renderer/camera/camera.hpp"
#include "engine/asset/model.hpp"
#include "renderer/material/material.hpp"
#include "engine/scene/scene.hpp"
#include "subsystems/scene/scene_renderer.hpp"
#include "renderer/shader/shader_uniforms.hpp"
#include "renderer/gizmo/axis_gizmo.hpp"
#include "renderer/picking/picking_system.hpp"

// Forward declaration to avoid include dependency issues
namespace ohao {
    class ShadowRenderer;
    class LightingSystem;
    class ShadowMapPool;
}


#define GPU_VENDOR_NVIDIA 0
#define GPU_VENDOR_AMD 1
#define GPU_VENDOR_INTEL 2

// Change this to select preferred GPU vendor
#define PREFERRED_GPU_VENDOR GPU_VENDOR_NVIDIA

namespace ohao {

class SceneRenderer;
class UIManager;
class VulkanContext {
public:

    using UniformBufferObject = GlobalUniformBuffer;

    VulkanContext() = delete;
    VulkanContext(Window* windowHandle);
    ~VulkanContext();

    void initializeVulkan();
    void initializeSceneRenderer();
    void initializeDefaultScene();
    std::shared_ptr<Model> generateSphereMesh();
    void cleanup();
    void recreateSwapChain();
    void cleanupSwapChain();
    void cleanupSceneBuffers();


    //Getters
    // === OhaoVk Object Getters ===
    static VulkanContext* getContextInstance() { return contextInstance; }
    OhaoVkInstance* getInstance() const { return instance.get(); }
    OhaoVkSurface* getSurface() const { return surface.get(); }
    OhaoVkPhysicalDevice* getPhysicalDevice() const { return physicalDevice.get(); }
    OhaoVkDevice* getLogicalDevice() const { return device.get(); }
    OhaoVkSwapChain* getSwapChain() const { return swapchain.get(); }
    OhaoVkShaderModule* getShaderModules() const { return shaderModules.get(); }
    OhaoVkRenderPass* getRenderPass() const { return renderPass.get(); }
    OhaoVkPipeline* getPipeline() const { return pipeline.get(); }
    OhaoVkPipeline* getModelPipeline() const { return modelPipeline.get(); }
    OhaoVkPipeline* getWireframePipeline() const { return wireframePipeline.get(); }
    OhaoVkPipeline* getGizmoPipeline() const { return gizmoPipeline.get(); }
    OhaoVkPipeline* getModelPushConstantPipeline() const { return modelPushConstantPipeline.get(); }
    OhaoVkDescriptor* getDescriptor() const { return descriptor.get(); }
    OhaoVkImage* getDepthImage() const { return depthImage.get(); }
    OhaoVkFramebuffer* getFramebufferManager() const { return framebufferManager.get(); }
    OhaoVkCommandManager* getCommandManager() const { return commandManager.get(); }
    OhaoVkSyncObjects* getSyncObjects() const { return syncObjects.get(); }
    OhaoVkBuffer* getVertexBuffer() const { 
        if (!vertexBuffer || !vertexBuffer->isValid()) {
            fprintf(stderr, "Warning: Attempting to access null or invalid vertex buffer\n");
        }
        return vertexBuffer.get(); 
    }
    OhaoVkBuffer* getIndexBuffer() const { 
        if (!indexBuffer || !indexBuffer->isValid()) {
            fprintf(stderr, "Warning: Attempting to access null or invalid index buffer\n");
        }
        return indexBuffer.get(); 
    }
    // === Raw Vulkan Handle Getters ===
    VkInstance getVkInstance() const { return instance ? instance->getInstance() : VK_NULL_HANDLE; }
    VkSurfaceKHR getVkSurface() const { return surface ? surface->getSurface() : VK_NULL_HANDLE; }
    VkPhysicalDevice getVkPhysicalDevice() const { return physicalDevice ? physicalDevice->getDevice() : VK_NULL_HANDLE; }
    VkDevice getVkDevice() const { return device ? device->getDevice() : VK_NULL_HANDLE; }
    VkSwapchainKHR getVkSwapChain() const { return swapchain ? swapchain->getSwapChain() : VK_NULL_HANDLE; }
    VkRenderPass getVkRenderPass() const { return renderPass ? renderPass->getVkRenderPass() : VK_NULL_HANDLE; }
    VkPipeline getVkPipeline() const { return pipeline ? pipeline->getPipeline() : VK_NULL_HANDLE; }
    VkPipelineLayout getVkPipelineLayout() const { return pipeline ? pipeline->getPipelineLayout() : VK_NULL_HANDLE; }
    VkDescriptorPool getVkDescriptorPool() const { return descriptor ? descriptor->getPool() : VK_NULL_HANDLE; }
    VkDescriptorSetLayout getVkDescriptorSetLayout() const { return descriptor ? descriptor->getLayout() : VK_NULL_HANDLE; }
    VkImage getVkDepthImage() const { return depthImage ? depthImage->getImage() : VK_NULL_HANDLE; }
    VkImageView getVkDepthImageView() const { return depthImage ? depthImage->getImageView() : VK_NULL_HANDLE; }
    VkCommandPool getVkCommandPool() const { return commandManager ? commandManager->getCommandPool() : VK_NULL_HANDLE; }
    VkBuffer getVkVertexBuffer() const { return vertexBuffer ? vertexBuffer->getBuffer() : VK_NULL_HANDLE; }
    VkBuffer getVkIndexBuffer() const { return indexBuffer ? indexBuffer->getBuffer() : VK_NULL_HANDLE; }
    // Sync objects getters
    VkSemaphore getImageAvailableSemaphore(size_t frame) const {return syncObjects ? syncObjects->getImageAvailableSemaphore(frame) : VK_NULL_HANDLE;}
    VkSemaphore getRenderFinishedSemaphore(size_t frame) const {return syncObjects ? syncObjects->getRenderFinishedSemaphore(frame) : VK_NULL_HANDLE;}
    VkFence getInFlightFence(size_t frame) const {return syncObjects ? syncObjects->getInFlightFence(frame) : VK_NULL_HANDLE;}

    Camera& getCamera() {return camera;}
    OhaoVkUniformBuffer* getUniformBuffer() const {return uniformBuffer.get();}
    size_t getCurrentFrame() const { return currentFrame; }

    void drawFrame();
    void renderModel(VkCommandBuffer commandBuffer);
    void renderGizmos(VkCommandBuffer commandBuffer);
    bool hasLoadScene();
    bool createNewScene(const std::string& name);
    bool saveScene(const std::string& filename);
    bool loadScene(const std::string& filename);
    bool importModel(const std::string& filename);
    void cleanupCurrentModel();
    void updateViewport(uint32_t width, uint32_t height);

    VkDescriptorSet getSceneTextureDescriptor() const { return sceneTextureDescriptor; }
    void setViewportSize(uint32_t width, uint32_t height);
    void setUIManager(std::shared_ptr<UIManager> manager) {uiManager = manager;}
    std::shared_ptr<UIManager> getUIManager() const {return uiManager;}
    SceneRenderer* getSceneRenderer() const {return sceneRenderer.get();}
    Scene* getScene() const {return scene.get();}
    AxisGizmo* getAxisGizmo() const {
        return sceneRenderer ? sceneRenderer->getAxisGizmo() : nullptr;
    }

    PickingSystem* getPickingSystem() const { return pickingSystem.get(); }
    ShadowRenderer* getShadowRenderer() const { return shadowRenderer.get(); }

    void toggleWireframeMode() { wireframeMode = !wireframeMode; }
    bool isWireframeMode() const { return wireframeMode; }
    void setWireframeMode(bool enable) { wireframeMode = enable; }
    
    // Scene updates
    void updateScene(float deltaTime);

    bool updateModelBuffers(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    bool updateSceneBuffers(); //update all scene objects buffers
    // ID-based lookup (reliable across frames)
    const MeshBufferInfo* getMeshBufferInfo(uint64_t actorId) const {
        auto it = meshBufferMap.find(actorId);
        if (it != meshBufferMap.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Overload that fills a reference with the data
    bool getMeshBufferInfo(uint64_t actorId, MeshBufferInfo& outInfo) const {
        auto it = meshBufferMap.find(actorId);
        if (it != meshBufferMap.end()) {
            outInfo = it->second;
            return true;
        }
        return false;
    }

    // Get meshBufferMap size for diagnostics
    size_t getMeshBufferMapSize() const { return meshBufferMap.size(); }

    bool hasUnsavedChanges() const { return sceneModified; }
    void markSceneModified() { sceneModified = true; }
    void clearSceneModified() { sceneModified = false; }



private:
    Window* window;
    static VulkanContext* contextInstance;

    //vulkan
    const int MAX_FRAMES_IN_FLIGHT = 2;
    size_t currentFrame = 0;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width{}, height{};
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue presentQueue{VK_NULL_HANDLE};
    std::unique_ptr<OhaoVkInstance> instance;
    std::unique_ptr<OhaoVkSurface> surface;
    std::unique_ptr<OhaoVkPhysicalDevice> physicalDevice;
    std::unique_ptr<OhaoVkDevice> device;
    std::unique_ptr<OhaoVkSwapChain> swapchain;
    std::unique_ptr<OhaoVkShaderModule> shaderModules;
    std::unique_ptr<OhaoVkRenderPass> renderPass;
    std::unique_ptr<OhaoVkPipeline> pipeline;
    std::unique_ptr<OhaoVkPipeline> modelPipeline;      // For solid model rendering
    std::unique_ptr<OhaoVkPipeline> wireframePipeline;  // For wireframe model rendering
    std::unique_ptr<OhaoVkPipeline> gizmoPipeline;      // For gizmo
    std::unique_ptr<OhaoVkPipeline> modelPushConstantPipeline; // For model rendering with push constants
    std::unique_ptr<OhaoVkPipeline> scenePipeline;
    std::unique_ptr<OhaoVkPipeline> sceneGizmoPipeline;
    std::unique_ptr<OhaoVkDescriptor> descriptor;
    std::unique_ptr<OhaoVkImage> depthImage;
    std::unique_ptr<OhaoVkFramebuffer> framebufferManager;
    std::unique_ptr<OhaoVkCommandManager> commandManager;
    std::unique_ptr<OhaoVkSyncObjects> syncObjects;
    std::unique_ptr<OhaoVkBuffer> vertexBuffer;
    std::unique_ptr<OhaoVkBuffer> indexBuffer;
    std::unique_ptr<OhaoVkBuffer> gizmoVertexBuffer;
    std::unique_ptr<OhaoVkBuffer> gizmoIndexBuffer;
    std::unique_ptr<OhaoVkUniformBuffer> uniformBuffer;
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void createVertexBuffer(const std::vector<Vertex>& vertices);
    void createIndexBuffer(const std::vector<uint32_t>& indices);

    VkImage sceneColorImage{VK_NULL_HANDLE};
    VkDeviceMemory sceneColorMemory{VK_NULL_HANDLE};
    VkImageView sceneColorView{VK_NULL_HANDLE};
    VkDescriptorSet sceneTextureDescriptor{VK_NULL_HANDLE};

    Camera camera;
    std::unique_ptr<Scene> scene;

    uint32_t lastWidth{0};
    uint32_t lastHeight{0};
    bool needsResize{false};
    // Maintain a constant camera/reference aspect; render is letterboxed within panel
    
    std::shared_ptr<UIManager> uiManager;
    std::unique_ptr<SceneRenderer> sceneRenderer;

    bool wireframeMode{false};
    uint32_t gizmoIndexCount{0};
    std::unique_ptr<AxisGizmo> axisGizmo;

    std::unordered_map<uint64_t, MeshBufferInfo> meshBufferMap;  // Key = Actor ID

    bool sceneModified{false};

    // Picking system for viewport selection
    std::unique_ptr<PickingSystem> pickingSystem;

    // Shadow renderer for shadow mapping (legacy)
    std::unique_ptr<ShadowRenderer> shadowRenderer;

    // Unified lighting system
    std::unique_ptr<LightingSystem> lightingSystem;
    std::unique_ptr<ShadowMapPool> shadowMapPool;
};

} // namespace ohao
