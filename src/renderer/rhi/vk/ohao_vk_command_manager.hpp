#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace ohao {

class OhaoVkDevice;

class OhaoVkCommandManager {
public:
    OhaoVkCommandManager() = default;
    ~OhaoVkCommandManager();

    bool initialize(OhaoVkDevice* device, uint32_t queueFamilyIndex);
    void cleanup();

    // Command pool management
    VkCommandPool getCommandPool() const { return commandPool; }

    // Command buffer management
    bool allocateCommandBuffers(uint32_t count);
    void freeCommandBuffers();
    VkCommandBuffer getCommandBuffer(uint32_t index) const;
    const VkCommandBuffer* getCommandBufferPtr(uint32_t index) const;
    void resetCommandBuffer(uint32_t index);

    // Helper functions for common command buffer operations
    VkCommandBuffer beginSingleTime();
    void endSingleTime(VkCommandBuffer commandBuffer);

    //submit info
    const VkSubmitInfo* getSubmitInfo() const { return &submitInfo; }
    void updateSubmitInfo(VkSemaphore waitSemaphore,
                         VkSemaphore signalSemaphore,
                         VkCommandBuffer commandBuffer);

private:
    OhaoVkDevice* device{nullptr};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> commandBuffers;

    VkSubmitInfo submitInfo{};
    VkPipelineStageFlags waitStages{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

    bool createCommandPool(uint32_t queueFamilyIndex);
    void setupSubmitInfo();
};

} // namespace ohao
