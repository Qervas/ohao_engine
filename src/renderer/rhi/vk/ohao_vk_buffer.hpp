#pragma once
#include <vulkan/vulkan.h>
#include <cstdio>

namespace ohao {

class OhaoVkDevice;

class OhaoVkBuffer {
public:
    OhaoVkBuffer() = default;
    ~OhaoVkBuffer();

    bool initialize(OhaoVkDevice* device);
    void cleanup();

    bool create(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties
    );

    static bool copyBuffer(
        OhaoVkDevice* device,
        VkCommandPool commandPool,
        VkBuffer srcBuffer,
        VkBuffer dstBuffer,
        VkDeviceSize size
    );

    bool map(VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0);
    void unmap();
    void writeToBuffer(const void* data, VkDeviceSize size);

    // Staging buffer helper
    static bool createWithStaging(
        OhaoVkDevice* device,
        VkCommandPool commandPool,
        const void* data,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        OhaoVkBuffer& buffer
    );

    // Safe getter for buffer handle - checks if valid and logs errors
    VkBuffer getBuffer() const { 
        if (buffer == VK_NULL_HANDLE) {
            // Use fprintf instead of logging in case logger depends on this
            fprintf(stderr, "Warning: Attempting to get a null VkBuffer handle\n");
        }
        return buffer; 
    }
    
    VkDeviceMemory getMemory() const { return memory; }
    bool isMapped() const { return mapped != nullptr; }
    void* getMappedMemory() const { return mapped; }
    bool isValid() const { return buffer != VK_NULL_HANDLE && memory != VK_NULL_HANDLE; }

private:
    OhaoVkDevice* device{nullptr};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void* mapped{nullptr};

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
};

} // namespace ohao
