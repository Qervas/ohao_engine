#include "frame_resources.hpp"
#include "core/log.hpp"
#include <cstring>
#include <stdexcept>

namespace ohao {

FrameResourceManager::~FrameResourceManager() {
    shutdown();
}

FrameResourceManager::FrameResourceManager(FrameResourceManager&& other) noexcept
    : m_device(other.m_device)
    , m_physicalDevice(other.m_physicalDevice)
    , m_frames(std::move(other.m_frames))
    , m_initialized(other.m_initialized)
{
    other.m_device = VK_NULL_HANDLE;
    other.m_physicalDevice = VK_NULL_HANDLE;
    other.m_initialized = false;
}

FrameResourceManager& FrameResourceManager::operator=(FrameResourceManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_device = other.m_device;
        m_physicalDevice = other.m_physicalDevice;
        m_frames = std::move(other.m_frames);
        m_initialized = other.m_initialized;
        other.m_device = VK_NULL_HANDLE;
        other.m_physicalDevice = VK_NULL_HANDLE;
        other.m_initialized = false;
    }
    return *this;
}

bool FrameResourceManager::initialize(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkCommandPool commandPool,
    size_t stagingBufferSize)
{
    if (m_initialized) {
        return true;
    }

    m_device = device;
    m_physicalDevice = physicalDevice;

    if (!createCommandBuffers(commandPool)) {
        OHAO_LOG(Error, render, "Failed to create per-frame command buffers");
        return false;
    }

    if (!createSyncObjects()) {
        OHAO_LOG(Error, render, "Failed to create per-frame sync objects");
        return false;
    }

    if (stagingBufferSize > 0) {
        if (!createStagingBuffers(stagingBufferSize)) {
            OHAO_LOG(Error, render, "Failed to create per-frame staging buffers");
            return false;
        }
    }

    m_initialized = true;
    OHAO_LOG(Debug, render, "FrameResourceManager initialized with {} frames in flight",
             MAX_FRAMES_IN_FLIGHT);
    return true;
}

void FrameResourceManager::shutdown() {
    if (!m_initialized || m_device == VK_NULL_HANDLE) {
        return;
    }

    // Wait for all frames to complete
    vkDeviceWaitIdle(m_device);

    for (auto& frame : m_frames) {
        // Command buffers are freed when command pool is destroyed

        // Destroy fences
        if (frame.renderFence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, frame.renderFence, nullptr);
            frame.renderFence = VK_NULL_HANDLE;
        }

        // Destroy staging buffers
        if (frame.stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, frame.stagingBuffer, nullptr);
            frame.stagingBuffer = VK_NULL_HANDLE;
        }
        if (frame.stagingBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, frame.stagingBufferMemory, nullptr);
            frame.stagingBufferMemory = VK_NULL_HANDLE;
        }

        frame.valid = false;
    }

    m_initialized = false;
}

bool FrameResourceManager::waitForFrame(uint32_t frameIndex, uint64_t timeoutNs) {
    if (!m_initialized || frameIndex >= MAX_FRAMES_IN_FLIGHT) {
        return false;
    }

    VkFence fence = m_frames[frameIndex].renderFence;
    if (fence == VK_NULL_HANDLE) {
        return true; // No fence means nothing to wait for
    }

    VkResult result = vkWaitForFences(m_device, 1, &fence, VK_TRUE, timeoutNs);
    return result == VK_SUCCESS;
}

void FrameResourceManager::resetFrame(uint32_t frameIndex) {
    if (!m_initialized || frameIndex >= MAX_FRAMES_IN_FLIGHT) {
        return;
    }

    VkFence fence = m_frames[frameIndex].renderFence;
    if (fence != VK_NULL_HANDLE) {
        vkResetFences(m_device, 1, &fence);
    }
}

FrameResources& FrameResourceManager::getFrame(uint32_t frameIndex) {
    return m_frames[frameIndex % MAX_FRAMES_IN_FLIGHT];
}

const FrameResources& FrameResourceManager::getFrame(uint32_t frameIndex) const {
    return m_frames[frameIndex % MAX_FRAMES_IN_FLIGHT];
}

bool FrameResourceManager::createCommandBuffers(VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> commandBuffers{};
    if (vkAllocateCommandBuffers(m_device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        return false;
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_frames[i].commandBuffer = commandBuffers[i];
    }

    return true;
}

bool FrameResourceManager::createSyncObjects() {
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Create fence in signaled state so first frame doesn't wait forever
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& frame : m_frames) {
        if (vkCreateFence(m_device, &fenceInfo, nullptr, &frame.renderFence) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool FrameResourceManager::createStagingBuffers(size_t size) {
    for (auto& frame : m_frames) {
        if (!createBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frame.stagingBuffer,
            frame.stagingBufferMemory))
        {
            return false;
        }

        // Map staging buffer persistently
        vkMapMemory(m_device, frame.stagingBufferMemory, 0, size, 0, &frame.stagingBufferMapped);

        // Initialize staging buffer to black (prevents garbage on first few frames)
        memset(frame.stagingBufferMapped, 0, size);
    }

    return true;
}

bool FrameResourceManager::resizeStagingBuffers(size_t newSize) {
    if (!m_initialized || m_device == VK_NULL_HANDLE) {
        return false;
    }

    // Wait for all frames to complete (caller should have done this, but be safe)
    vkDeviceWaitIdle(m_device);

    // Cleanup old staging buffers
    for (auto& frame : m_frames) {
        if (frame.stagingBufferMapped != nullptr) {
            vkUnmapMemory(m_device, frame.stagingBufferMemory);
            frame.stagingBufferMapped = nullptr;
        }
        if (frame.stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, frame.stagingBuffer, nullptr);
            frame.stagingBuffer = VK_NULL_HANDLE;
        }
        if (frame.stagingBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, frame.stagingBufferMemory, nullptr);
            frame.stagingBufferMemory = VK_NULL_HANDLE;
        }
    }

    // Create new staging buffers with new size
    if (!createStagingBuffers(newSize)) {
        OHAO_LOG(Error, render, "Failed to recreate staging buffers on resize");
        return false;
    }

    OHAO_LOG(Debug, render, "Staging buffers resized to {} bytes", newSize);
    return true;
}

uint32_t FrameResourceManager::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

bool FrameResourceManager::createBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& bufferMemory)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(m_device, buffer, bufferMemory, 0);
    return true;
}

} // namespace ohao
