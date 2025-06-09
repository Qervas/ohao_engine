#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace ohao {

class OhaoVkDevice;

class OhaoVkSyncObjects {
public:
    OhaoVkSyncObjects() = default;
    ~OhaoVkSyncObjects();

    bool initialize(OhaoVkDevice* device, uint32_t maxFramesInFlight);
    bool initializeSwapchainSemaphores(uint32_t swapchainImageCount);
    void cleanup();

    VkSemaphore getImageAvailableSemaphore(uint32_t frameIndex) const;
    VkSemaphore getRenderFinishedSemaphore(uint32_t frameIndex) const;
    VkSemaphore getSwapchainImageAvailableSemaphore(uint32_t imageIndex) const;
    VkSemaphore getSwapchainRenderFinishedSemaphore(uint32_t imageIndex) const;
    VkFence getInFlightFence(uint32_t frameIndex) const;

    void waitForFence(uint32_t frameIndex) const;
    void resetFence(uint32_t frameIndex) const;

private:
    OhaoVkDevice* device{nullptr};
    
    // Per-frame semaphores (for frames in flight)
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t maxFrames{0};
    
    // Per-swapchain-image semaphores (for swapchain images)
    std::vector<VkSemaphore> swapchainImageAvailableSemaphores;
    std::vector<VkSemaphore> swapchainRenderFinishedSemaphores;
    uint32_t swapchainImageCount{0};

    bool createSyncObjects();
    bool createSwapchainSemaphores();
};

} // namespace ohao
