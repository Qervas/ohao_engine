#include "render/rt/emissive_lights.hpp"
#include "render/shader_bindings/rt_emissive_collect.hpp"
#include "render/shader_bindings/rt_emissive_reduce.hpp"
#include "render/rt/rt_compute.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace ohao::render {

namespace {

constexpr uint32_t kMaxLevels = 16;   // a 32768-texel side: a billion lights

}  // namespace

bool EmissiveLights::initialize(VkDevice device, GpuAllocator& allocator, VkDescriptorSetLayout texturesLayout) {
    m_device = device;
    m_allocator = &allocator;
    if (texturesLayout == VK_NULL_HANDLE) return false;

    const VkDescriptorSetLayoutBinding collect[] = {
        shader::layoutBinding(shader::rt_emissive_collect::vertices), shader::layoutBinding(shader::rt_emissive_collect::indices),
        shader::layoutBinding(shader::rt_emissive_collect::meshes), shader::layoutBinding(shader::rt_emissive_collect::triangleSlots),
        shader::layoutBinding(shader::rt_emissive_collect::materials), shader::layoutBinding(shader::rt_emissive_collect::instances),
        shader::layoutBinding(shader::rt_emissive_collect::materialRefs), shader::layoutBinding(shader::rt_emissive_collect::ranges),
        shader::layoutBinding(shader::rt_emissive_collect::lights), shader::layoutBinding(shader::rt_emissive_collect::power),
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 10;
    li.pBindings = collect;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_collectSetLayout) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding reduce[] = {shader::layoutBinding(shader::rt_emissive_reduce::below),
                                                   shader::layoutBinding(shader::rt_emissive_reduce::above)};
    li.bindingCount = 2;
    li.pBindings = reduce;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_reduceSetLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_FRAMES_IN_FLIGHT + 2 * kMaxLevels},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT + kMaxLevels;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_collectSetLayout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &ai, m_collectSets.data()) != VK_SUCCESS) return false;

    const VkDescriptorSetLayout collectSets[2] = {m_collectSetLayout, texturesLayout};
    if (!rtc::createComputePipeline(device, "rt_emissive_collect.slang.spv", "main", collectSets, 0, m_collectLayout,
                                    m_collect) ||
        !rtc::createComputePipeline(device, "rt_emissive_reduce.slang.spv", "main", m_reduceSetLayout, 0, m_reduceLayout,
                                    m_reduce)) {
        OHAO_LOG(Error, render, "emissive lights: the pipelines could not be created");
        return false;
    }

    // The smallest resources, so every binding is valid with no emissive light.
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        m_ranges[s] = rtc::createHostBuffer(allocator, 4 * sizeof(uint32_t) * 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        m_first[s] = rtc::createHostBuffer(allocator, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!m_ranges[s].isValid() || !m_first[s].isValid()) return false;
        m_rangeCapacity[s] = 3;
        m_firstCapacity[s] = 1;
        const uint32_t none = ~0u;
        std::memcpy(m_first[s].getMappedData(), &none, sizeof(none));
        allocator.flushBuffer(m_first[s]);
        m_firstVersion[s] = ~0ull;
    }
    return createLights(1) && createPower(1);
}

bool EmissiveLights::createLights(uint32_t capacity) {
    m_allocator->retire(m_lights);
    m_lightCapacity = 0;
    m_lights = m_allocator->createBuffer(VkDeviceSize{capacity} * 4 * sizeof(float) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         AllocationUsage::GpuOnly);
    if (!m_lights.isValid()) return false;
    m_lightCapacity = capacity;
    return true;
}

void EmissiveLights::destroyPower() {
    if (!m_reduceSets.empty()) {
        vkFreeDescriptorSets(m_device, m_pool, static_cast<uint32_t>(m_reduceSets.size()), m_reduceSets.data());
        m_reduceSets.clear();
    }
    m_allocator->retire(m_power);
    m_side = m_levels = 0;
}

bool EmissiveLights::createPower(uint32_t side) {
    destroyPower();
    uint32_t levels = 1;
    while ((1u << (levels - 1)) < side) ++levels;
    m_power = m_allocator->createImage({"emissive.power", VK_FORMAT_R32_SFLOAT, side, side,
                                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, levels});
    if (!m_power) return false;
    m_side = side;
    m_levels = levels;

    // One reduce set a level above 0: level - 1 into level.
    if (levels > 1) {
        m_reduceSets.resize(levels - 1);
        std::vector<VkDescriptorSetLayout> layouts(levels - 1, m_reduceSetLayout);
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = m_pool;
        sai.descriptorSetCount = levels - 1;
        sai.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(m_device, &sai, m_reduceSets.data()) != VK_SUCCESS) return false;
        for (uint32_t l = 1; l < levels; ++l) {
            const VkDescriptorImageInfo below{VK_NULL_HANDLE, m_power.subView(l - 1, 0), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo above{VK_NULL_HANDLE, m_power.subView(l, 0), VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[2] = {shader::write(shader::rt_emissive_reduce::below, m_reduceSets[l - 1]),
                                         shader::write(shader::rt_emissive_reduce::above, m_reduceSets[l - 1])};
            w[0].pImageInfo = &below;
            w[1].pImageInfo = &above;
            vkUpdateDescriptorSets(m_device, 2, w, 0, nullptr);
        }
    }
    return true;
}

void EmissiveLights::destroy() {
    if (!m_device) return;
    destroyPower();
    m_allocator->retire(m_lights);
    for (auto& b : m_ranges) m_allocator->retire(b);
    for (auto& b : m_first) m_allocator->retire(b);
    for (VkPipeline* p : {&m_collect, &m_reduce})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_collectLayout, &m_reduceLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (m_pool) { vkDestroyDescriptorPool(m_device, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
    for (VkDescriptorSetLayout* l : {&m_collectSetLayout, &m_reduceSetLayout})
        if (*l) { vkDestroyDescriptorSetLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    m_device = VK_NULL_HANDLE;
}

void EmissiveLights::prepare(uint32_t frameSlot, const std::vector<RenderScene::EmissiveInstance>& instances,
                             uint64_t version, uint32_t slotEnd, float analyticPower) {
    if (!m_device || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    uint64_t total = 0;
    for (const auto& e : instances) total += e.triangles;
    m_count = static_cast<uint32_t>(std::min<uint64_t>(total, (1ull << (2 * (kMaxLevels - 1)))));

    // Device resources only grow: the power image's side is the one every draw descends.
    uint32_t side = 1;
    while (uint64_t{side} * side < m_count) side *= 2;
    if (m_count > m_lightCapacity || side > m_side) {
        vkDeviceWaitIdle(m_device);   // frames in flight use the reduce sets createPower frees and rewrites
        if (m_count > m_lightCapacity && !createLights(std::max(m_count, m_lightCapacity * 2))) {
            OHAO_LOG(Error, render, "emissive lights: {} lights could not be allocated", m_count);
            m_count = 0;
        }
        if (side > m_side && !createPower(side)) {
            OHAO_LOG(Error, render, "emissive lights: the power image ({}^2) could not be allocated", side);
            m_count = 0;
        }
    }
    m_frameSide = m_side;
    m_frameLevels = m_levels;

    // This slot's ranges (its fence was waited: the GPU is done with them).
    const uint32_t R = static_cast<uint32_t>(instances.size());
    if (R + 2 > m_rangeCapacity[frameSlot]) {
        m_allocator->destroyBuffer(m_ranges[frameSlot]);
        const uint32_t cap = std::max(R + 2, m_rangeCapacity[frameSlot] * 2);
        m_ranges[frameSlot] =
            rtc::createHostBuffer(*m_allocator, VkDeviceSize{cap} * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        m_rangeCapacity[frameSlot] = cap;
    }
    auto* r = static_cast<uint32_t*>(m_ranges[frameSlot].getMappedData());
    r[0] = m_count;
    r[1] = m_frameSide;
    r[2] = m_frameLevels;
    r[3] = m_count > 0 ? R : 0;
    std::memcpy(&r[4], &analyticPower, sizeof(float));
    r[5] = r[6] = r[7] = 0;
    uint32_t first = 0;
    for (uint32_t k = 0; k < R; ++k) {
        r[8 + 4 * k] = instances[k].slot;
        r[9 + 4 * k] = first;
        r[10 + 4 * k] = instances[k].triangles;
        r[11 + 4 * k] = 0;
        first += instances[k].triangles;
    }
    m_allocator->flushBuffer(m_ranges[frameSlot]);

    // This slot's first light a slot, rewritten when the set or the slot count changed.
    const uint32_t slots = std::max(slotEnd, 1u);
    if (m_firstVersion[frameSlot] != version || m_firstSlotEnd[frameSlot] != slots) {
        if (slots > m_firstCapacity[frameSlot]) {
            m_allocator->destroyBuffer(m_first[frameSlot]);
            const uint32_t cap = std::max(slots, m_firstCapacity[frameSlot] * 2);
            m_first[frameSlot] =
                rtc::createHostBuffer(*m_allocator, VkDeviceSize{cap} * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            m_firstCapacity[frameSlot] = cap;
        }
        auto* f = static_cast<uint32_t*>(m_first[frameSlot].getMappedData());
        std::fill(f, f + m_firstCapacity[frameSlot], ~0u);
        uint32_t at = 0;
        for (const auto& e : instances) {
            if (e.slot < m_firstCapacity[frameSlot] && m_count > 0) f[e.slot] = at;
            at += e.triangles;
        }
        m_allocator->flushBuffer(m_first[frameSlot]);
        m_firstVersion[frameSlot] = version;
        m_firstSlotEnd[frameSlot] = slots;
    }
}

void EmissiveLights::prepare(uint32_t frameSlot, const RenderScene& scene) {
    prepare(frameSlot, scene.emissiveInstances(), scene.emissiveVersion(), scene.instanceSlotEnd(),
            scene.analyticLightPower());
}

void EmissiveLights::record(const Frame& frame, const RenderScene& scene, VkDescriptorSet textures) {
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t frameSlot = frame.slot;
    if (!m_device || frameSlot >= MAX_FRAMES_IN_FLIGHT || !m_power) return;
    if (m_count == 0 || textures == VK_NULL_HANDLE) return;   // the header says none: nothing reads the image or lights

    const RenderScene::Buffers sb = scene.buffers();
    GpuBuffer* const sceneBuffers[7] = {&sb.vertices,  &sb.indices,   &sb.meshes,      &sb.triangleSlots,
                                        &sb.materials, &sb.instances, &sb.materialRefs};
    const VkDescriptorSet set = m_collectSets[frameSlot];
    VkDescriptorBufferInfo infos[9];
    for (uint32_t i = 0; i < 7; ++i) infos[i] = {sceneBuffers[i]->buffer, 0, VK_WHOLE_SIZE};
    infos[7] = {m_ranges[frameSlot].buffer, 0, VK_WHOLE_SIZE};
    infos[8] = {m_lights.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[10];
    for (uint32_t i = 0; i < 9; ++i) {
        w[i] = shader::write(shader::rt_emissive_collect::kSet0[i], set);
        w[i].pBufferInfo = &infos[i];
    }
    const VkDescriptorImageInfo level0{VK_NULL_HANDLE, m_power.subView(0, 0), VK_IMAGE_LAYOUT_GENERAL};
    w[9] = shader::write(shader::rt_emissive_collect::power, set);
    w[9].pImageInfo = &level0;
    vkUpdateDescriptorSets(m_device, 10, w, 0, nullptr);

    {
        // Every triangle in world space and its power into level 0.
        Pass pass(frame, nullptr, Pass::Compute);
        for (GpuBuffer* b : sceneBuffers) pass.read(*b, BufferUse::Storage);
        pass.read(m_ranges[frameSlot], BufferUse::Storage).write(m_lights, BufferUse::Storage);
        pass.storageWrite(m_power, Old::Keep, {0, 0});
        pass.begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_collect);
        const VkDescriptorSet sets[2] = {set, textures};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_collectLayout, 0, 2, sets, 0, nullptr);
        vkCmdDispatch(cmd, (m_frameSide * m_frameSide + 63) / 64, 1, 1);
    }

    // Each level above the sum of the one below.
    for (uint32_t l = 1; l < m_frameLevels; ++l) {
        Pass pass(frame, nullptr, Pass::Compute);
        pass.storageRead(m_power, {l - 1, 0}).storageWrite(m_power, Old::Keep, {l, 0});
        pass.begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_reduce);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_reduceLayout, 0, 1, &m_reduceSets[l - 1], 0, nullptr);
        const uint32_t side = std::max(m_frameSide >> l, 1u);
        vkCmdDispatch(cmd, (side + 7) / 8, (side + 7) / 8, 1);
    }
}

}  // namespace ohao::render
