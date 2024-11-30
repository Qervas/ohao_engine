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

    VkSemaphore getImageAvailableSemaphore(uint32_t frameIndex) const;
    VkSemaphore getRenderFinishedSemaphore(uint32_t frameIndex) const;
    VkFence getInFlightFence(uint32_t frameIndex) const;

    void waitForFence(uint32_t frameIndex) const;
    void resetFence(uint32_t frameIndex) const;

private:
    OhaoVkDevice* device{nullptr};
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t maxFrames{0};

    bool createSyncObjects();
};

} // namespace ohao
