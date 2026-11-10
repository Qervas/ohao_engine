#include "gpu/vulkan/gpu_fault.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/vk_utils.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ohao::gpu {
namespace {

struct Injection {
    enum class Kind { None, Lost, Hang } kind{Kind::None};
    uint64_t frame{0};
};

Injection parseInjection() {
    const char* v = std::getenv("OHAO_INJECT_GPU_FAULT");
    if (!v || !*v) return {};
    const std::string_view s(v);
    const auto colon = s.find(':');
    Injection in;
    const std::string_view kind = s.substr(0, colon);
    if (kind == "lost") in.kind = Injection::Kind::Lost;
    else if (kind == "hang") in.kind = Injection::Kind::Hang;
    if (in.kind == Injection::Kind::None || colon == std::string_view::npos ||
        std::from_chars(s.data() + colon + 1, s.data() + s.size(), in.frame).ec != std::errc{} || in.frame == 0) {
        OHAO_LOG(Error, gpu, "OHAO_INJECT_GPU_FAULT: '{}' is not lost:N or hang:N (N a frame, from 1); ignored", s);
        return {};
    }
    OHAO_LOG(Warn, gpu, "OHAO_INJECT_GPU_FAULT: frame {} will {}", in.frame,
             in.kind == Injection::Kind::Lost ? "report a lost device" : "never signal its fence");
    return in;
}

const Injection& injection() {
    static const Injection in = parseInjection();
    return in;
}

std::mutex g_mutex;
VkDevice g_device{VK_NULL_HANDLE};
bool g_deviceFault{false};
std::function<std::string()> g_describe;
std::atomic<uint64_t> g_frame{0};
std::atomic<bool> g_reporting{false};

nlohmann::json deviceFaultInfo() {
    if (!g_deviceFault || g_device == VK_NULL_HANDLE) return nullptr;
    const auto get = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(g_device, "vkGetDeviceFaultInfoEXT"));
    if (!get) return nullptr;
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (const VkResult c = get(g_device, &counts, nullptr); c != VK_SUCCESS)
        return std::string("vkGetDeviceFaultInfoEXT: ") + std::string(vk_result_name(c));
    counts.vendorBinarySize = 0;   // the vendor's crash dump is for their tools, not this report
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
    const VkResult r = get(g_device, &counts, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return std::string("vkGetDeviceFaultInfoEXT: ") + std::string(vk_result_name(r));
    nlohmann::json out;
    out["description"] = info.description;
    static constexpr const char* kAddressKind[] = {"none", "read_invalid", "write_invalid", "execute_invalid",
                                                   "instruction_pointer_unknown", "instruction_pointer_invalid",
                                                   "instruction_pointer_fault"};
    for (uint32_t i = 0; i < counts.addressInfoCount && i < 8; ++i) {
        const auto& a = addresses[i];
        const auto type = static_cast<uint32_t>(a.addressType);
        char addr[24];
        std::snprintf(addr, sizeof addr, "0x%llx", static_cast<unsigned long long>(a.reportedAddress));
        out["addresses"].push_back({{"kind", type < std::size(kAddressKind) ? kAddressKind[type] : "other"},
                                    {"address", addr},
                                    {"precision", a.addressPrecision}});
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount && i < 8; ++i)
        out["vendor"].push_back({{"description", vendor[i].description},
                                 {"code", vendor[i].vendorFaultCode},
                                 {"data", vendor[i].vendorFaultData}});
    return out;
}

} // namespace

void setFaultDevice(VkDevice device, bool deviceFault) {
    std::lock_guard lock(g_mutex);
    g_device = device;
    g_deviceFault = deviceFault;
}

void setFaultContext(std::function<std::string()> describe) {
    std::lock_guard lock(g_mutex);
    g_describe = std::move(describe);
}

void setFaultFrame(uint64_t frame) noexcept { g_frame.store(frame, std::memory_order_relaxed); }

void reportFault(FaultKind kind, std::string_view where) {
    // A second thread meeting the fault (the present waiter) waits for the first to exit.
    if (g_reporting.exchange(true)) {
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    nlohmann::json r;
    r["fault"] = kind == FaultKind::Hang ? "hang" : "device_lost";
    r["where"] = where;
    r["frame"] = g_frame.load(std::memory_order_relaxed);
    if (kind == FaultKind::Hang) r["timeoutMs"] = kGpuTimeout.count();
    if (injection().kind != Injection::Kind::None) r["injected"] = true;   // OHAO_INJECT_GPU_FAULT: not a real fault
    {
        std::lock_guard lock(g_mutex);
        if (g_describe) {
            const std::string ran = g_describe();
            r["ran"] = nlohmann::json::parse(ran, nullptr, false);
        }
        if (nlohmann::json f = deviceFaultInfo(); !f.is_null()) r["deviceFault"] = std::move(f);
    }
    const std::string text = r.dump();
    OHAO_LOG(Error, gpu, "GPU fault: {}", text);
    log::flush();
    // The log cuts a record at 1 KB; stderr gets it whole (stdout is the protocol's in ohao serve).
    if (text.size() > log::kMaxMessage - 16) std::fprintf(stderr, "GPU fault (whole): %s\n", text.c_str());
    std::fflush(stderr);
    std::quick_exit(kExitGpuLost);
}

VkResult submit(VkQueue queue, const VkSubmitInfo& info, VkFence fence, std::string_view where, uint64_t frame) {
    if (frame > 0 && frame == injection().frame) {
        if (injection().kind == Injection::Kind::Lost) reportFault(FaultKind::DeviceLost, where);
        return VK_SUCCESS;   // hang: never submitted, so its fence never signals
    }
    const VkResult r = vkQueueSubmit(queue, 1, &info, fence);
    if (r == VK_ERROR_DEVICE_LOST) reportFault(FaultKind::DeviceLost, where);
    if (r != VK_SUCCESS) OHAO_LOG(Error, gpu, "{}: vkQueueSubmit: {}", where, vk_result_name(r));
    return r;
}

void waitFence(VkDevice device, VkFence fence, std::string_view where) {
    const auto ns = static_cast<uint64_t>(std::chrono::nanoseconds(kGpuTimeout).count());
    const VkResult r = vkWaitForFences(device, 1, &fence, VK_TRUE, ns);
    if (r == VK_SUCCESS) return;
    if (r == VK_TIMEOUT) reportFault(FaultKind::Hang, where);
    reportFault(FaultKind::DeviceLost, where);   // VK_ERROR_DEVICE_LOST, or out of memory in a wait
}

void submitAndWait(VkDevice device, VkQueue queue, const VkSubmitInfo& info, std::string_view where) {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "{}: vkCreateFence failed; waiting for the queue", where);
        if (submit(queue, info, VK_NULL_HANDLE, where) == VK_SUCCESS) checkResult(vkQueueWaitIdle(queue), where);
        return;
    }
    if (submit(queue, info, fence, where) == VK_SUCCESS) waitFence(device, fence, where);
    vkDestroyFence(device, fence, nullptr);
}

VkResult checkResult(VkResult r, std::string_view where) {
    if (r == VK_ERROR_DEVICE_LOST) reportFault(FaultKind::DeviceLost, where);
    return r;
}

} // namespace ohao::gpu
