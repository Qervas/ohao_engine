#include "render/rt/radiance_cache.hpp"
#include "render/shader_bindings/rt_radiance_cache_resolve.hpp"

#include "core/log.hpp"
#include "render/rt/rt_compute.hpp"

namespace ohao::render {

namespace {

struct ResolveConstants {
    uint32_t capacity;
    uint32_t maxSamples;
    uint32_t staleFrames;
    uint32_t pad;
};
OHAO_CHECK_PUSH(ResolveConstants, rt_radiance_cache_resolve)   // rt/radiance_cache_resolve.slang's

} // namespace

bool RadianceCache::create(VkDevice device, GpuAllocator& allocator, uint32_t capacity, uint32_t maxSamples) {
    m_device = device;
    m_allocator = &allocator;
    m_capacity = capacity;
    m_maxSamples = maxSamples;
    constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    m_keys = allocator.createBuffer(VkDeviceSize(capacity) * 4u, kUsage, AllocationUsage::GpuOnly);
    m_accum = allocator.createBuffer(VkDeviceSize(capacity) * 16u, kUsage, AllocationUsage::GpuOnly);
    m_resolved = allocator.createBuffer(VkDeviceSize(capacity) * 16u, kUsage, AllocationUsage::GpuOnly);
    if (!m_keys.isValid() || !m_accum.isValid() || !m_resolved.isValid()) {
        OHAO_LOG(Error, render, "RadianceCache: buffers of {} entries could not be created", capacity);
        destroy();
        return false;
    }

    const VkDescriptorSetLayoutBinding bindings[] = {
        shader::layoutBinding(shader::rt_radiance_cache_resolve::keys),
        shader::layoutBinding(shader::rt_radiance_cache_resolve::accum),
        shader::layoutBinding(shader::rt_radiance_cache_resolve::resolved),
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
    const VkDescriptorBufferInfo infos[] = {{m_keys.buffer, 0, VK_WHOLE_SIZE}, {m_accum.buffer, 0, VK_WHOLE_SIZE},
                                            {m_resolved.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = shader::write(shader::rt_radiance_cache_resolve::kSet0[i], m_set);
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
    for (GpuBuffer* b : {&m_keys, &m_accum, &m_resolved}) m_allocator->retire(*b);
    *this = RadianceCache{};
}

void RadianceCache::fedBy(Pass& pass) {
    pass.write(m_keys, BufferUse::Storage).write(m_accum, BufferUse::Storage).read(m_resolved, BufferUse::Storage);
}

void RadianceCache::lookedUpBy(Pass& pass) {
    pass.read(m_keys, BufferUse::Storage).read(m_resolved, BufferUse::Storage);
}

void RadianceCache::recordBeginFrame(const Frame& frame) {
    if (!valid() || !m_clearPending) return;
    m_clearPending = false;
    Pass pass(frame, nullptr, Pass::Transfer);
    for (GpuBuffer* b : {&m_keys, &m_accum, &m_resolved}) pass.write(*b, BufferUse::CopyDst);
    pass.begin();
    for (const GpuBuffer* b : {&m_keys, &m_accum, &m_resolved}) vkCmdFillBuffer(frame.cmd, b->buffer, 0, VK_WHOLE_SIZE, 0u);
}

void RadianceCache::recordResolve(const Frame& frame) {
    if (!valid()) return;
    const VkCommandBuffer cmd = frame.cmd;
    Pass pass(frame, nullptr, Pass::Compute);
    for (GpuBuffer* b : {&m_keys, &m_accum, &m_resolved}) pass.write(*b, BufferUse::Storage);
    pass.begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
    const ResolveConstants rc{m_capacity, m_maxSamples, kStaleFrames, 0};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rc), &rc);
    vkCmdDispatch(cmd, (m_capacity + 255u) / 256u, 1, 1);
}

} // namespace ohao::render
