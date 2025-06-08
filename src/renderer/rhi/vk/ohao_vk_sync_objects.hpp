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
    void cleanup();

    // Per-frame semaphores (for frames in flight)
    VkSemaphore getImageAvailableSemaphore(uint32_t frameIndex) const;
    VkSemaphore getRenderFinishedSemaphore(uint32_t frameIndex) const;
    VkFence getInFlightFence(uint32_t frameIndex) const;

    // Per-image semaphores (for swapchain images)
    void createPerImageSemaphores(uint32_t imageCount);
    VkSemaphore getImageAvailableSemaphoreForImage(uint32_t imageIndex) const;
    VkSemaphore getRenderFinishedSemaphoreForImage(uint32_t imageIndex) const;
    bool hasPerImageSemaphores() const { return !perImageAvailableSemaphores.empty(); }

    void waitForFence(uint32_t frameIndex) const;
    void resetFence(uint32_t frameIndex) const;

private:
    OhaoVkDevice* device{nullptr};
    
    // Per-frame synchronization objects
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t maxFrames{0};

    // Per-image semaphores for proper swapchain synchronization
    std::vector<VkSemaphore> perImageAvailableSemaphores;
    std::vector<VkSemaphore> perImageRenderFinishedSemaphores;

    bool createSyncObjects();
    void cleanupPerImageSemaphores();
};

} // namespace ohao
