#include "render/scene/skin_pass.hpp"

#include "core/log.hpp"
#include "gpu/shared_layout.hpp"

#include <algorithm>
#include <cstring>

namespace ohao::render {

namespace {
constexpr uint32_t kBindings = 4;   // vertices, palettes, meshlets, meshlet vertices
constexpr VkDeviceSize kMinPaletteBytes = 64u * 1024u;
}

bool SkinPass::initialize(VkDevice device, VkPhysicalDevice physical) {
    m_device = device;
    m_physical = physical;
    std::array<VkDescriptorSetLayoutBinding, kBindings> bindings{};
    for (uint32_t i = 0; i < kBindings; ++i) bindings[i] = rtc::layoutBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
        rtc::destroyHostBuffer(m_device, s.palettes);
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

bool SkinPass::prepare(uint32_t slotIndex, std::span<const Job> jobs, VkBuffer vertexArena, VkBuffer meshletArena,
                       VkBuffer meshletVertexArena) {
    if (!ready() || slotIndex >= m_slots.size() || jobs.empty()) return false;
    Slot& s = m_slots[slotIndex];
    VkDeviceSize matrices = 0;
    for (const Job& j : jobs) matrices += j.current.size() + j.previous.size();
    const VkDeviceSize bytes = std::max<VkDeviceSize>(matrices * sizeof(glm::mat4), sizeof(glm::mat4));
    if (bytes > s.capacity) {
        rtc::destroyHostBuffer(m_device, s.palettes);   // the slot's fence was waited: unused
        const VkDeviceSize cap = std::max({bytes, s.capacity * 2, kMinPaletteBytes});
        if (!rtc::createHostBuffer(m_device, m_physical, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s.palettes)) {
            s.capacity = 0;
            OHAO_LOG(Error, render, "skin: palette buffer allocation failed ({} bytes)", cap);
            return false;
        }
        s.capacity = cap;
    }
    auto* out = static_cast<glm::mat4*>(s.palettes.mapped);
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

    const std::array<VkDescriptorBufferInfo, kBindings> infos{{
        {vertexArena, 0, VK_WHOLE_SIZE},
        {s.palettes.buffer, 0, VK_WHOLE_SIZE},
        {meshletArena, 0, VK_WHOLE_SIZE},
        {meshletVertexArena, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        writes[i] = rtc::descriptorWrite(s.set, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, kBindings, writes.data(), 0, nullptr);
    return true;
}

void SkinPass::recordSkin(VkCommandBuffer cmd, uint32_t slotIndex, std::span<const Job> jobs) {
    const Slot& s = m_slots[slotIndex];
    // Earlier frames read these ranges (draws, traces, BLAS builds) and wrote them: this frame's
    // writes come after both. The palettes are host writes made before the submit.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0,
                         nullptr, 0, nullptr);
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
    // The deformed vertices to their readers: the bounds pass, the BLAS refit, the draws, the traces.
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                          VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0,
                         nullptr, 0, nullptr);
}

void SkinPass::recordBounds(VkCommandBuffer cmd, uint32_t slotIndex, std::span<const Job> jobs) {
    const Slot& s = m_slots[slotIndex];
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
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0,
                         nullptr, 0, nullptr);
}

} // namespace ohao::render
