#include "ohao_vk_instance.hpp"
#include <GLFW/glfw3.h>
#include <iostream>
#include <cstring>

// Portability enumeration extension for MoltenVK on macOS
#ifndef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

namespace ohao {

OhaoVkInstance::~OhaoVkInstance() {
    cleanup();
}

bool OhaoVkInstance::initialize(const std::string& appName, bool enableValidation) {
    validationEnabled = enableValidation;
    return createInstance(appName);
}

void OhaoVkInstance::cleanup() {
    if (instance) {
        if (validationEnabled && debugMessenger) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func) {
                func(instance, debugMessenger, nullptr);
            }
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

bool OhaoVkInstance::createInstance(const std::string& appName) {
    // Graceful fallback if validation layers not available (common on macOS)
    if (validationEnabled && !checkValidationLayerSupport()) {
        std::cerr << "Warning: Validation layers requested but not available. Continuing without validation.\n";
        validationEnabled = false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = appName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "OHAO Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    // Use Vulkan 1.2 on macOS (MoltenVK), 1.3 on Linux
#ifdef __APPLE__
    appInfo.apiVersion = VK_API_VERSION_1_2;
#else
    appInfo.apiVersion = VK_API_VERSION_1_3;
#endif

    auto extensions = getRequiredExtensions();
    if (extensions.empty()) {
        std::cerr << "Failed to get required Vulkan extensions\n";
        return false;
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

#ifdef __APPLE__
    // Required for MoltenVK
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (validationEnabled) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();

        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext = &debugCreateInfo;
    } else {
        createInfo.enabledLayerCount = 0;
        createInfo.pNext = nullptr;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance\n";
        return false;
    }

    if (validationEnabled) {
        return setupDebugMessenger();
    }

    return true;
}

bool OhaoVkInstance::setupDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");

    if (!func) {
        std::cerr << "Failed to get debug messenger creation function\n";
        return false;
    }

    if (func(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
        std::cerr << "Failed to create debug messenger\n";
        return false;
    }

    return true;
}

bool OhaoVkInstance::checkValidationLayerSupport() const {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : validationLayers) {
        bool layerFound = false;
        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }
        if (!layerFound) return false;
    }

    return true;
}

std::vector<const char*> OhaoVkInstance::getRequiredExtensions() const {
    // Check if GLFW supports Vulkan
    if (!glfwVulkanSupported()) {
        std::cerr << "Error: GLFW reports Vulkan is not supported on this system.\n";
        std::cerr << "Make sure you have Vulkan drivers installed (or MoltenVK on macOS).\n";
        return {};
    }

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    if (!glfwExtensions) {
        std::cerr << "Error: Failed to get required Vulkan extensions from GLFW.\n";
        return {};
    }

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (validationEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

#ifdef __APPLE__
    // Required for MoltenVK portability
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    return extensions;
}

void OhaoVkInstance::populateDebugMessengerCreateInfo(
    VkDebugUtilsMessengerCreateInfoEXT& createInfo) const {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    createInfo.pUserData = nullptr;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OhaoVkInstance::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    std::cerr << "Validation layer: " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

} // namespace ohao
