#pragma once
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <vk/ohao_vk_buffer.hpp>
#include <vk/ohao_vk_command_manager.hpp>
#include <vk/ohao_vk_descriptor.hpp>
#include <vk/ohao_vk_device.hpp>
#include <vk/ohao_vk_framebuffer.hpp>
#include <vk/ohao_vk_image.hpp>
#include <vk/ohao_vk_physical_device.hpp>
#include <vk/ohao_vk_pipeline.hpp>
#include <vk/ohao_vk_render_pass.hpp>
#include <vk/ohao_vk_shader_module.hpp>
#include <vk/ohao_vk_surface.hpp>
#include <vk/ohao_vk_swapchain.hpp>
#include <vk/ohao_vk_sync_objects.hpp>
#include <vk/ohao_vk_uniform_buffer.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "renderer/camera/camera.hpp"
#include "core/asset/model.hpp"
#include "core/material/material.hpp"
#include "core/scene/scene.hpp"
#include "renderer/vk/ohao_vk_instance.hpp"

#define GPU_VENDOR_NVIDIA 0
#define GPU_VENDOR_AMD 1
#define GPU_VENDOR_INTEL 2

// Change this to select preferred GPU vendor
#define PREFERRED_GPU_VENDOR GPU_VENDOR_NVIDIA

namespace ohao {

class VulkanContext {
public:
    VulkanContext() = delete;
    VulkanContext(GLFWwindow* windowHandle);
    ~VulkanContext();

    void initializeVulkan();
    void initializeScene();
    void cleanup();

    //Getters
    // === OhaoVk Object Getters ===
    OhaoVkInstance* getInstance() const { return instance.get(); }
    OhaoVkSurface* getSurface() const { return surface.get(); }
    OhaoVkPhysicalDevice* getPhysicalDevice() const { return physicalDevice.get(); }
    OhaoVkDevice* getLogicalDevice() const { return device.get(); }
    OhaoVkSwapChain* getSwapChain() const { return swapchain.get(); }
    OhaoVkShaderModule* getShaderModules() const { return shaderModules.get(); }
    OhaoVkRenderPass* getRenderPass() const { return renderPass.get(); }
    OhaoVkPipeline* getPipeline() const { return pipeline.get(); }
    OhaoVkDescriptor* getDescriptor() const { return descriptor.get(); }
    OhaoVkImage* getDepthImage() const { return depthImage.get(); }
    OhaoVkFramebuffer* getFramebufferManager() const { return framebufferManager.get(); }
    OhaoVkCommandManager* getCommandManager() const { return commandManager.get(); }
    OhaoVkSyncObjects* getSyncObjects() const { return syncObjects.get(); }
    OhaoVkBuffer* getVertexBuffer() const { return vertexBuffer.get(); }
    OhaoVkBuffer* getIndexBuffer() const { return indexBuffer.get(); }
    // === Raw Vulkan Handle Getters ===
    VkInstance getVkInstance() const { return instance ? instance->getInstance() : VK_NULL_HANDLE; }
    VkSurfaceKHR getVkSurface() const { return surface ? surface->getSurface() : VK_NULL_HANDLE; }
    VkPhysicalDevice getVkPhysicalDevice() const { return physicalDevice ? physicalDevice->getDevice() : VK_NULL_HANDLE; }
    VkDevice getVkDevice() const { return device ? device->getDevice() : VK_NULL_HANDLE; }
    VkSwapchainKHR getVkSwapChain() const { return swapchain ? swapchain->getSwapChain() : VK_NULL_HANDLE; }
    VkRenderPass getVkRenderPass() const { return renderPass ? renderPass->getRenderPass() : VK_NULL_HANDLE; }
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

    struct UniformBufferObject{
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec3 viewPos;
        float padding1;

        glm::vec3 lightPos;
        float padding2;
        glm::vec3 lightColor;
        float lightIntensity;

        glm::vec3 baseColor;
        float metallic;
        float roughness;
        float ao;
        float padding3;
        float padding4;
    };

    bool hasLoadScene();
    bool loadModel(const std::string& filename);
    void cleanupCurrentModel();


private:
    GLFWwindow* window;

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
    std::unique_ptr<OhaoVkDescriptor> descriptor;
    std::unique_ptr<OhaoVkImage> depthImage;
    std::unique_ptr<OhaoVkFramebuffer> framebufferManager;
    std::unique_ptr<OhaoVkCommandManager> commandManager;
    std::unique_ptr<OhaoVkSyncObjects> syncObjects;
    std::unique_ptr<OhaoVkBuffer> vertexBuffer;
    std::unique_ptr<OhaoVkBuffer> indexBuffer;
    std::unique_ptr<OhaoVkUniformBuffer> uniformBuffer;
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void createVertexBuffer(const std::vector<Vertex>& vertices);
    void createIndexBuffer(const std::vector<uint32_t>& indices);

    Camera camera;
    std::unique_ptr<Scene> scene;

};

} // namespace ohao
