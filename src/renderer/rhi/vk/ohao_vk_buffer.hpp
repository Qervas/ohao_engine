#pragma once
#include <vulkan/vulkan.h>

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

    VkBuffer getBuffer() const { return buffer; }
    VkDeviceMemory getMemory() const { return memory; }
    bool isMapped() const { return mapped != nullptr; }
    void* getMappedMemory() const { return mapped; }

private:
    OhaoVkDevice* device{nullptr};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void* mapped{nullptr};

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
};

} // namespace ohao
