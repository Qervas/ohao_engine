#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "ohao_vk_device.hpp"
#include "ohao_vk_surface.hpp"

namespace ohao {

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class OhaoVkSwapChain {
public:
    OhaoVkSwapChain() = default;
    ~OhaoVkSwapChain();

    bool initialize(OhaoVkDevice* device,
                   OhaoVkSurface* surface,
                   uint32_t width,
                   uint32_t height);
    void cleanup();

    // Getters
    VkSwapchainKHR getSwapChain() const { return swapChain; }
    VkFormat getImageFormat() const { return imageFormat; }
    VkExtent2D getExtent() const { return extent; }
    const std::vector<VkImageView>& getImageViews() const { return imageViews; }
    const std::vector<VkImage>& getImages() const { return images; }

    // Support query
    SwapChainSupportDetails querySwapChainSupport() const;

    bool recreate(uint32_t width, uint32_t height);
    VkSwapchainKHR getOldSwapChain() const {return oldSwapChain;}

private:
    void createSwapChain(uint32_t width, uint32_t height);
    void createImageViews();

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities,
        uint32_t width,
        uint32_t height);
    void setupPresentInfo();
    void updatePresentInfo(VkSemaphore waitSemaphore, uint32_t* pImageIndex);


    OhaoVkDevice* device{nullptr};
    OhaoVkSurface* surface{nullptr};

    VkSwapchainKHR swapChain{VK_NULL_HANDLE};
    VkSwapchainKHR oldSwapChain{VK_NULL_HANDLE};

    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    VkFormat imageFormat;
    VkExtent2D extent;

    VkPresentInfoKHR presentInfo{};

};

} // namespace ohao
