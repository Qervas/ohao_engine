#include "render/frame/gpu_pass_timer.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace ohao::render {

bool GpuPassTimer::init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamily, uint32_t slots,
                        bool markers) {
    destroy();
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.limits.timestampPeriod <= 0.0f) {
        OHAO_LOG(Warn, render, "GPU pass timing off: the device has no timestamps");
        return false;
    }
    // The queue family's valid bits; 0 means no timestamps.
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &families, fam.data());
    const uint32_t bits = queueFamily < fam.size() ? fam[queueFamily].timestampValidBits : 0;
    if (bits == 0) {
        OHAO_LOG(Warn, render, "GPU pass timing off: the graphics queue has no timestamps");
        return false;
    }
    m_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    m_device = device;
    m_msPerTick = static_cast<double>(props.limits.timestampPeriod) / 1.0e6;
    m_slots.resize(slots);
    for (Slot& s : m_slots) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kMaxTimedPasses * 2;
        if (vkCreateQueryPool(device, &info, nullptr, &s.pool) != VK_SUCCESS) {
            OHAO_LOG(Error, render, "GPU pass timing off: vkCreateQueryPool failed");
            destroy();
            return false;
        }
    }
    if (markers) initMarkers(physicalDevice, slots);
    OHAO_LOG(Debug, render, "GPU pass timing: {} slots, {} passes a frame, {} ns a tick", slots, kMaxTimedPasses,
             props.limits.timestampPeriod);
    return true;
}

bool GpuPassTimer::initMarkers(VkPhysicalDevice physicalDevice, uint32_t slots) {
    m_writeMarker = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(
        vkGetDeviceProcAddr(m_device, "vkCmdWriteBufferMarkerAMD"));
    if (!m_writeMarker) return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sizeof(uint32_t) * kMarkerWords * slots;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &bi, nullptr, &m_markerBuffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, m_markerBuffer, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mem);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void* mapped = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(m_device, &ai, nullptr, &m_markerMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, m_markerBuffer, m_markerMemory, 0) != VK_SUCCESS ||
        vkMapMemory(m_device, m_markerMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        OHAO_LOG(Warn, render, "GPU breadcrumbs off: no host-coherent marker buffer");
        destroyMarkers();
        return false;
    }
    std::memset(mapped, 0, static_cast<std::size_t>(bi.size));
    m_markers = static_cast<const volatile uint32_t*>(mapped);
    return true;
}

void GpuPassTimer::destroyMarkers() {
    if (m_markerMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_markerMemory, nullptr);   // unmaps it
    if (m_markerBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_markerBuffer, nullptr);
    m_markerMemory = VK_NULL_HANDLE;
    m_markerBuffer = VK_NULL_HANDLE;
    m_markers = nullptr;
    m_writeMarker = nullptr;
}

void GpuPassTimer::writeMarker(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, uint32_t word) {
    if (!m_markers || m_current < 0) return;
    const Slot& s = m_slots[static_cast<std::size_t>(m_current)];
    const auto offset = sizeof(uint32_t) * (static_cast<std::size_t>(m_current) * kMarkerWords + word);
    m_writeMarker(cmd, stage, m_markerBuffer, offset, static_cast<uint32_t>(s.frame));
}

std::string GpuPassTimer::describe() const {
    if (!m_markers) return R"({"breadcrumbs":"off"})";
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < m_slots.size(); ++i)
        if (m_slots[i].frame > 0) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return m_slots[a].frame < m_slots[b].frame; });
    std::string out = "[";
    for (const uint32_t i : order) {
        const Slot& s = m_slots[i];
        const auto f = static_cast<uint32_t>(s.frame);
        const volatile uint32_t* w = m_markers + static_cast<std::size_t>(i) * kMarkerWords;
        const bool recording = static_cast<int>(i) == m_current;
        const uint32_t count = recording && m_open ? s.count + 1 : s.count;
        std::string finished = "none", running;
        uint32_t started = 0, ended = 0;
        for (uint32_t p = 0; p < count; ++p) {
            const bool began = w[1 + p * 2] == f, done = w[2 + p * 2] == f;
            started += began;
            ended += done;
            const std::string_view name = kPasses[s.pass[p]].name;
            if (done) finished = name;
            else if (began) running += std::string(running.empty() ? "" : ",") + '"' + std::string(name) + '"';
        }
        if (out.size() > 1) out += ',';
        out += std::format(R"({{"frame":{},"submitted":{},"gpuStarted":{},"passes":{},"began":{},"finished":{},)"
                           R"("lastFinished":"{}","unfinished":[{}]}})",
                           s.frame, recording ? "false" : "true", w[0] == f ? "true" : "false", count, started, ended,
                           finished, running);
    }
    return out + "]";
}

void GpuPassTimer::destroy() {
    destroyMarkers();
    for (Slot& s : m_slots)
        if (s.pool != VK_NULL_HANDLE) vkDestroyQueryPool(m_device, s.pool, nullptr);
    m_slots.clear();
    m_current = -1;
    m_open = false;
}

void GpuPassTimer::beginFrame(VkCommandBuffer cmd, uint32_t slot, uint64_t frameNumber) {
    if (slot >= m_slots.size()) return;
    collect(slot);
    Slot& s = m_slots[slot];
    vkCmdResetQueryPool(cmd, s.pool, 0, kMaxTimedPasses * 2);
    s.frame = frameNumber;
    s.count = 0;
    s.pending = false;
    m_current = static_cast<int>(slot);
    m_open = false;
    writeMarker(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
}

void GpuPassTimer::begin(VkCommandBuffer cmd, std::string_view pass) {
    if (m_current < 0) return;
    if (m_open) end(cmd);
    const int index = passIndex(pass);
    Slot& s = m_slots[static_cast<std::size_t>(m_current)];
    if (index < 0 || s.count >= kMaxTimedPasses) {
        static bool warned = false;
        if (!warned) {
            OHAO_LOG(Error, render, "GPU pass timing: '{}' is {}", pass,
                     index < 0 ? "not in the pass registry" : "over the per-frame pass limit");
            warned = true;
        }
        return;
    }
    s.pass[s.count] = static_cast<uint8_t>(index);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, s.count * 2);
    writeMarker(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 1 + s.count * 2);
    m_open = true;
}

void GpuPassTimer::end(VkCommandBuffer cmd) {
    if (m_current < 0 || !m_open) return;
    Slot& s = m_slots[static_cast<std::size_t>(m_current)];
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, s.count * 2 + 1);
    writeMarker(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2 + s.count * 2);
    ++s.count;
    m_open = false;
}

void GpuPassTimer::endFrame(double cpuMs) {
    if (m_current < 0) return;
    Slot& s = m_slots[static_cast<std::size_t>(m_current)];
    s.cpuMs = static_cast<float>(cpuMs);
    s.pending = s.count > 0;
    m_current = -1;
    m_open = false;
}

void GpuPassTimer::collect(uint32_t slot) {
    if (slot >= m_slots.size()) return;
    Slot& s = m_slots[slot];
    if (!s.pending) return;
    s.pending = false;
    std::array<uint64_t, kMaxTimedPasses * 2> t{};
    // The frame's fence has signalled, so WAIT returns at once.
    if (vkGetQueryPoolResults(m_device, s.pool, 0, s.count * 2, sizeof(uint64_t) * s.count * 2, t.data(),
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return;
    FrameTiming& f = m_ring[m_ringNext];
    f.frame = s.frame;
    f.cpuMs = s.cpuMs;
    f.count = s.count;
    for (uint32_t i = 0; i < s.count; ++i) {
        f.pass[i] = s.pass[i];
        const uint64_t ticks = (t[i * 2 + 1] - t[i * 2]) & m_mask;
        f.ms[i] = static_cast<float>(static_cast<double>(ticks) * m_msPerTick);
    }
    f.gpuMs = static_cast<float>(static_cast<double>((t[s.count * 2 - 1] - t[0]) & m_mask) * m_msPerTick);
    m_ringNext = (m_ringNext + 1) % kRingFrames;
    m_ringSize = std::min(m_ringSize + 1, kRingFrames);
}

std::vector<FrameTiming> GpuPassTimer::recent(std::size_t n) const {
    n = std::min(n, m_ringSize);
    std::vector<FrameTiming> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(m_ring[(m_ringNext + kRingFrames - n + i) % kRingFrames]);
    return out;
}

const FrameTiming* GpuPassTimer::latest() const noexcept {
    return m_ringSize ? &m_ring[(m_ringNext + kRingFrames - 1) % kRingFrames] : nullptr;
}

} // namespace ohao::render
