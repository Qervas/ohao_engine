#pragma once
// The GPU side of a bench's machine record (sub-project 2b spec, "Profiling first"): a fixed
// compute dispatch (shaders/bench/gpu_calibrate.comp, an integer hash over a 1 MiB buffer)
// timed by timestamps. Run before and after the timed frames and compared with the
// reference `--calibrate` wrote, it tells a GPU that something else was using from a slow
// renderer, as calibrationMs() does for the CPU.
//
// It borrows the renderer's device and graphics queue, and waits for its own work: call it
// only between frames, never while a frame is being recorded.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>

namespace ohao::bench {

class GpuCalibration {
public:
    static constexpr uint32_t kElements = (1u << 20) / 4;   // 1 MiB of uint
    static constexpr uint32_t kGroupSize = 256;            // the shader's local_size_x

    ~GpuCalibration() { destroy(); }

    // `module` is the kernel's SPIR-V; ownership stays with the caller.
    bool init(VkDevice device, VkPhysicalDevice physical, VkQueue queue, uint32_t family, VkShaderModule module) {
        m_device = device;
        m_queue = queue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        m_msPerTick = static_cast<double>(props.limits.timestampPeriod) / 1.0e6;
        if (m_msPerTick <= 0.0 || !module) return false;

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize{kElements} * 4;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vkCreateBuffer(device, &bi, nullptr, &m_buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, m_buffer, &req);
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &m_memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(device, m_buffer, m_memory, 0);

        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &b;
        if (vkCreateDescriptorSetLayout(device, &dl, nullptr, &m_setLayout) != VK_SUCCESS) return false;
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &m_setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(device, &pl, nullptr, &m_layout) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = module;
        cp.stage.pName = "main";
        cp.layout = m_layout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &m_pipeline) != VK_SUCCESS) return false;

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(device, &dp, nullptr, &m_pool) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = m_pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(device, &da, &m_set) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo dbi{m_buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &cpi, nullptr, &m_cmdPool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = m_cmdPool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &ca, &m_cmd) != VK_SUCCESS) return false;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        if (vkCreateQueryPool(device, &qi, nullptr, &m_queries) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device, &fi, nullptr, &m_fence) != VK_SUCCESS) return false;
        m_ready = true;
        return true;
    }

    // One dispatch of `iterations` hash rounds per element, in GPU ms; < 0 on failure.
    double dispatchMs(uint32_t iterations) {
        if (!m_ready) return -1.0;
        vkResetCommandBuffer(m_cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &bi);
        vkCmdResetQueryPool(m_cmd, m_queries, 0, 2);
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
        vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof iterations, &iterations);
        vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, 0);
        vkCmdDispatch(m_cmd, kElements / kGroupSize, 1, 1);
        vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, 1);
        vkEndCommandBuffer(m_cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_cmd;
        vkResetFences(m_device, 1, &m_fence);
        if (vkQueueSubmit(m_queue, 1, &si, m_fence) != VK_SUCCESS) return -1.0;
        if (vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return -1.0;
        uint64_t t[2]{};
        if (vkGetQueryPoolResults(m_device, m_queries, 0, 2, sizeof t, t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            return -1.0;
        return static_cast<double>(t[1] - t[0]) * m_msPerTick;
    }

    // The calibration: two warm-up dispatches (the GPU leaves its idle clocks), then the best
    // of five, in ms. < 0 when the kernel could not run.
    double calibrationMs(uint32_t iterations) {
        for (int i = 0; i < 2; ++i)
            if (dispatchMs(iterations) < 0) return -1.0;
        double best = 1e30;
        for (int i = 0; i < 5; ++i) best = std::min(best, dispatchMs(iterations));
        return best;
    }

    void destroy() {
        if (!m_device) return;
        if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);
        if (m_queries) vkDestroyQueryPool(m_device, m_queries, nullptr);
        if (m_cmdPool) vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
        if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_layout) vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        if (m_buffer) vkDestroyBuffer(m_device, m_buffer, nullptr);
        if (m_memory) vkFreeMemory(m_device, m_memory, nullptr);
        *this = GpuCalibration{};
    }

    GpuCalibration() = default;
    GpuCalibration(const GpuCalibration&) = delete;
    GpuCalibration& operator=(const GpuCalibration&) = delete;
    GpuCalibration& operator=(GpuCalibration&&) = default;

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    double m_msPerTick{0.0};
    VkBuffer m_buffer{VK_NULL_HANDLE};
    VkDeviceMemory m_memory{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkDescriptorSet m_set{VK_NULL_HANDLE};
    VkCommandPool m_cmdPool{VK_NULL_HANDLE};
    VkCommandBuffer m_cmd{VK_NULL_HANDLE};
    VkQueryPool m_queries{VK_NULL_HANDLE};
    VkFence m_fence{VK_NULL_HANDLE};
    bool m_ready{false};
};

} // namespace ohao::bench
