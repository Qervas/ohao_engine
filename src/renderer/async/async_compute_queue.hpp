#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <atomic>
#include <glm/glm.hpp>

namespace ohao {

// Forward declarations
class GpuAllocator;

// Async compute task status
enum class AsyncTaskStatus {
    Pending,
    Executing,
    Completed,
    Failed
};

// Async compute task handle
struct AsyncTaskHandle {
    uint64_t id{0};
    bool valid() const { return id != 0; }

    bool operator==(const AsyncTaskHandle& other) const { return id == other.id; }
    bool operator!=(const AsyncTaskHandle& other) const { return id != other.id; }
};

// Timeline semaphore for async synchronization
struct TimelineSemaphore {
    VkSemaphore semaphore{VK_NULL_HANDLE};
    uint64_t currentValue{0};
};

// Async compute task definition
struct AsyncComputeTask {
    AsyncTaskHandle handle;
    std::function<void(VkCommandBuffer)> recordCommands;
    std::function<void()> onComplete;

    // Synchronization
    uint64_t signalValue{0};
    std::vector<std::pair<VkSemaphore, uint64_t>> waitSemaphores;  // (semaphore, value) pairs

    AsyncTaskStatus status{AsyncTaskStatus::Pending};
};

// Manages async compute queue for parallel GPU work
class AsyncComputeQueue {
public:
    AsyncComputeQueue() = default;
    ~AsyncComputeQueue();

    // Initialization
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t computeQueueFamily, uint32_t computeQueueIndex);
    void cleanup();

    // Queue a compute task
    AsyncTaskHandle submitTask(
        std::function<void(VkCommandBuffer)> recordCommands,
        std::function<void()> onComplete = nullptr
    );

    // Submit with explicit wait semaphores (for cross-queue sync)
    AsyncTaskHandle submitTaskWithWait(
        std::function<void(VkCommandBuffer)> recordCommands,
        const std::vector<std::pair<VkSemaphore, uint64_t>>& waitSemaphores,
        std::function<void()> onComplete = nullptr
    );

    // Check if a task is complete
    bool isTaskComplete(AsyncTaskHandle handle);

    // Wait for a specific task to complete (blocking)
    void waitForTask(AsyncTaskHandle handle);

    // Wait for all pending tasks (blocking)
    void waitIdle();

    // Poll and process completed tasks (call each frame)
    void processCompletedTasks();

    // Get the timeline semaphore for external synchronization
    VkSemaphore getTimelineSemaphore() const { return m_timelineSemaphore.semaphore; }
    uint64_t getCurrentSemaphoreValue() const { return m_timelineSemaphore.currentValue; }

    // Get semaphore value for a specific task (for wait operations)
    uint64_t getTaskSignalValue(AsyncTaskHandle handle) const;

    // Stats
    uint32_t getPendingTaskCount() const { return m_pendingTaskCount.load(); }
    uint32_t getCompletedTaskCount() const { return m_completedTaskCount.load(); }

private:
    bool createCommandPool();
    bool createTimelineSemaphore();
    VkCommandBuffer allocateCommandBuffer();
    void freeCommandBuffer(VkCommandBuffer cmd);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkQueue m_computeQueue{VK_NULL_HANDLE};
    uint32_t m_computeQueueFamily{0};

    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> m_freeCommandBuffers;

    TimelineSemaphore m_timelineSemaphore;

    // Task management
    std::vector<AsyncComputeTask> m_activeTasks;
    std::map<uint64_t, AsyncComputeTask> m_taskMap;  // handle.id -> task
    std::mutex m_taskMutex;

    std::atomic<uint64_t> m_nextTaskId{1};
    std::atomic<uint32_t> m_pendingTaskCount{0};
    std::atomic<uint32_t> m_completedTaskCount{0};

    static constexpr uint32_t MAX_COMMAND_BUFFERS = 16;
};

// Helper class for common async compute operations
class AsyncComputeHelper {
public:
    static void dispatchCompute(VkCommandBuffer cmd, VkPipeline pipeline,
                                VkPipelineLayout layout, VkDescriptorSet descSet,
                                uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ);

    // Insert memory barrier for compute->compute dependency
    static void computeBarrier(VkCommandBuffer cmd);

    // Insert memory barrier for compute->graphics dependency
    static void computeToGraphicsBarrier(VkCommandBuffer cmd,
                                          VkImage image, VkImageLayout oldLayout,
                                          VkImageLayout newLayout);

    // Insert memory barrier for graphics->compute dependency
    static void graphicsToComputeBarrier(VkCommandBuffer cmd,
                                          VkImage image, VkImageLayout oldLayout,
                                          VkImageLayout newLayout);
};

} // namespace ohao
