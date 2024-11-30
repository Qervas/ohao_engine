#include "ohao_vk_command_manager.hpp"
#include "ohao_vk_device.hpp"
#include <iostream>
#include <stdexcept>

namespace ohao {

OhaoVkCommandManager::~OhaoVkCommandManager() {
    cleanup();
}

bool OhaoVkCommandManager::initialize(OhaoVkDevice* devicePtr, uint32_t queueFamilyIndex) {
    device = devicePtr;
    return createCommandPool(queueFamilyIndex);
}

void OhaoVkCommandManager::cleanup() {
    if (device) {
        freeCommandBuffers();
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device->getDevice(), commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
        }
    }
}

bool OhaoVkCommandManager::createCommandPool(uint32_t queueFamilyIndex) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;

    if (vkCreateCommandPool(device->getDevice(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        std::cerr << "Failed to create command pool!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkCommandManager::allocateCommandBuffers(uint32_t count) {
    commandBuffers.resize(count);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = count;

    if (vkAllocateCommandBuffers(device->getDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        std::cerr << "Failed to allocate command buffers!" << std::endl;
        return false;
    }

    return true;
}

void OhaoVkCommandManager::freeCommandBuffers() {
    if (!commandBuffers.empty() && device) {
        vkFreeCommandBuffers(
            device->getDevice(),
            commandPool,
            static_cast<uint32_t>(commandBuffers.size()),
            commandBuffers.data()
        );
        commandBuffers.clear();
    }
}

VkCommandBuffer OhaoVkCommandManager::getCommandBuffer(uint32_t index) const {
    if (index >= commandBuffers.size()) {
        throw std::runtime_error("Command buffer index out of range");
    }
    return commandBuffers[index];
}

const VkCommandBuffer* OhaoVkCommandManager::getCommandBufferPtr(uint32_t index) const{
    if (index >= commandBuffers.size()) {
         throw std::runtime_error("Command buffer index out of range");
     }
     return &commandBuffers[index];
}

void OhaoVkCommandManager::resetCommandBuffer(uint32_t index) {
    if (index >= commandBuffers.size()) {
        throw std::runtime_error("Command buffer index out of range");
    }
    vkResetCommandBuffer(commandBuffers[index], 0);
}

VkCommandBuffer OhaoVkCommandManager::beginSingleTime() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device->getDevice(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void OhaoVkCommandManager::endSingleTime(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(device->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->getGraphicsQueue());

    vkFreeCommandBuffers(device->getDevice(), commandPool, 1, &commandBuffer);
}

} // namespace ohao
