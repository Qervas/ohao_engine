#include "ohao_vk_buffer.hpp"
#include "ohao_vk_device.hpp"
#include <iostream>
#include <cstring>

namespace ohao {

OhaoVkBuffer::~OhaoVkBuffer() {
    cleanup();
}

bool OhaoVkBuffer::initialize(OhaoVkDevice* devicePtr) {
    device = devicePtr;
    return true;
}

void OhaoVkBuffer::cleanup() {
    if (device) {
        if (mapped) {
            unmap();
        }
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device->getDevice(), buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device->getDevice(), memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }
}

bool OhaoVkBuffer::create(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties){
    // Create buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device->getDevice(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        std::cerr << "Failed to create buffer!" << std::endl;
        return false;
    }

    // Get memory requirements
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device->getDevice(), buffer, &memRequirements);

    // Allocate memory
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device->getDevice(), &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate buffer memory!" << std::endl;
        return false;
    }

    // Bind buffer memory
    if (vkBindBufferMemory(device->getDevice(), buffer, memory, 0) != VK_SUCCESS) {
        std::cerr << "Failed to bind buffer memory!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkBuffer::createWithStaging(
    OhaoVkDevice* device,
    VkCommandPool commandPool,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    OhaoVkBuffer& buffer){

    // Create staging buffer
    OhaoVkBuffer stagingBuffer;
    stagingBuffer.initialize(device);

    if (!stagingBuffer.create(
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        return false;
    }

    // Copy data to staging buffer
    stagingBuffer.writeToBuffer(data, size);

    // Create device local buffer
    if (!buffer.create(
        size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }

    // Copy from staging to device local buffer
    if (!copyBuffer(device, commandPool,
        stagingBuffer.getBuffer(), buffer.getBuffer(), size)) {
        return false;
    }

    // Cleanup staging buffer
    stagingBuffer.cleanup();

    return true;
}

bool OhaoVkBuffer::copyBuffer(OhaoVkDevice* device,VkCommandPool commandPool,
    VkBuffer srcBuffer, VkBuffer dstBuffer,VkDeviceSize size){

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(device->getDevice(), &allocInfo, &commandBuffer) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(device->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->getGraphicsQueue());

    vkFreeCommandBuffers(device->getDevice(), commandPool, 1, &commandBuffer);

    return true;
}

bool OhaoVkBuffer::map(VkDeviceSize size, VkDeviceSize offset) {
    if (mapped) {
        return true;  // Already mapped
    }

    if (vkMapMemory(device->getDevice(), memory, offset, size, 0, &mapped) != VK_SUCCESS) {
        std::cerr << "Failed to map buffer memory!" << std::endl;
        return false;
    }

    return true;
}

void OhaoVkBuffer::unmap() {
    if (mapped) {
        vkUnmapMemory(device->getDevice(), memory);
        mapped = nullptr;
    }
}

void OhaoVkBuffer::writeToBuffer(const void* data, VkDeviceSize size) {
    if (!mapped) {
        if (!map(size)) {
            throw std::runtime_error("Failed to map buffer for writing!");
        }
    }
    std::memcpy(mapped, data, size);
}

uint32_t OhaoVkBuffer::findMemoryType(
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties) const{

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(device->getPhysicalDevice()->getDevice(), &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

} // namespace ohao
