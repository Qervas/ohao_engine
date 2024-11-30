#pragma once
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <vk/ohao_vk_buffer.hpp>
#include <vk/ohao_vk_command_manager.hpp>
#include <vk/ohao_vk_descriptor.hpp>
#include <vk/ohao_vk_device.hpp>
#include <vk/ohao_vk_physical_device.hpp>
#include <vk/ohao_vk_pipeline.hpp>
#include <vk/ohao_vk_render_pass.hpp>
#include <vk/ohao_vk_shader_module.hpp>
#include <vk/ohao_vk_surface.hpp>
#include <vk/ohao_vk_swapchain.hpp>
#include <vk/ohao_vk_sync_objects.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "../core/camera.hpp"
#include "../core/model.hpp"
#include "../core/material.hpp"
#include "../core/scene.hpp"
#include "vk/ohao_vk_instance.hpp"

#define GPU_VENDOR_NVIDIA 0
#define GPU_VENDOR_AMD 1
#define GPU_VENDOR_INTEL 2

// Change this to select preferred GPU vendor
#define PREFERRED_GPU_VENDOR GPU_VENDOR_NVIDIA
#define WIDTH 1024
#define HEIGHT 768

namespace ohao {

class VulkanContext {
public:
    VulkanContext();
    VulkanContext(GLFWwindow* windowHandle);
    ~VulkanContext();

    bool initialize();
    void cleanup();

    VkDevice getDevice()const{return device->getDevice();}
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

    Camera& getCamera() {return camera;}

private:
    GLFWwindow* window;
    std::unique_ptr<OhaoVkInstance> instance;
    std::unique_ptr<OhaoVkSurface> surface;
    std::unique_ptr<OhaoVkPhysicalDevice> physicalDevice;
    std::unique_ptr<OhaoVkDevice> device;
    //Queue handles
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue presentQueue{VK_NULL_HANDLE};
    std::unique_ptr<OhaoVkSwapChain> swapchain;
    uint32_t width{WIDTH}, height{HEIGHT};
    std::unique_ptr<OhaoVkShaderModule> shaderModules;

    //pipeline
    std::unique_ptr<OhaoVkRenderPass> renderPass;
    std::unique_ptr<OhaoVkPipeline> pipeline;

    std::unique_ptr<OhaoVkDescriptor> descriptor;

    //framebuffers
    std::vector<VkFramebuffer> swapChainFrameBuffers;
    std::unique_ptr<OhaoVkCommandManager> commandManager;

    std::unique_ptr<OhaoVkSyncObjects> syncObjects;
    const int MAX_FRAMES_IN_FLIGHT = 2;
    size_t currentFrame = 0;

    void createFramebuffers();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);

    std::unique_ptr<OhaoVkBuffer> vertexBuffer;
    std::unique_ptr<OhaoVkBuffer> indexBuffer;
    std::vector<std::unique_ptr<OhaoVkBuffer>> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;

    Camera camera;

    void createUniformBuffers();
    void updateUniformBuffer(uint32_t currentImage);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);



    void createVertexBuffer(const std::vector<Vertex>& vertices);
    void createIndexBuffer(const std::vector<uint32_t>& indices);

    void updateMaterial(const Material& material);
    void updateLight(const glm::vec3& position, const glm::vec3& color, float intensity);

    std::unique_ptr<Scene> scene;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;
    void createDepthResources();
    VkFormat findDepthFormat();

    void createImage(uint32_t width, uint32_t height, VkFormat format,
                        VkImageTiling tiling, VkImageUsageFlags usage,
                        VkMemoryPropertyFlags properties, VkImage& image,
                        VkDeviceMemory& imageMemory, VkSampleCountFlagBits mssaSamples);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
};

} // namespace ohao
