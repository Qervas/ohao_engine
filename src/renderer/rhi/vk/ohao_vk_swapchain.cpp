#include "ohao_vk_swapchain.hpp"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <vulkan/vulkan_core.h>

namespace ohao {

OhaoVkSwapChain::~OhaoVkSwapChain() {
    cleanup();
}

bool OhaoVkSwapChain::initialize(OhaoVkDevice* device,
                                OhaoVkSurface* surface,
                                uint32_t width,
                                uint32_t height) {
    this->device = device;
    this->surface = surface;

    try {
        createSwapChain(width, height);
        createImageViews();
        setupPresentInfo();
    } catch (const std::exception& e) {
        std::cerr << "Failed to create swap chain: " << e.what() << std::endl;
        cleanup();
        return false;
    }

    return true;
}

void OhaoVkSwapChain::cleanup() {
    if (device) {
        for (auto imageView : imageViews) {
            vkDestroyImageView(device->getDevice(), imageView, nullptr);
        }
        imageViews.clear();

        if (swapChain) {
            vkDestroySwapchainKHR(device->getDevice(), swapChain, nullptr);
            swapChain = VK_NULL_HANDLE;
        }
    }
}

void OhaoVkSwapChain::createSwapChain(uint32_t width, uint32_t height) {
    SwapChainSupportDetails swapChainSupport = querySwapChainSupport();

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
    VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
    VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, width, height);

    // Choose image count
    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, swapChainSupport.capabilities.maxImageCount);
    }

    // Create swap chain info
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface->getSurface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    // Handle queue family indices
    const auto& indices = device->getQueueFamilyIndices();
    if (indices.graphicsFamily != indices.presentFamily) {
        uint32_t queueFamilyIndices[] = {
            indices.graphicsFamily.value(),
            indices.presentFamily.value()
        };
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices = nullptr;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapChain;

    if (vkCreateSwapchainKHR(device->getDevice(), &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swap chain!");
    }

    // Get swap chain images
    vkGetSwapchainImagesKHR(device->getDevice(), swapChain, &imageCount, nullptr);
    images.resize(imageCount);
    vkGetSwapchainImagesKHR(device->getDevice(), swapChain, &imageCount, images.data());

    imageFormat = surfaceFormat.format;
    this->extent = extent;
}

void OhaoVkSwapChain::createImageViews() {
    imageViews.resize(images.size());

    for (size_t i = 0; i < images.size(); i++) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = images[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = imageFormat;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device->getDevice(), &createInfo, nullptr, &imageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image views!");
        }
    }
}

SwapChainSupportDetails OhaoVkSwapChain::querySwapChainSupport() const {
    SwapChainSupportDetails details;
    VkPhysicalDevice physicalDevice = device->getPhysicalDevice()->getDevice();

    // Get capabilities
    details.capabilities = surface->getCapabilities(physicalDevice);

    // Log capabilities
    std::cout << "Swap chain capabilities:" << std::endl;
    std::cout << "\tminImageCount: " << details.capabilities.minImageCount << std::endl;
    std::cout << "\tmaxImageCount: " << details.capabilities.maxImageCount << std::endl;

    // Get formats
    details.formats = surface->getFormats(physicalDevice);

    // Get present modes
    details.presentModes = surface->getPresentModes(physicalDevice);

    return details;
}

VkSurfaceFormatKHR OhaoVkSwapChain::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

VkPresentModeKHR OhaoVkSwapChain::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& availablePresentModes) {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D OhaoVkSwapChain::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    uint32_t width,
    uint32_t height) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D actualExtent = {width, height};

    actualExtent.width = std::clamp(actualExtent.width,
        capabilities.minImageExtent.width,
        capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height,
        capabilities.minImageExtent.height,
        capabilities.maxImageExtent.height);

    return actualExtent;
}

void OhaoVkSwapChain::setupPresentInfo() {
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapChain;
}

void OhaoVkSwapChain::updatePresentInfo(VkSemaphore waitSemaphore, uint32_t* pImageIndex) {
    presentInfo.pWaitSemaphores = &waitSemaphore;
    presentInfo.pImageIndices = pImageIndex;
}

bool OhaoVkSwapChain::recreate(uint32_t width, uint32_t height){
    oldSwapChain = swapChain;

    for(auto imageView: imageViews){
        vkDestroyImageView(device->getDevice(), imageView, nullptr);
    }
    imageViews.clear();
    images.clear();

    try{
        createSwapChain(width, height);
        createImageViews();
        setupPresentInfo();
    }catch(const std::exception& e){
        std::cerr << "failed to recreate swapchain:" << e.what() << std::endl;
        return false;
    }
    if (oldSwapChain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device->getDevice(), oldSwapChain, nullptr);
        oldSwapChain = VK_NULL_HANDLE;
    }
    return true;
}

} // namespace ohao
