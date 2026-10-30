#include "render/rt/radiance_cache.hpp"

#include "core/log.hpp"
#include "render/rt/rt_compute.hpp"

namespace ohao::render {

namespace {

bool createDeviceBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBuffer& buffer,
                        VkDeviceMemory& memory) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = rtc::findMemoryType(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
    return vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
}

struct ResolveConstants {
    uint32_t capacity;
    uint32_t maxSamples;
    uint32_t staleFrames;
    uint32_t pad;
};

} // namespace

bool RadianceCache::create(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t capacity) {
    m_device = device;
    m_capacity = capacity;
    if (!createDeviceBuffer(device, physicalDevice, VkDeviceSize(capacity) * 4u, m_keys, m_keysMemory) ||
        !createDeviceBuffer(device, physicalDevice, VkDeviceSize(capacity) * 16u, m_accum, m_accumMemory) ||
        !createDeviceBuffer(device, physicalDevice, VkDeviceSize(capacity) * 16u, m_resolved, m_resolvedMemory)) {
        OHAO_LOG(Error, render, "RadianceCache: buffers of {} entries could not be created", capacity);
        destroy();
        return false;
    }

    const VkDescriptorSetLayoutBinding bindings[] = {
        rtc::layoutBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        rtc::layoutBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        rtc::layoutBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS ||
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        destroy();
        return false;
    }
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(device, &setInfo, &m_set) != VK_SUCCESS) {
        destroy();
        return false;
    }
    // The buffers never change: the set is written once.
    const VkDescriptorBufferInfo infos[] = {{m_keys, 0, VK_WHOLE_SIZE}, {m_accum, 0, VK_WHOLE_SIZE},
                                            {m_resolved, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = rtc::descriptorWrite(m_set, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    if (!rtc::createComputePipeline(device, "rt_radiance_cache_resolve.slang.spv", "resolve", m_setLayout,
                                    sizeof(ResolveConstants), m_pipelineLayout, m_pipeline)) {
        destroy();
        return false;
    }
    m_clearPending = true;
    return true;
}

void RadianceCache::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    for (VkBuffer* b : {&m_keys, &m_accum, &m_resolved})
        if (*b) vkDestroyBuffer(m_device, *b, nullptr);
    for (VkDeviceMemory* m : {&m_keysMemory, &m_accumMemory, &m_resolvedMemory})
        if (*m) vkFreeMemory(m_device, *m, nullptr);
    *this = RadianceCache{};
}

void RadianceCache::recordBeginFrame(VkCommandBuffer cmd) {
    if (!valid()) return;
    constexpr VkPipelineStageFlags kPrevious =
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (!m_clearPending) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, kPrevious, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &b, 0, nullptr, 0, nullptr);
        return;
    }
    m_clearPending = false;
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, kPrevious, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    for (VkBuffer b : {m_keys, m_accum, m_resolved}) vkCmdFillBuffer(cmd, b, 0, VK_WHOLE_SIZE, 0u);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &after, 0,
                         nullptr, 0, nullptr);
}

void RadianceCache::recordResolve(VkCommandBuffer cmd) {
    if (!valid()) return;
    VkMemoryBarrier toCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toCompute.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &toCompute, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
    const ResolveConstants rc{m_capacity, kMaxSamples, kStaleFrames, 0};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rc), &rc);
    vkCmdDispatch(cmd, (m_capacity + 255u) / 256u, 1, 1);
    VkMemoryBarrier toTrace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toTrace.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTrace.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1,
                         &toTrace, 0, nullptr, 0, nullptr);
}

} // namespace ohao::render
