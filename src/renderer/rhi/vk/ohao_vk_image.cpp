#include "ohao_vk_image.hpp"
#include "ohao_vk_device.hpp"
#include <iostream>
#include <cstring>

namespace ohao {

OhaoVkImage::~OhaoVkImage() {
    cleanup();
}

bool OhaoVkImage::initialize(OhaoVkDevice* devicePtr) {
    device = devicePtr;
    return true;
}

void OhaoVkImage::cleanup() {
    if (device) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device->getDevice(), imageView, nullptr);
            imageView = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device->getDevice(), image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (imageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device->getDevice(), imageMemory, nullptr);
            imageMemory = VK_NULL_HANDLE;
        }
    }
}

bool OhaoVkImage::createImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageTiling tiling,
    VkImageUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkSampleCountFlagBits numSamples)
{
    this->width = width;
    this->height = height;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = numSamples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device->getDevice(), &imageInfo, nullptr, &image) != VK_SUCCESS) {
        std::cerr << "Failed to create image!" << std::endl;
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device->getDevice(), image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device->getDevice(), &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate image memory!" << std::endl;
        return false;
    }

    vkBindImageMemory(device->getDevice(), image, imageMemory, 0);
    return true;
}

bool OhaoVkImage::createImageView(VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device->getDevice(), &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        std::cerr << "Failed to create texture image view!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkImage::createDepthResources(VkExtent2D extent, VkSampleCountFlagBits msaaSamples) {
    width = extent.width;
    height = extent.height;
    VkFormat depthFormat = findDepthFormat(device);

    if (!createImage(
        extent.width, extent.height,
        depthFormat,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        msaaSamples)) {
        std::cerr << "Failed to create depth image!" << std::endl;
        return false;
    }

    if (!createImageView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        std::cerr << "Failed to create depth image view!" << std::endl;
        return false;
    }

    return true;
}

VkFormat OhaoVkImage::findDepthFormat(OhaoVkDevice* device) {
    const std::vector<VkFormat> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(
            device->getPhysicalDevice()->getDevice(),
            format,
            &props
        );

        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }

    throw std::runtime_error("Failed to find supported depth format!");
}

bool OhaoVkImage::hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT;
}

uint32_t OhaoVkImage::findMemoryType(
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(
        device->getPhysicalDevice()->getDevice(),
        &memProperties
    );

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

bool OhaoVkImage::createTextureImage(OhaoVkDevice* devicePtr, int width, int height, unsigned char* data) {
    device = devicePtr;
    this->width = static_cast<uint32_t>(width);
    this->height = static_cast<uint32_t>(height);
    
    VkDeviceSize imageSize = width * height * 4; // RGBA
    
    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    if (device->allocateBuffer(
        imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingBufferMemory) != VK_SUCCESS) {
        return false;
    }
    
    // Copy image data to staging buffer
    void* mappedData;
    vkMapMemory(device->getDevice(), stagingBufferMemory, 0, imageSize, 0, &mappedData);
    memcpy(mappedData, data, static_cast<size_t>(imageSize));
    vkUnmapMemory(device->getDevice(), stagingBufferMemory);
    
    // Create the actual image
    if (!createImage(
        width, height,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        device->freeBuffer(stagingBuffer, stagingBufferMemory);
        return false;
    }
    
    // Transition image layout and copy from staging buffer
    // Note: This is a simplified version - a full implementation would use command pools
    
    // Clean up staging buffer
    device->freeBuffer(stagingBuffer, stagingBufferMemory);
    
    return true;
}

} // namespace ohao
