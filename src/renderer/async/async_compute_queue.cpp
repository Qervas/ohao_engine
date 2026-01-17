#include "async_compute_queue.hpp"
#include <iostream>
#include <algorithm>

namespace ohao {

AsyncComputeQueue::~AsyncComputeQueue() {
    cleanup();
}

bool AsyncComputeQueue::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                    uint32_t computeQueueFamily, uint32_t computeQueueIndex) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_computeQueueFamily = computeQueueFamily;

    // Get compute queue
    vkGetDeviceQueue(m_device, computeQueueFamily, computeQueueIndex, &m_computeQueue);
    if (!m_computeQueue) {
        std::cerr << "Failed to get compute queue" << std::endl;
        return false;
    }

    if (!createCommandPool()) {
        return false;
    }

    if (!createTimelineSemaphore()) {
        return false;
    }

    std::cout << "AsyncComputeQueue initialized on queue family " << computeQueueFamily << std::endl;
    return true;
}

void AsyncComputeQueue::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    // Wait for all pending work
    waitIdle();

    // Destroy timeline semaphore
    if (m_timelineSemaphore.semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_timelineSemaphore.semaphore, nullptr);
        m_timelineSemaphore.semaphore = VK_NULL_HANDLE;
    }

    // Free command buffers
    if (!m_freeCommandBuffers.empty() && m_commandPool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(m_device, m_commandPool,
                             static_cast<uint32_t>(m_freeCommandBuffers.size()),
                             m_freeCommandBuffers.data());
        m_freeCommandBuffers.clear();
    }

    // Destroy command pool
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    m_activeTasks.clear();
    m_taskMap.clear();
    m_device = VK_NULL_HANDLE;
}

bool AsyncComputeQueue::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_computeQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        std::cerr << "Failed to create async compute command pool" << std::endl;
        return false;
    }

    return true;
}

bool AsyncComputeQueue::createTimelineSemaphore() {
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &typeInfo;

    if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_timelineSemaphore.semaphore) != VK_SUCCESS) {
        std::cerr << "Failed to create timeline semaphore" << std::endl;
        return false;
    }

    m_timelineSemaphore.currentValue = 0;
    return true;
}

VkCommandBuffer AsyncComputeQueue::allocateCommandBuffer() {
    if (!m_freeCommandBuffers.empty()) {
        VkCommandBuffer cmd = m_freeCommandBuffers.back();
        m_freeCommandBuffers.pop_back();
        vkResetCommandBuffer(cmd, 0);
        return cmd;
    }

    // Allocate new command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(m_device, &allocInfo, &cmd) != VK_SUCCESS) {
        std::cerr << "Failed to allocate async compute command buffer" << std::endl;
        return VK_NULL_HANDLE;
    }

    return cmd;
}

void AsyncComputeQueue::freeCommandBuffer(VkCommandBuffer cmd) {
    if (m_freeCommandBuffers.size() < MAX_COMMAND_BUFFERS) {
        m_freeCommandBuffers.push_back(cmd);
    } else {
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    }
}

AsyncTaskHandle AsyncComputeQueue::submitTask(
    std::function<void(VkCommandBuffer)> recordCommands,
    std::function<void()> onComplete
) {
    return submitTaskWithWait(recordCommands, {}, onComplete);
}

AsyncTaskHandle AsyncComputeQueue::submitTaskWithWait(
    std::function<void(VkCommandBuffer)> recordCommands,
    const std::vector<std::pair<VkSemaphore, uint64_t>>& waitSemaphores,
    std::function<void()> onComplete
) {
    std::lock_guard<std::mutex> lock(m_taskMutex);

    // Allocate command buffer
    VkCommandBuffer cmd = allocateCommandBuffer();
    if (cmd == VK_NULL_HANDLE) {
        return AsyncTaskHandle{0};
    }

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        freeCommandBuffer(cmd);
        return AsyncTaskHandle{0};
    }

    // Record user commands
    recordCommands(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        freeCommandBuffer(cmd);
        return AsyncTaskHandle{0};
    }

    // Create task
    AsyncComputeTask task;
    task.handle = AsyncTaskHandle{m_nextTaskId++};
    task.recordCommands = recordCommands;
    task.onComplete = onComplete;
    task.waitSemaphores = waitSemaphores;
    task.signalValue = ++m_timelineSemaphore.currentValue;
    task.status = AsyncTaskStatus::Executing;

    // Build submit info with timeline semaphores
    std::vector<VkSemaphore> waitSems;
    std::vector<uint64_t> waitValues;
    std::vector<VkPipelineStageFlags> waitStages;

    for (const auto& [sem, value] : waitSemaphores) {
        waitSems.push_back(sem);
        waitValues.push_back(value);
        waitStages.push_back(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
    timelineInfo.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &task.signalValue;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
    submitInfo.pWaitSemaphores = waitSems.empty() ? nullptr : waitSems.data();
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_timelineSemaphore.semaphore;

    if (vkQueueSubmit(m_computeQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        freeCommandBuffer(cmd);
        return AsyncTaskHandle{0};
    }

    // Store task info (with command buffer for later cleanup)
    m_taskMap[task.handle.id] = task;
    m_activeTasks.push_back(task);
    m_pendingTaskCount++;

    return task.handle;
}

bool AsyncComputeQueue::isTaskComplete(AsyncTaskHandle handle) {
    if (!handle.valid()) return true;

    std::lock_guard<std::mutex> lock(m_taskMutex);

    auto it = m_taskMap.find(handle.id);
    if (it == m_taskMap.end()) {
        return true;  // Task not found, assume complete
    }

    if (it->second.status == AsyncTaskStatus::Completed) {
        return true;
    }

    // Query timeline semaphore value
    uint64_t semValue = 0;
    vkGetSemaphoreCounterValue(m_device, m_timelineSemaphore.semaphore, &semValue);

    return semValue >= it->second.signalValue;
}

void AsyncComputeQueue::waitForTask(AsyncTaskHandle handle) {
    if (!handle.valid()) return;

    uint64_t signalValue = getTaskSignalValue(handle);
    if (signalValue == 0) return;

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_timelineSemaphore.semaphore;
    waitInfo.pValues = &signalValue;

    vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX);
}

void AsyncComputeQueue::waitIdle() {
    if (m_computeQueue != VK_NULL_HANDLE) {
        vkQueueWaitIdle(m_computeQueue);
    }

    processCompletedTasks();
}

void AsyncComputeQueue::processCompletedTasks() {
    std::lock_guard<std::mutex> lock(m_taskMutex);

    // Get current semaphore value
    uint64_t semValue = 0;
    vkGetSemaphoreCounterValue(m_device, m_timelineSemaphore.semaphore, &semValue);

    // Process completed tasks
    auto it = m_activeTasks.begin();
    while (it != m_activeTasks.end()) {
        if (semValue >= it->signalValue) {
            // Task completed
            it->status = AsyncTaskStatus::Completed;
            m_pendingTaskCount--;
            m_completedTaskCount++;

            // Call completion callback
            if (it->onComplete) {
                it->onComplete();
            }

            // Update task map
            auto mapIt = m_taskMap.find(it->handle.id);
            if (mapIt != m_taskMap.end()) {
                mapIt->second.status = AsyncTaskStatus::Completed;
            }

            it = m_activeTasks.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t AsyncComputeQueue::getTaskSignalValue(AsyncTaskHandle handle) const {
    auto it = m_taskMap.find(handle.id);
    if (it == m_taskMap.end()) {
        return 0;
    }
    return it->second.signalValue;
}

// AsyncComputeHelper implementations

void AsyncComputeHelper::dispatchCompute(VkCommandBuffer cmd, VkPipeline pipeline,
                                          VkPipelineLayout layout, VkDescriptorSet descSet,
                                          uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &descSet, 0, nullptr);
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
}

void AsyncComputeHelper::computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1, &barrier,
        0, nullptr,
        0, nullptr
    );
}

void AsyncComputeHelper::computeToGraphicsBarrier(VkCommandBuffer cmd,
                                                   VkImage image, VkImageLayout oldLayout,
                                                   VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

void AsyncComputeHelper::graphicsToComputeBarrier(VkCommandBuffer cmd,
                                                   VkImage image, VkImageLayout oldLayout,
                                                   VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

} // namespace ohao
