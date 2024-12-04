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

    VkDevice getDevice()const{return device->getDevice();}
    OhaoVkUniformBuffer* getUniformBuffer() const {return uniformBuffer.get();}
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
