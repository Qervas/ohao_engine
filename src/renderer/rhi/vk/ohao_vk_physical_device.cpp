#include "ohao_vk_physical_device.hpp"
#include "ohao_vk_instance.hpp"
#include "ohao_vk_surface.hpp"
#include <iostream>
#include <set>
#include <string>
#include <map>

namespace ohao {

bool OhaoVkPhysicalDevice::initialize(OhaoVkInstance* instance,
                                    OhaoVkSurface* surface,
                                    PreferredVendor preferredVendor) {
    if (!instance || !surface) {
        std::cerr << "Null instance or surface provided to OhaoVkPhysicalDevice::initialize" << std::endl;
        return false;
    }

    return selectPhysicalDevice(instance, surface, preferredVendor);
}

bool OhaoVkPhysicalDevice::selectPhysicalDevice(OhaoVkInstance* instance,
                                              OhaoVkSurface* surface,
                                              PreferredVendor preferredVendor) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance->getInstance(), &deviceCount, nullptr);

    if (deviceCount == 0) {
        std::cerr << "Failed to find GPUs with Vulkan support" << std::endl;
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance->getInstance(), &deviceCount, devices.data());

    // Print available devices
    std::cout << "Available physical devices:" << std::endl;
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        std::cout << "  - " << props.deviceName << " (Vendor ID: 0x"
                 << std::hex << props.vendorID << ")" << std::endl;
    }

    // Rate devices and pick the best one
    std::multimap<int, VkPhysicalDevice> candidates;
    for (const auto& device : devices) {
        if (isDeviceSuitable(device, surface, preferredVendor)) {
            int score = rateDeviceSuitability(device, preferredVendor);
            candidates.insert(std::make_pair(score, device));
        }
    }

    if (candidates.empty()) {
        std::cerr << "Failed to find a suitable GPU" << std::endl;
        return false;
    }

    // Select the highest-rated device
    physicalDevice = candidates.rbegin()->second;
    queueFamilyIndices = findQueueFamilies(physicalDevice, surface);

    // Print selected device info
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    std::cout << "Selected physical device: " << props.deviceName << std::endl;

    return true;
}

bool OhaoVkPhysicalDevice::isDeviceSuitable(VkPhysicalDevice device,
                                          OhaoVkSurface* surface,
                                          PreferredVendor preferredVendor) {
    QueueFamilyIndices indices = findQueueFamilies(device, surface);
    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported && surface) {
        auto formats = surface->getFormats(device);
        auto presentModes = surface->getPresentModes(device);
        swapChainAdequate = !formats.empty() && !presentModes.empty();
    }

    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

    return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

int OhaoVkPhysicalDevice::rateDeviceSuitability(VkPhysicalDevice device,
                                               PreferredVendor preferredVendor) const {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceProperties(device, &props);
    vkGetPhysicalDeviceFeatures(device, &features);

    int score = 0;

    // Discrete GPUs have a significant performance advantage
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    }

    // Maximum possible size of textures affects graphics quality
    score += props.limits.maxImageDimension2D;

    // Preferred vendor bonus
    if (static_cast<uint32_t>(preferredVendor) != 0 &&
        props.vendorID == static_cast<uint32_t>(preferredVendor)) {
        score += 2000;
    }

    return score;
}

bool OhaoVkPhysicalDevice::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensionsSet(requiredExtensions.begin(), requiredExtensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensionsSet.erase(extension.extensionName);
    }

    return requiredExtensionsSet.empty();
}

QueueFamilyIndices OhaoVkPhysicalDevice::findQueueFamilies(VkPhysicalDevice device,
                                                          OhaoVkSurface* surface) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilies.size(); i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface->getSurface(), &presentSupport);
        if (presentSupport) {
            indices.presentFamily = i;
        }

        if (indices.isComplete()) {
            break;
        }
    }

    return indices;
}

VkPhysicalDeviceProperties OhaoVkPhysicalDevice::getProperties() const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    return props;
}

VkPhysicalDeviceFeatures OhaoVkPhysicalDevice::getFeatures() const {
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physicalDevice, &features);
    return features;
}

uint32_t OhaoVkPhysicalDevice::findMemoryType(uint32_t typeFilter,
                                            VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}

} // namespace ohao
