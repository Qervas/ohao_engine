#pragma once
#include <vulkan/vulkan.h>
#include <optional>
#include <vector>

namespace ohao {

class OhaoVkInstance;
class OhaoVkSurface;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

class OhaoVkPhysicalDevice {
public:
    enum class PreferredVendor {
        NVIDIA = 0x10DE,
        AMD = 0x1002,
        INTEL = 0x8086,
        ANY = 0
    };

    OhaoVkPhysicalDevice() = default;
    ~OhaoVkPhysicalDevice() = default;

    bool initialize(OhaoVkInstance* instance,
                   OhaoVkSurface* surface,
                   PreferredVendor preferredVendor = PreferredVendor::ANY);

    VkPhysicalDevice getDevice() const { return physicalDevice; }
    const QueueFamilyIndices& getQueueFamilyIndices() const { return queueFamilyIndices; }

    // Device properties and features
    VkPhysicalDeviceProperties getProperties() const;
    VkPhysicalDeviceFeatures getFeatures() const;
    VkSampleCountFlagBits getMaxUsableSampleCount() const;
    auto&& getRequiredExtensions() const {return requiredExtensions;}
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    // Device support queries
    bool isFormatSupported(VkFormat format, VkImageTiling tiling, VkFormatFeatureFlags features) const;
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                VkImageTiling tiling,
                                VkFormatFeatureFlags features) const;

private:
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    QueueFamilyIndices queueFamilyIndices;

    bool selectPhysicalDevice(OhaoVkInstance* instance,
                            OhaoVkSurface* surface,
                            PreferredVendor preferredVendor);
    bool isDeviceSuitable(VkPhysicalDevice device,
                         OhaoVkSurface* surface,
                         PreferredVendor preferredVendor);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, OhaoVkSurface* surface) const;
    int rateDeviceSuitability(VkPhysicalDevice device, PreferredVendor preferredVendor) const;

    const std::vector<const char*> requiredExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
};

} // namespace ohao
