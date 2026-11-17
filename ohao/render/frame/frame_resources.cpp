#include "frame_resources.hpp"
#include "gpu/vulkan/gpu_fault.hpp"
#include "core/log.hpp"

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
    VkCommandPool commandPool)
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

        frame.valid = false;
    }

    m_initialized = false;
}

bool FrameResourceManager::waitForFrame(uint32_t frameIndex) {
    if (!m_initialized || frameIndex >= MAX_FRAMES_IN_FLIGHT) {
        return false;
    }

    VkFence fence = m_frames[frameIndex].renderFence;
    if (fence == VK_NULL_HANDLE) {
        return true; // No fence means nothing to wait for
    }

    gpu::waitFence(m_device, fence, "frame fence");   // a hang or a lost device does not return
    return true;
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

} // namespace ohao
