#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace ohao {

class OhaoVkInstance {
public:
    OhaoVkInstance() = default;
    ~OhaoVkInstance();

    bool initialize(const std::string& appName, bool enableValidation = true);
    void cleanup();

    VkInstance getInstance() const { return instance; }
    bool isValidationEnabled() const { return validationEnabled; }

private:
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};
    bool validationEnabled{false};

    bool createInstance(const std::string& appName);
    bool setupDebugMessenger();
    bool checkValidationLayerSupport() const;
    std::vector<const char*> getRequiredExtensions() const;

    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) const;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };
};

} // namespace ohao
