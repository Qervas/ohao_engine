#pragma once
#include <vulkan/vulkan.hpp>
#include <vector>
#include <string>

namespace ohao {

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    bool initialize();
    void cleanup();
    void setupDebugMessenger();//validation layer


private:
    VkInstance instance;
    void createInstance();
    bool checkValidationLayerSupport();
    std::vector<const char*> getRequiredExtensions();

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };


    //debug
#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    const bool enableValidationLayers = true;
#endif

    VkDebugUtilsMessengerEXT debugMessenger;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);

    VkResult CreateDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger);

    void DestroyDebugUtilsMessengerEXT(
        VkInstance instance,
        VkDebugUtilsMessengerEXT debugMessenger,
        const VkAllocationCallbacks* pAllocator);

    void populateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT& createInfo);
};

} // namespace ohao
