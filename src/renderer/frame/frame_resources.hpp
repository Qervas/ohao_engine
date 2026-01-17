#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>

namespace ohao {

/**
 * Maximum number of frames that can be processed concurrently.
 * 3 frames allows for optimal pipelining: one being rendered by GPU,
 * one being recorded by CPU, one being displayed.
 */
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

/**
 * Per-frame GPU resources for multi-frame rendering.
 *
 * Each frame in flight has its own set of:
 * - Command buffer (to record commands while previous frame is executing)
 * - Fence (to track when this frame's GPU work completes)
 * - Uniform buffers (to avoid overwriting data while GPU is reading)
 * - Descriptor set (bound to this frame's uniform buffers)
 *
 * This eliminates CPU-GPU synchronization stalls by allowing the CPU
 * to prepare frame N+1 while the GPU is still rendering frame N.
 */
struct FrameResources {
    // Command buffer for this frame
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};

    // Synchronization: fence signals when GPU finishes this frame
    VkFence renderFence{VK_NULL_HANDLE};

    // Camera uniform buffer (view/proj matrices)
    VkBuffer cameraBuffer{VK_NULL_HANDLE};
    VkDeviceMemory cameraBufferMemory{VK_NULL_HANDLE};
    void* cameraBufferMapped{nullptr};

    // Light uniform buffer
    VkBuffer lightBuffer{VK_NULL_HANDLE};
    VkDeviceMemory lightBufferMemory{VK_NULL_HANDLE};
    void* lightBufferMapped{nullptr};

    // Descriptor set for this frame (binds to this frame's buffers)
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};

    // Staging buffer for pixel readback (optional, only if async readback is needed)
    VkBuffer stagingBuffer{VK_NULL_HANDLE};
    VkDeviceMemory stagingBufferMemory{VK_NULL_HANDLE};
    void* stagingBufferMapped{nullptr};

    // Track if this frame's resources are valid
    bool valid{false};
};

/**
 * Camera uniform buffer structure (must match shader layout)
 */
struct FrameCameraUBO {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::vec3 viewPos;
};

/**
 * Manages a ring buffer of frame resources for multi-frame rendering.
 *
 * Usage pattern:
 *   1. At frame start: waitForFrame(currentFrame) - blocks until frame N-3 completes
 *   2. Get resources: getFrameResources(currentFrame)
 *   3. Record commands to the frame's command buffer
 *   4. Submit work, signaling the frame's fence
 *   5. Advance: currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT
 *
 * This allows up to MAX_FRAMES_IN_FLIGHT frames to be in various stages
 * of processing simultaneously.
 */
class FrameResourceManager {
public:
    FrameResourceManager() = default;
    ~FrameResourceManager();

    // Non-copyable
    FrameResourceManager(const FrameResourceManager&) = delete;
    FrameResourceManager& operator=(const FrameResourceManager&) = delete;

    // Move allowed
    FrameResourceManager(FrameResourceManager&&) noexcept;
    FrameResourceManager& operator=(FrameResourceManager&&) noexcept;

    /**
     * Initialize all frame resources.
     * Must be called after Vulkan device and command pool are created.
     */
    bool initialize(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkCommandPool commandPool,
        VkDescriptorSetLayout descriptorSetLayout,
        VkDescriptorPool descriptorPool,
        VkImageView shadowImageView,
        VkSampler shadowSampler,
        size_t cameraBufferSize,
        size_t lightBufferSize,
        size_t stagingBufferSize
    );

    /**
     * Clean up all frame resources.
     */
    void shutdown();

    /**
     * Wait for a specific frame to complete GPU execution.
     * Call this at the start of each frame to ensure frame N-MAX_FRAMES_IN_FLIGHT
     * has completed before we reuse its resources.
     *
     * @param frameIndex Frame index (0 to MAX_FRAMES_IN_FLIGHT-1)
     * @param timeoutNs Timeout in nanoseconds (default: infinite)
     * @return true if fence was signaled, false if timeout
     */
    bool waitForFrame(uint32_t frameIndex, uint64_t timeoutNs = UINT64_MAX);

    /**
     * Reset the fence for a frame so it can be reused.
     * Call this after waitForFrame() succeeds.
     *
     * @param frameIndex Frame index
     */
    void resetFrame(uint32_t frameIndex);

    /**
     * Get the resources for a specific frame.
     *
     * @param frameIndex Frame index (0 to MAX_FRAMES_IN_FLIGHT-1)
     * @return Reference to frame resources
     */
    FrameResources& getFrame(uint32_t frameIndex);
    const FrameResources& getFrame(uint32_t frameIndex) const;

    /**
     * Get the next frame index in the ring buffer.
     *
     * @param currentFrame Current frame index
     * @return Next frame index
     */
    static uint32_t nextFrame(uint32_t currentFrame) {
        return (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    /**
     * Check if resources are initialized.
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * Resize staging buffers for new framebuffer dimensions.
     * Must wait for all frames to complete before calling.
     *
     * @param newSize New staging buffer size in bytes
     * @return true if successful
     */
    bool resizeStagingBuffers(size_t newSize);

private:
    bool createCommandBuffers(VkCommandPool commandPool);
    bool createSyncObjects();
    bool createUniformBuffers(size_t cameraBufferSize, size_t lightBufferSize);
    bool createStagingBuffers(size_t size);
    bool createDescriptorSets(
        VkDescriptorSetLayout layout,
        VkDescriptorPool pool,
        VkImageView shadowImageView,
        VkSampler shadowSampler,
        size_t cameraBufferSize,
        size_t lightBufferSize
    );

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    bool createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkBuffer& buffer,
        VkDeviceMemory& bufferMemory
    );

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    std::array<FrameResources, MAX_FRAMES_IN_FLIGHT> m_frames{};
    bool m_initialized{false};
};

} // namespace ohao
