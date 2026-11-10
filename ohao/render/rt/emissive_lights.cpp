#include "render/rt/emissive_lights.hpp"
#include "render/shader_bindings/rt_emissive_collect.hpp"
#include "render/shader_bindings/rt_emissive_reduce.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace ohao::render {

namespace {

constexpr uint32_t kMaxLevels = 16;   // a 32768-texel side: a billion lights

VkMemoryBarrier memoryBarrier(VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    return b;
}

bool createDeviceBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size, VkBuffer& buffer,
                        VkDeviceMemory& memory) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, buffer, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = rtc::findMemoryType(physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    return vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
}

}  // namespace

bool EmissiveLights::initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkDescriptorSetLayout texturesLayout,
                                bool rayTracingPipeline) {
    m_device = device;
    m_physical = physicalDevice;
    m_readers = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                (rayTracingPipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR : VkPipelineStageFlags{0});
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
        if (!rtc::createHostBuffer(device, physicalDevice, 4 * sizeof(uint32_t) * 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   m_ranges[s]) ||
            !rtc::createHostBuffer(device, physicalDevice, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_first[s]))
            return false;
        m_rangeCapacity[s] = 3;
        m_firstCapacity[s] = 1;
        std::memset(m_ranges[s].mapped, 0, 4 * sizeof(uint32_t) * 3);
        const uint32_t none = ~0u;
        std::memcpy(m_first[s].mapped, &none, sizeof(none));
        m_firstVersion[s] = ~0ull;
    }
    return createLights(1) && createPower(1);
}

bool EmissiveLights::createLights(uint32_t capacity) {
    if (m_lights) vkDestroyBuffer(m_device, m_lights, nullptr);
    if (m_lightsMemory) vkFreeMemory(m_device, m_lightsMemory, nullptr);
    m_lights = VK_NULL_HANDLE;
    m_lightsMemory = VK_NULL_HANDLE;
    m_lightCapacity = 0;
    if (!createDeviceBuffer(m_device, m_physical, VkDeviceSize{capacity} * 4 * sizeof(float) * 4, m_lights, m_lightsMemory))
        return false;
    m_lightCapacity = capacity;
    return true;
}

void EmissiveLights::destroyPower() {
    if (!m_reduceSets.empty()) {
        vkFreeDescriptorSets(m_device, m_pool, static_cast<uint32_t>(m_reduceSets.size()), m_reduceSets.data());
        m_reduceSets.clear();
    }
    for (VkImageView v : m_levelViews) vkDestroyImageView(m_device, v, nullptr);
    m_levelViews.clear();
    if (m_powerView) vkDestroyImageView(m_device, m_powerView, nullptr);
    if (m_power) vkDestroyImage(m_device, m_power, nullptr);
    if (m_powerMemory) vkFreeMemory(m_device, m_powerMemory, nullptr);
    m_powerView = VK_NULL_HANDLE;
    m_power = VK_NULL_HANDLE;
    m_powerMemory = VK_NULL_HANDLE;
    m_side = m_levels = 0;
}

bool EmissiveLights::createPower(uint32_t side) {
    destroyPower();
    uint32_t levels = 1;
    while ((1u << (levels - 1)) < side) ++levels;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R32_SFLOAT;
    ici.extent = {side, side, 1};
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &m_power) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(m_device, m_power, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = rtc::findMemoryType(m_physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &m_powerMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_power, m_powerMemory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_power;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (vkCreateImageView(m_device, &vi, nullptr, &m_powerView) != VK_SUCCESS) return false;
    m_levelViews.resize(levels);
    for (uint32_t l = 0; l < levels; ++l) {
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1};
        if (vkCreateImageView(m_device, &vi, nullptr, &m_levelViews[l]) != VK_SUCCESS) return false;
    }
    m_side = side;
    m_levels = levels;
    m_powerFresh = true;

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
            const VkDescriptorImageInfo below{VK_NULL_HANDLE, m_levelViews[l - 1], VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo above{VK_NULL_HANDLE, m_levelViews[l], VK_IMAGE_LAYOUT_GENERAL};
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
    if (m_lights) vkDestroyBuffer(m_device, m_lights, nullptr);
    if (m_lightsMemory) vkFreeMemory(m_device, m_lightsMemory, nullptr);
    m_lights = VK_NULL_HANDLE;
    m_lightsMemory = VK_NULL_HANDLE;
    for (auto& b : m_ranges) rtc::destroyHostBuffer(m_device, b);
    for (auto& b : m_first) rtc::destroyHostBuffer(m_device, b);
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
        vkDeviceWaitIdle(m_device);   // frames in flight read the old ones
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
        rtc::destroyHostBuffer(m_device, m_ranges[frameSlot]);
        const uint32_t cap = std::max(R + 2, m_rangeCapacity[frameSlot] * 2);
        rtc::createHostBuffer(m_device, m_physical, VkDeviceSize{cap} * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              m_ranges[frameSlot]);
        m_rangeCapacity[frameSlot] = cap;
    }
    auto* r = static_cast<uint32_t*>(m_ranges[frameSlot].mapped);
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

    // This slot's first light a slot, rewritten when the set or the slot count changed.
    const uint32_t slots = std::max(slotEnd, 1u);
    if (m_firstVersion[frameSlot] != version || m_firstSlotEnd[frameSlot] != slots) {
        if (slots > m_firstCapacity[frameSlot]) {
            rtc::destroyHostBuffer(m_device, m_first[frameSlot]);
            const uint32_t cap = std::max(slots, m_firstCapacity[frameSlot] * 2);
            rtc::createHostBuffer(m_device, m_physical, VkDeviceSize{cap} * sizeof(uint32_t),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_first[frameSlot]);
            m_firstCapacity[frameSlot] = cap;
        }
        auto* f = static_cast<uint32_t*>(m_first[frameSlot].mapped);
        std::fill(f, f + m_firstCapacity[frameSlot], ~0u);
        uint32_t at = 0;
        for (const auto& e : instances) {
            if (e.slot < m_firstCapacity[frameSlot] && m_count > 0) f[e.slot] = at;
            at += e.triangles;
        }
        m_firstVersion[frameSlot] = version;
        m_firstSlotEnd[frameSlot] = slots;
    }
}

void EmissiveLights::prepare(uint32_t frameSlot, const RenderScene& scene) {
    prepare(frameSlot, scene.emissiveInstances(), scene.emissiveVersion(), scene.instanceSlotEnd(),
            scene.analyticLightPower());
}

EmissiveLights::SceneBuffers EmissiveLights::buffersOf(const RenderScene& scene) {
    return {scene.vertexArena(),    scene.indexArena(),     scene.meshTable(),         scene.triangleSlotBuffer(),
            scene.materialBuffer(), scene.instanceBuffer(), scene.materialRefBuffer()};
}

void EmissiveLights::record(VkCommandBuffer cmd, uint32_t frameSlot, const SceneBuffers& scene, VkDescriptorSet textures) {
    if (!m_device || frameSlot >= MAX_FRAMES_IN_FLIGHT || !m_power) return;
    if (m_powerFresh) {
        // GENERAL for good: the passes write it as storage and the path tracer reads it sampled.
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_power;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_levels, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        m_powerFresh = false;
    }
    if (m_count == 0 || textures == VK_NULL_HANDLE) return;   // the header says none: nothing reads the image or lights

    const VkDescriptorSet set = m_collectSets[frameSlot];
    const VkDescriptorBufferInfo infos[9] = {
        {scene.vertices, 0, VK_WHOLE_SIZE},     {scene.indices, 0, VK_WHOLE_SIZE},
        {scene.meshes, 0, VK_WHOLE_SIZE},       {scene.triangleSlots, 0, VK_WHOLE_SIZE},
        {scene.materials, 0, VK_WHOLE_SIZE},    {scene.instances, 0, VK_WHOLE_SIZE},
        {scene.materialRefs, 0, VK_WHOLE_SIZE}, {m_ranges[frameSlot].buffer, 0, VK_WHOLE_SIZE},
        {m_lights, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet w[10];
    for (uint32_t i = 0; i < 9; ++i) {
        w[i] = shader::write(shader::rt_emissive_collect::kSet0[i], set);
        w[i].pBufferInfo = &infos[i];
    }
    const VkDescriptorImageInfo level0{VK_NULL_HANDLE, m_levelViews[0], VK_IMAGE_LAYOUT_GENERAL};
    w[9] = shader::write(shader::rt_emissive_collect::power, set);
    w[9].pImageInfo = &level0;
    vkUpdateDescriptorSets(m_device, 10, w, 0, nullptr);

    // Last frame's readers read the lights and the image (queue order); this frame rewrites them.
    const VkMemoryBarrier before = memoryBarrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, m_readers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_collect);
    const VkDescriptorSet sets[2] = {set, textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_collectLayout, 0, 2, sets, 0, nullptr);
    vkCmdDispatch(cmd, (m_frameSide * m_frameSide + 63) / 64, 1, 1);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_reduce);
    for (uint32_t l = 1; l < m_frameLevels; ++l) {
        const VkMemoryBarrier level = memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &level, 0,
                             nullptr, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_reduceLayout, 0, 1, &m_reduceSets[l - 1], 0, nullptr);
        const uint32_t side = std::max(m_frameSide >> l, 1u);
        vkCmdDispatch(cmd, (side + 7) / 8, (side + 7) / 8, 1);
    }

    const VkMemoryBarrier after = memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_readers, 0, 1, &after, 0, nullptr, 0, nullptr);
}

}  // namespace ohao::render
