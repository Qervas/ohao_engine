#include "light_clusters.hpp"
#include "render/shader_bindings/compute_light_cluster.hpp"
#include "render/rt/rt_compute.hpp"
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

bool LightClusters::initialize(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;
    const VkDescriptorSetLayoutBinding bindings[] = {shader::layoutBinding(shader::compute_light_cluster::lights),
                                                     shader::layoutBinding(shader::compute_light_cluster::clusters),
                                                     shader::layoutBinding(shader::compute_light_cluster::stats)};
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

    // The froxel lists, device-local.
    m_clusters = allocator.createBuffer(
        kClusterBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        AllocationUsage::GpuOnly);
    if (!m_clusters.isValid()) return false;

    for (Slot& s : m_slots) {
        // Host-coherent memory without a preference, as the shader writes it: in host-cached memory
        // (AllocationUsage::GpuToCpu) the build measured 0.3 ms, here 0.02.
        s.stats = allocator.createBuffer(kStatsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::CpuOnly, true);
        if (!s.stats.isValid() || !s.stats.getMappedData()) return false;
    }
    return true;
}

void LightClusters::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    for (Slot& s : m_slots) {
        m_allocator->retire(s.lights);
        m_allocator->retire(s.stats);
        s = Slot{};
    }
    m_allocator->retire(m_captureBuffer);
    m_allocator->retire(m_clusters);
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
    if (count > s.capacity || !s.lights.buffer) {
        uint32_t cap = std::max(s.capacity, 64u);
        while (cap < count) cap *= 2;
        m_allocator->retire(s.lights);
        s.capacity = 0;
        s.lights = m_allocator->createBuffer(VkDeviceSize{cap} * sizeof(shared::LightGpu),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::CpuToGpu, true);
        if (!s.lights.isValid() || !s.lights.getMappedData()) {
            OHAO_LOG(Error, render, "LightClusters: a light buffer of {} lights failed", cap);
            m_allocator->retire(s.lights);
            s.count = 0;
            return;
        }
        s.capacity = cap;
    }
    // The slot's fence was waited on: its frame no longer reads the buffer.
    auto* dst = static_cast<shared::LightGpu*>(s.lights.getMappedData());
    for (uint32_t i = 0; i < count; ++i) dst[i] = toGpu(lights[i]);
    m_allocator->flushBuffer(s.lights, 0, VkDeviceSize{count} * sizeof(shared::LightGpu));
    s.count = count;
    s.version = version;
}

void LightClusters::build(const render::Frame& frame, const glm::mat4& view, const glm::mat4& proj, uint32_t width,
                          uint32_t height, float farPlane, uint64_t frameNumber) {
    render::Timed timed(frame, "lights.cluster");
    const uint32_t slot = frame.slot;
    if (m_pipeline == VK_NULL_HANDLE || slot >= MAX_FRAMES_IN_FLIGHT) return;
    Slot& s = m_slots[slot];
    if (!s.lights.buffer) return;
    collectSlot(slot);   // the slot's last frame finished at its fence
    const VkCommandBuffer cmd = frame.cmd;

    const float farD = std::max(farPlane, shared::kClusterNear * 2.0f);
    m_sliceScale = static_cast<float>(shared::kClusterSlices) / std::log(farD / shared::kClusterNear);
    m_sliceBias = -std::log(shared::kClusterNear) * m_sliceScale;

    const VkDescriptorBufferInfo infos[3] = {{s.lights.buffer, 0, VK_WHOLE_SIZE},
                                             {m_clusters.buffer, 0, VK_WHOLE_SIZE},
                                             {s.stats.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = shader::write(shader::compute_light_cluster::kSet0[i], m_sets[slot]);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
    std::memset(s.stats.getMappedData(), 0, kStatsBytes);   // read back after the fence
    m_allocator->flushBuffer(s.stats);
    s.pending = true;
    s.frame = frameNumber;

    shared::ClusterPush pc{};
    std::memcpy(&pc.view, &view, sizeof(view));
    pc.proj = {proj[0][0], proj[1][1], proj[2][0], proj[2][1]};
    pc.screen = {static_cast<float>(width), static_cast<float>(height), m_sliceScale, m_sliceBias};
    pc.config = {s.count, 0u, 0u, 0u};

    {
        render::Pass pass(frame, nullptr, render::Pass::Compute);
        pass.read(s.lights, render::BufferUse::Storage)
            .write(m_clusters, render::BufferUse::Storage)
            .readWrite(s.stats, render::BufferUse::Storage);
        pass.begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_sets[slot], 0, nullptr);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, shared::kClusterCount, 1, 1);
    }
    {
        render::Pass toHost(frame, nullptr, render::Pass::Transfer);
        toHost.read(s.stats, render::BufferUse::HostRead);
        toHost.begin();
    }

    if (m_captureRequested) {
        m_captured = false;
        if (!m_captureBuffer.buffer) {
            m_captureBuffer =
                m_allocator->createBuffer(kClusterBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, AllocationUsage::GpuToCpu, true);
            if (!m_captureBuffer.isValid() || !m_captureBuffer.getMappedData()) {
                m_allocator->retire(m_captureBuffer);
                return;
            }
        }
        {
            render::Pass copy(frame, nullptr, render::Pass::Transfer);
            copy.read(m_clusters, render::BufferUse::CopySrc).write(m_captureBuffer, render::BufferUse::CopyDst);
            copy.begin();
            const VkBufferCopy region{0, 0, kClusterBytes};
            vkCmdCopyBuffer(cmd, m_clusters.buffer, m_captureBuffer.buffer, 1, &region);
        }
        render::Pass toHost(frame, nullptr, render::Pass::Transfer);
        toHost.read(m_captureBuffer, render::BufferUse::HostRead);
        toHost.begin();
        m_captured = true;
    }
}

void LightClusters::collectSlot(uint32_t slot) {
    Slot& s = m_slots[slot];
    if (!s.pending || !s.stats.getMappedData()) return;
    m_allocator->invalidateBuffer(s.stats);
    uint32_t words[3];
    std::memcpy(words, s.stats.getMappedData(), sizeof(words));
    if (!m_latest || s.frame >= m_latest->frame) m_latest = Overflow{s.frame, words[0], words[1], words[2]};
    s.pending = false;
}

std::vector<uint32_t> LightClusters::capture() const {
    if (!m_captured || !m_captureBuffer.getMappedData()) return {};
    const auto* words = static_cast<const uint32_t*>(m_captureBuffer.getMappedData());
    return std::vector<uint32_t>(words, words + kClusterBytes / sizeof(uint32_t));
}

} // namespace ohao
