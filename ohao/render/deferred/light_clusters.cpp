#include "light_clusters.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ohao {

namespace shared = gpu::shared;

static_assert(sizeof(shared::LightGpu) == 64);
static_assert(sizeof(shared::ClusterPush) <= 128);
static_assert(shared::kClusterCount == shared::kClusterTilesX * shared::kClusterTilesY * shared::kClusterSlices);

namespace {

constexpr VkDeviceSize kClusterBytes =
    VkDeviceSize{shared::kClusterCount} * (1 + shared::kClusterMaxLights) * sizeof(uint32_t);
constexpr VkDeviceSize kStatsBytes = 4 * sizeof(uint32_t);

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                   VkAccessFlags dstAccess) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

shared::LightGpu toGpu(const LightData& l) {
    shared::LightGpu g{};
    std::memcpy(g.position.data(), &l.position, sizeof(g.position));
    std::memcpy(g.direction.data(), &l.direction, sizeof(g.direction));
    std::memcpy(g.color.data(), &l.color, sizeof(g.color));
    std::memcpy(g.params.data(), &l.params, sizeof(g.params));
    return g;
}

} // namespace

LightClusters::~LightClusters() {
    cleanup();
}

bool LightClusters::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    using rtc::layoutBinding;
    const VkDescriptorSetLayoutBinding bindings[] = {layoutBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                                     layoutBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                                     layoutBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    for (VkDescriptorSet& set : m_sets) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) return false;
    }
    if (!rtc::createComputePipeline(m_device, "compute_light_cluster.slang.spv", "build", m_setLayout,
                                    sizeof(shared::ClusterPush), m_layout, m_pipeline)) {
        OHAO_LOG(Error, render, "LightClusters: lights.cluster pipeline failed");
        return false;
    }

    // The froxel lists, device-local; counts start at zero so a frame that never builds reads
    // empty lists.
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kClusterBytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &bi, nullptr, &m_clusters.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, m_clusters.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &m_clusters.memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(m_device, m_clusters.buffer, m_clusters.memory, 0) != VK_SUCCESS) return false;

    for (Slot& s : m_slots)
        if (!rtc::createHostBuffer(m_device, m_physicalDevice, kStatsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s.stats))
            return false;
    return true;
}

void LightClusters::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    for (Slot& s : m_slots) {
        rtc::destroyHostBuffer(m_device, s.lights);
        rtc::destroyHostBuffer(m_device, s.stats);
        s = Slot{};
    }
    rtc::destroyHostBuffer(m_device, m_captureBuffer);
    if (m_clusters.buffer) vkDestroyBuffer(m_device, m_clusters.buffer, nullptr);
    if (m_clusters.memory) vkFreeMemory(m_device, m_clusters.memory, nullptr);
    m_clusters = {};
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_device, m_layout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sets = {};
    m_device = VK_NULL_HANDLE;
}

void LightClusters::setLights(uint32_t slot, std::span<const LightData> lights, uint64_t version) {
    if (m_device == VK_NULL_HANDLE || slot >= MAX_FRAMES_IN_FLIGHT) return;
    Slot& s = m_slots[slot];
    std::size_t n = lights.size();
    if (n > shared::kLightCapacity) {
        static bool logged = false;
        if (!logged) {
            OHAO_LOG(Warn, render, "LightClusters: {} raster lights: only the first {} are drawn", n,
                     shared::kLightCapacity);
            logged = true;
        }
        n = shared::kLightCapacity;
    }
    const auto count = static_cast<uint32_t>(n);
    if (version != 0 && s.version == version && s.count == count && s.lights.buffer) return;
    // The slot's fence was waited on: its frame no longer reads the old buffer.
    if (count > s.capacity || !s.lights.buffer) {
        uint32_t cap = std::max(s.capacity, 64u);
        while (cap < count) cap *= 2;
        rtc::destroyHostBuffer(m_device, s.lights);
        s.capacity = 0;
        if (!rtc::createHostBuffer(m_device, m_physicalDevice, VkDeviceSize{cap} * sizeof(shared::LightGpu),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s.lights)) {
            OHAO_LOG(Error, render, "LightClusters: a light buffer of {} lights failed", cap);
            s.count = 0;
            return;
        }
        s.capacity = cap;
    }
    auto* dst = static_cast<shared::LightGpu*>(s.lights.mapped);
    for (uint32_t i = 0; i < count; ++i) dst[i] = toGpu(lights[i]);
    s.count = count;
    s.version = version;
}

void LightClusters::build(VkCommandBuffer cmd, uint32_t slot, const glm::mat4& view, const glm::mat4& proj,
                          uint32_t width, uint32_t height, float farPlane, uint64_t frameNumber) {
    if (m_pipeline == VK_NULL_HANDLE || slot >= MAX_FRAMES_IN_FLIGHT) return;
    Slot& s = m_slots[slot];
    if (!s.lights.buffer) return;
    collectSlot(slot);   // the slot's last frame finished at its fence

    const float farD = std::max(farPlane, shared::kClusterNear * 2.0f);
    m_sliceScale = static_cast<float>(shared::kClusterSlices) / std::log(farD / shared::kClusterNear);
    m_sliceBias = -std::log(shared::kClusterNear) * m_sliceScale;

    const VkDescriptorBufferInfo infos[3] = {{s.lights.buffer, 0, VK_WHOLE_SIZE},
                                             {m_clusters.buffer, 0, VK_WHOLE_SIZE},
                                             {s.stats.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = rtc::descriptorWrite(m_sets[slot], i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
    std::memset(s.stats.mapped, 0, kStatsBytes);   // host-coherent; read back after the fence
    s.pending = true;
    s.frame = frameNumber;

    shared::ClusterPush pc{};
    std::memcpy(&pc.view, &view, sizeof(view));
    pc.proj = {proj[0][0], proj[1][1], proj[2][0], proj[2][1]};
    pc.screen = {static_cast<float>(width), static_cast<float>(height), m_sliceScale, m_sliceBias};
    pc.config = {s.count, 0u, 0u, 0u};

    // The last frame's lighting read the lists before this rewrite (queue order).
    memoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_sets[slot], 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, shared::kClusterCount, 1, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT);

    if (m_captureRequested) {
        m_captured = false;
        if (!m_captureBuffer.buffer &&
            !rtc::createHostBuffer(m_device, m_physicalDevice, kClusterBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   m_captureBuffer))
            return;
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{0, 0, kClusterBytes};
        vkCmdCopyBuffer(cmd, m_clusters.buffer, m_captureBuffer.buffer, 1, &region);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                      VK_ACCESS_HOST_READ_BIT);
        m_captured = true;
    }
}

void LightClusters::collectSlot(uint32_t slot) {
    Slot& s = m_slots[slot];
    if (!s.pending || !s.stats.mapped) return;
    uint32_t words[3];
    std::memcpy(words, s.stats.mapped, sizeof(words));
    if (!m_latest || s.frame >= m_latest->frame) m_latest = Overflow{s.frame, words[0], words[1], words[2]};
    s.pending = false;
}

std::vector<uint32_t> LightClusters::capture() const {
    if (!m_captured || !m_captureBuffer.mapped) return {};
    const auto* words = static_cast<const uint32_t*>(m_captureBuffer.mapped);
    return std::vector<uint32_t>(words, words + kClusterBytes / sizeof(uint32_t));
}

} // namespace ohao
