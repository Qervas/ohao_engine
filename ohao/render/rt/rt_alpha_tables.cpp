#include "render/rt/rt_alpha_tables.hpp"

#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

namespace ohao::rtc {

bool AlphaTables::init(VkDevice device) {
    m_device = device;
    std::array<VkDescriptorSetLayoutBinding, kBindings> b{};
    for (uint32_t i = 0; i < kBindings; ++i)
        b[i] = layoutBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindings;
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_layout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(m_layout);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(device, &ai, m_sets.data()) == VK_SUCCESS;
}

void AlphaTables::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_layout) vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_sets = {};
    m_device = VK_NULL_HANDLE;
}

bool AlphaTables::update(uint32_t slot, const render::RenderScene& scene) {
    if (slot >= m_sets.size() || m_sets[slot] == VK_NULL_HANDLE) return false;
    // rt/alpha_test.slang's set 1, in binding order.
    const std::array<VkBuffer, kBindings> buffers{
        scene.instanceBuffer(), scene.materialRefBuffer(), scene.materialBuffer(), scene.meshTable(),
        scene.triangleSlotBuffer(), scene.indexArena(), scene.vertexArena(),
    };
    std::array<VkDescriptorBufferInfo, kBindings> infos{};
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        if (buffers[i] == VK_NULL_HANDLE) return false;
        infos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
        writes[i] = descriptorWrite(m_sets[slot], i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, kBindings, writes.data(), 0, nullptr);
    return true;
}

} // namespace ohao::rtc
