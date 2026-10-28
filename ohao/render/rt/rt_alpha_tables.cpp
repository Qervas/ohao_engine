#include "render/rt/rt_alpha_tables.hpp"

#include "gpu/shared_layout.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace ohao::rtc {

namespace {

constexpr VkDeviceSize kSmokeBytes = gpu::shared::kSmokeSpheresOffset + gpu::shared::kSmokeSpheresMax * gpu::shared::kSmokeSphereStride;
static_assert(sizeof(gpu::shared::SmokeSphereGpu) == gpu::shared::kSmokeSphereStride);

} // namespace

bool AlphaTables::init(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physical = physicalDevice;
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

    for (HostBuffer& buf : m_smoke)
        if (!createHostBuffer(device, physicalDevice, kSmokeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buf)) return false;

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
    for (HostBuffer& buf : m_smoke) destroyHostBuffer(m_device, buf);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_layout) vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_sets = {};
    m_device = VK_NULL_HANDLE;
    m_physical = VK_NULL_HANDLE;
}

// The header (count, enabled) and the spheres, as the shader reads them (rt/smoke_proxy.slang). More
// spheres than the buffer holds are not written (the set's caps keep that from happening).
void AlphaTables::writeSmoke(uint32_t slot, const render::RenderScene& scene, bool smokeInGi) {
    auto* bytes = static_cast<uint8_t*>(m_smoke[slot].mapped);
    if (!bytes) return;
    const auto spheres = scene.vfx().smoke;
    const auto count = static_cast<uint32_t>(std::min<std::size_t>(spheres.size(), gpu::shared::kSmokeSpheresMax));
    const uint32_t header[2] = {count, smokeInGi ? 1u : 0u};
    std::memcpy(bytes, header, sizeof(header));
    for (uint32_t i = 0; i < count; ++i) {
        const vfx::SmokeSphere& s = spheres[i];
        const float sphere[5] = {s.centre.x, s.centre.y, s.centre.z, s.radius, s.density};
        std::memcpy(bytes + gpu::shared::kSmokeSpheresOffset + i * gpu::shared::kSmokeSphereStride, sphere, sizeof(sphere));
    }
}

bool AlphaTables::update(uint32_t slot, const render::RenderScene& scene, bool smokeInGi) {
    if (slot >= m_sets.size() || m_sets[slot] == VK_NULL_HANDLE) return false;
    writeSmoke(slot, scene, smokeInGi);
    // rt/alpha_test.slang's set 1, in binding order.
    const std::array<VkBuffer, kSceneBindings> buffers{
        scene.instanceBuffer(), scene.materialRefBuffer(), scene.materialBuffer(), scene.meshTable(),
        scene.triangleSlotBuffer(), scene.indexArena(), scene.vertexArena(),
    };
    std::array<VkDescriptorBufferInfo, kBindings> infos{};
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (uint32_t i = 0; i < kSceneBindings; ++i) {
        if (buffers[i] == VK_NULL_HANDLE) return false;
        infos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
    }
    infos[kSceneBindings] = {m_smoke[slot].buffer, 0, VK_WHOLE_SIZE};
    for (uint32_t i = 0; i < kBindings; ++i) {
        writes[i] = descriptorWrite(m_sets[slot], i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, kBindings, writes.data(), 0, nullptr);
    return true;
}

} // namespace ohao::rtc
