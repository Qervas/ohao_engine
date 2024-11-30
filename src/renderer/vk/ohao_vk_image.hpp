#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

namespace ohao {

class OhaoVkDevice;

class OhaoVkImage {
public:
    OhaoVkImage() = default;
    ~OhaoVkImage();

    bool initialize(OhaoVkDevice* device);
    void cleanup();

    // Image creation
    bool createImage(
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageTiling tiling,
        VkImageUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkSampleCountFlagBits numSamples = VK_SAMPLE_COUNT_1_BIT
    );

    // Image view creation
    bool createImageView(
        VkFormat format,
        VkImageAspectFlags aspectFlags
    );

    bool createDepthResources(VkExtent2D extent, VkSampleCountFlagBits msaaSamples);

    // Depth buffer specific functions
    static VkFormat findDepthFormat(OhaoVkDevice* device);
    static bool hasStencilComponent(VkFormat format);

    // Getters
    VkImage getImage() const { return image; }
    VkImageView getImageView() const { return imageView; }
    VkDeviceMemory getImageMemory() const { return imageMemory; }

private:
    OhaoVkDevice* device{nullptr};
    VkImage image{VK_NULL_HANDLE};
    VkImageView imageView{VK_NULL_HANDLE};
    VkDeviceMemory imageMemory{VK_NULL_HANDLE};

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
};

} // namespace ohao
