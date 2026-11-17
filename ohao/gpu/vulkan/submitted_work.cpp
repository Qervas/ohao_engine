#include "gpu/vulkan/submitted_work.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/gpu_fault.hpp"

namespace ohao::gpu {

void SubmittedWork::initialize(VkDevice device, VkQueue queue, VkCommandPool pool) {
    m_device = device;
    m_queue = queue;
    m_pool = pool;
}

void SubmittedWork::shutdown() {
    if (m_device == VK_NULL_HANDLE) return;
    waitAll();
    for (const VkFence f : m_fences) vkDestroyFence(m_device, f, nullptr);
    m_fences.clear();
    m_device = VK_NULL_HANDLE;
}

VkCommandBuffer SubmittedWork::begin() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void SubmittedWork::submit(VkCommandBuffer cmd, std::string_view where, VkDeviceSize bytes, std::function<void()> release) {
    collect();
    vkEndCommandBuffer(cmd);
    VkFence fence = VK_NULL_HANDLE;
    if (!m_fences.empty()) {
        fence = m_fences.back();
        m_fences.pop_back();
    } else {
        const VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(m_device, &fi, nullptr, &fence) != VK_SUCCESS) fence = VK_NULL_HANDLE;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (fence == VK_NULL_HANDLE) {   // no fence to watch: the old way, once
        OHAO_LOG(Error, gpu, "{}: no fence; waiting for the work", where);
        submitAndWait(m_device, m_queue, si, where);
        vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
        if (release) release();
        return;
    }
    Work w{fence, cmd, bytes, std::move(release)};
    if (gpu::submit(m_queue, si, fence, where) != VK_SUCCESS) {   // nothing was submitted
        vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
        m_fences.push_back(fence);
        if (w.release) w.release();
        return;
    }
    m_bytes += bytes;
    m_work.push_back(std::move(w));
    while (m_bytes > kMaxBytes && m_work.size() > 1) {
        waitFence(m_device, m_work.front().fence, where);
        finish(m_work.front());
        m_work.pop_front();
    }
}

void SubmittedWork::collect() {
    while (!m_work.empty() && vkGetFenceStatus(m_device, m_work.front().fence) == VK_SUCCESS) {
        finish(m_work.front());
        m_work.pop_front();
    }
}

void SubmittedWork::waitAll() {
    while (!m_work.empty()) {
        waitFence(m_device, m_work.front().fence, "submitted work");
        finish(m_work.front());
        m_work.pop_front();
    }
}

void SubmittedWork::finish(Work& w) {
    vkFreeCommandBuffers(m_device, m_pool, 1, &w.cmd);
    vkResetFences(m_device, 1, &w.fence);
    m_fences.push_back(w.fence);
    m_bytes -= w.bytes;
    if (w.release) w.release();
}

} // namespace ohao::gpu
