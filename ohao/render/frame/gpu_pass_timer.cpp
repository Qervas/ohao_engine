#include "render/frame/gpu_pass_timer.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace ohao::render {

bool GpuPassTimer::init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamily, uint32_t slots) {
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
    OHAO_LOG(Debug, render, "GPU pass timing: {} slots, {} passes a frame, {} ns a tick", slots, kMaxTimedPasses,
             props.limits.timestampPeriod);
    return true;
}

void GpuPassTimer::destroy() {
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
    m_open = true;
}

void GpuPassTimer::end(VkCommandBuffer cmd) {
    if (m_current < 0 || !m_open) return;
    Slot& s = m_slots[static_cast<std::size_t>(m_current)];
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, s.count * 2 + 1);
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
