#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include "ohao_vk_physical_device.hpp"

namespace ohao {

class OhaoVkDevice {
public:
    OhaoVkDevice() = default;
    ~OhaoVkDevice();

    bool initialize(OhaoVkPhysicalDevice* physicalDevice,
                   const std::vector<const char*>& validationLayers);
    void cleanup();

    // Core getters
    VkDevice getDevice() const { return device; }
    VkQueue getGraphicsQueue() const { return graphicsQueue; }
    VkQueue getPresentQueue() const { return presentQueue; }
    const QueueFamilyIndices& getQueueFamilyIndices() const {return queueFamilyIndices;}
    OhaoVkPhysicalDevice* getPhysicalDevice() const {return physicalDevice;}

    // Device operations
    void waitIdle() const;

    // Memory management helpers
    VkResult allocateBuffer(VkDeviceSize size,
                           VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkBuffer& buffer,
                           VkDeviceMemory& bufferMemory);

    void freeBuffer(VkBuffer buffer, VkDeviceMemory bufferMemory);

    // Command operations
    VkCommandBuffer beginSingleTimeCommands(VkCommandPool commandPool) const;
    void endSingleTimeCommands(VkCommandPool commandPool, VkCommandBuffer commandBuffer) const;

private:
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue presentQueue{VK_NULL_HANDLE};

    // Non-owning pointer to physical device
    QueueFamilyIndices queueFamilyIndices;
    OhaoVkPhysicalDevice* physicalDevice{nullptr};

    bool createLogicalDevice(const std::vector<const char*>& validationLayers);
    void setupQueues();
};

} // namespace ohao
