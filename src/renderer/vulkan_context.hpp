#pragma once
#include <algorithm>
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <vk/ohao_vk_surface.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <string>
#include <vulkan/vulkan_core.h>
#include <unordered_map>
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

namespace ohao {

class VulkanContext {
public:
    VulkanContext();
    VulkanContext(GLFWwindow* windowHandle);
    ~VulkanContext();

    bool initialize();
    void cleanup();

    VkDevice getDevice()const{return device;}
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

    //Device
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};

    //Queue handles
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue presentQueue{VK_NULL_HANDLE};

    struct QueueFamilyIndices{
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;

        bool isComplete(){
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    void pickPhysicalDevice();
    void createLogicalDevice();

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    bool isDeviceSuitable(VkPhysicalDevice device);

    //Swap Chain
    VkSwapchainKHR swapChain{VK_NULL_HANDLE};
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImageView> swapChainImageViews;

    struct SwapChainSupportDetails{
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    void createSwapChain();
    void createImageViews();

    bool checkDeviceExtensionSupport(VkPhysicalDevice);
    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    //shader
    enum class ShaderType{
        VERTEX,
        FRAGMENT,
        COMPUTE,
        GEOMETRY,
        TESSELLATION_CONTROL,
        TESSELLATION_EVALUATION
    };

    struct ShaderModule{
        VkShaderModule modules{VK_NULL_HANDLE};
        ShaderType type;
        std::string entryPoint{"main"};
    };

    std::unordered_map<std::string, ShaderModule> shaderModules;

    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readShaderFile(const std::string& filename);
    ShaderModule* createShaderFromFile(const std::string& filename, ShaderType type);
    void destroyShaderModule(const std::string& name);

    //pipeline
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VkPipeline graphicsPipeline{VK_NULL_HANDLE};

    void createRenderPass();
    void createGraphicsPipeline();
    void createPipelineLayout();

    //framebuffers
    std::vector<VkFramebuffer> swapChainFrameBuffers;

    VkCommandPool commandPool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    const int MAX_FRAMES_IN_FLIGHT = 2;
    size_t currentFrame = 0;

    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);

    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> descriptorSets;
    Camera camera;

    void createDescriptorSetLayout();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSets();
    void updateUniformBuffer(uint32_t currentImage);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    VkBuffer vertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory vertexBufferMemory{VK_NULL_HANDLE};
    VkBuffer indexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory indexBufferMemory{VK_NULL_HANDLE};


    void createVertexBuffer(const std::vector<Vertex>& vertices);
    void createIndexBuffer(const std::vector<uint32_t>& indices);
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags propertiesm, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

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
