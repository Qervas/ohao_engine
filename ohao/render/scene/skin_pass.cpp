#include "render/scene/skin_pass.hpp"
#include "render/shader_bindings/skin_skin.hpp"

#include "core/log.hpp"
#include "render/rt/rt_compute.hpp"
#include "gpu/shared_layout.hpp"

#include <algorithm>
#include <cstring>

namespace ohao::render {

namespace {
constexpr uint32_t kBindings = shader::skin_skin::kSet0.size();   // vertices, palettes, meshlets, meshlet vertices
constexpr VkDeviceSize kMinPaletteBytes = 64u * 1024u;
}

bool SkinPass::initialize(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;
    std::array<VkDescriptorSetLayoutBinding, kBindings> bindings{};
    for (uint32_t i = 0; i < kBindings; ++i) bindings[i] = shader::layoutBinding(shader::skin_skin::kSet0[i]);
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindings;
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    for (Slot& s : m_slots) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(device, &ai, &s.set) != VK_SUCCESS) return false;
    }
    const auto push = static_cast<uint32_t>(sizeof(gpu::shared::SkinPush));
    if (!rtc::createComputePipeline(device, "skin_skin.slang.skin.spv", "skin", m_setLayout, push, m_skinLayout, m_skin) ||
        !rtc::createComputePipeline(device, "skin_skin.slang.bounds.spv", "bounds", m_setLayout, push, m_boundsLayout,
                                    m_bounds)) {
        OHAO_LOG(Warn, render, "skin: pipelines unavailable; skinned instances draw their bind pose");
        return false;
    }
    return true;
}

void SkinPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) return;
    for (Slot& s : m_slots) {
        m_allocator->destroyBuffer(s.palettes);
        s = Slot{};
    }
    for (VkPipeline* p : {&m_skin, &m_bounds})
        if (*p) vkDestroyPipeline(m_device, *p, nullptr), *p = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&m_skinLayout, &m_boundsLayout})
        if (*l) vkDestroyPipelineLayout(m_device, *l, nullptr), *l = VK_NULL_HANDLE;
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr), m_pool = VK_NULL_HANDLE;
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr), m_setLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

bool SkinPass::prepare(uint32_t slotIndex, std::span<const Job> jobs, GpuBuffer& vertexArena, GpuBuffer& meshletArena,
                       GpuBuffer& meshletVertexArena) {
    if (!ready() || slotIndex >= m_slots.size() || jobs.empty()) return false;
    m_vertices = &vertexArena;
    m_meshlets = &meshletArena;
    m_meshletVertices = &meshletVertexArena;
    Slot& s = m_slots[slotIndex];
    VkDeviceSize matrices = 0;
    for (const Job& j : jobs) matrices += j.current.size() + j.previous.size();
    const VkDeviceSize bytes = std::max<VkDeviceSize>(matrices * sizeof(glm::mat4), sizeof(glm::mat4));
    if (bytes > s.capacity) {
        m_allocator->destroyBuffer(s.palettes);   // the slot's fence was waited: unused
        const VkDeviceSize cap = std::max({bytes, s.capacity * 2, kMinPaletteBytes});
        s.palettes = rtc::createHostBuffer(*m_allocator, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!s.palettes.isValid()) {
            s.capacity = 0;
            OHAO_LOG(Error, render, "skin: palette buffer allocation failed ({} bytes)", cap);
            return false;
        }
        s.capacity = cap;
    }
    auto* out = static_cast<glm::mat4*>(s.palettes.getMappedData());
    s.offsets.clear();
    uint32_t at = 0;
    for (const Job& j : jobs) {
        s.offsets.push_back(at);
        std::memcpy(out + at, j.current.data(), j.current.size_bytes());
        at += static_cast<uint32_t>(j.current.size());
        s.offsets.push_back(at);
        std::memcpy(out + at, j.previous.data(), j.previous.size_bytes());
        at += static_cast<uint32_t>(j.previous.size());
    }
    m_allocator->flushBuffer(s.palettes, 0, VkDeviceSize{at} * sizeof(glm::mat4));

    const std::array<VkDescriptorBufferInfo, kBindings> infos{{
        {vertexArena.buffer, 0, VK_WHOLE_SIZE},
        {s.palettes.buffer, 0, VK_WHOLE_SIZE},
        {meshletArena.buffer, 0, VK_WHOLE_SIZE},
        {meshletVertexArena.buffer, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        writes[i] = shader::write(shader::skin_skin::kSet0[i], s.set);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, kBindings, writes.data(), 0, nullptr);
    return true;
}

void SkinPass::recordSkin(const Frame& frame, std::span<const Job> jobs) {
    Slot& s = m_slots[frame.slot];
    const VkCommandBuffer cmd = frame.cmd;
    Pass pass(frame, "skin", Pass::Compute);
    pass.readWrite(*m_vertices, BufferUse::Storage).read(s.palettes, BufferUse::Storage);
    pass.begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skin);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinLayout, 0, 1, &s.set, 0, nullptr);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const Job& j = jobs[i];
        gpu::shared::SkinPush push{};
        push.mesh = {j.baseVertex, j.vertex, j.vertexCount, 0u};
        push.palette = {s.offsets[2 * i], s.offsets[2 * i + 1], static_cast<uint32_t>(j.current.size()), 0u};
        push.meshlets = {j.firstMeshlet, j.meshletCount, 0u, 0u};
        vkCmdPushConstants(cmd, m_skinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (j.vertexCount + gpu::shared::kSkinGroupSize - 1) / gpu::shared::kSkinGroupSize, 1, 1);
    }
}

void SkinPass::recordBounds(const Frame& frame, std::span<const Job> jobs) {
    Slot& s = m_slots[frame.slot];
    const VkCommandBuffer cmd = frame.cmd;
    Pass pass(frame, "skin.bounds", Pass::Compute);
    pass.read(*m_vertices, BufferUse::Storage).read(*m_meshletVertices, BufferUse::Storage);
    pass.read(s.palettes, BufferUse::Storage).readWrite(*m_meshlets, BufferUse::Storage);
    pass.begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bounds);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boundsLayout, 0, 1, &s.set, 0, nullptr);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const Job& j = jobs[i];
        if (j.meshletCount == 0) continue;
        gpu::shared::SkinPush push{};
        push.mesh = {j.baseVertex, j.vertex, j.vertexCount, 0u};
        push.palette = {s.offsets[2 * i], s.offsets[2 * i + 1], static_cast<uint32_t>(j.current.size()), 0u};
        push.meshlets = {j.firstMeshlet, j.meshletCount, 0u, 0u};
        vkCmdPushConstants(cmd, m_boundsLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (j.meshletCount + gpu::shared::kSkinGroupSize - 1) / gpu::shared::kSkinGroupSize, 1, 1);
    }
}

} // namespace ohao::render
