#include "ohao_vk_device.hpp"
#include <iostream>
#include <set>
#include <string>

namespace ohao {

OhaoVkDevice::~OhaoVkDevice() {
    cleanup();
}

bool OhaoVkDevice::initialize(OhaoVkPhysicalDevice* phyDevice,
                            const std::vector<const char*>& validationLayers) {
    if (!phyDevice) {
        std::cerr << "Null physical device provided to OhaoVkDevice::initialize" << std::endl;
        return false;
    }

    physicalDevice = phyDevice;
    queueFamilyIndices = physicalDevice->getQueueFamilyIndices();

    if (!createLogicalDevice(validationLayers)) {
        return false;
    }

    // Verify queues after creation
    try {
        setupQueues();
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to setup queues: " << e.what() << std::endl;
        cleanup();
        return false;
    }

    return true;
}

void OhaoVkDevice::cleanup() {
    if (device) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        graphicsQueue = VK_NULL_HANDLE;
        presentQueue = VK_NULL_HANDLE;
    }
}

bool OhaoVkDevice::createLogicalDevice(const std::vector<const char*>& validationLayers) {
    const QueueFamilyIndices& indices = physicalDevice->getQueueFamilyIndices();

    // Create unique queue families set
    std::set<uint32_t> uniqueQueueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    // Create queue create infos
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    float queuePriority = 1.0f;

    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    auto deviceFeatures = physicalDevice->getEnabledFeatures();
    // Create the logical device
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    // Add required extensions
    auto requiredExtensions = physicalDevice->getRequiredExtensions();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    createInfo.ppEnabledExtensionNames = requiredExtensions.data();

    // Add validation layers if enabled
    if (!validationLayers.empty()) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    } else {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateDevice(physicalDevice->getDevice(), &createInfo, nullptr, &device) != VK_SUCCESS) {
        std::cerr << "Failed to create logical device" << std::endl;
        return false;
    }

    std::cout << "Logical device created successfully" << std::endl;
    return true;
}

void OhaoVkDevice::waitIdle() const {
    vkDeviceWaitIdle(device);
}

VkResult OhaoVkDevice::allocateBuffer(VkDeviceSize size,
                                    VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags properties,
                                    VkBuffer& buffer,
                                    VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = physicalDevice->findMemoryType(
        memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
    return VK_SUCCESS;
}

void OhaoVkDevice::freeBuffer(VkBuffer buffer, VkDeviceMemory bufferMemory) {
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
    }
    if (bufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, bufferMemory, nullptr);
    }
}

VkCommandBuffer OhaoVkDevice::beginSingleTimeCommands(VkCommandPool commandPool) const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void OhaoVkDevice::endSingleTimeCommands(VkCommandPool commandPool,
                                       VkCommandBuffer commandBuffer) const {
    if (commandBuffer == VK_NULL_HANDLE) {
        throw std::runtime_error("Invalid command buffer");
    }

    VkResult result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to end command buffer");
    }

    // Create a fence for synchronization
    VkFence fence;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fence");
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    // Additional validation
    if (graphicsQueue == VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
        throw std::runtime_error("Graphics queue is invalid");
    }

    std::cout << "Submitting command buffer to graphics queue" << std::endl;
    result = vkQueueSubmit(graphicsQueue, 1, &submitInfo, fence);

    if (result != VK_SUCCESS) {
        vkDestroyFence(device, fence, nullptr);
        throw std::runtime_error("Failed to submit command buffer to queue: " + std::to_string(result));
    }

    // Wait for the fence
    result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to wait for fence");
    }

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void OhaoVkDevice::setupQueues() {
    // Use stored queue family indices
    vkGetDeviceQueue(device, queueFamilyIndices.graphicsFamily.value(), 0, &graphicsQueue);
    vkGetDeviceQueue(device, queueFamilyIndices.presentFamily.value(), 0, &presentQueue);

    std::cout << "Setting up queues with indices:" << std::endl;
    std::cout << "Graphics Queue Family Index: " << queueFamilyIndices.graphicsFamily.value() << std::endl;
    std::cout << "Present Queue Family Index: " << queueFamilyIndices.presentFamily.value() << std::endl;

    if (graphicsQueue == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to get graphics queue handle");
    }
    if (presentQueue == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to get present queue handle");
    }
}

} // namespace ohao
