#include "pipeline_disk_cache.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define OHAO_PID() _getpid()
#else
#include <unistd.h>
#define OHAO_PID() getpid()
#endif

#include "core/log.hpp"

namespace ohao {

namespace {

// Past this the file is dropped and the cache starts empty: entries for shaders since changed
// are never evicted, and the path tracer's is about 11 MB.
constexpr std::uintmax_t kMaxCacheBytes = 64ull << 20;

std::filesystem::path cacheDirectory() {
    if (const char* dir = std::getenv("OHAO_PIPELINE_CACHE_DIR"); dir && *dir) return dir;
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::filesystem::path(local) / "ohao" / "pipeline_cache";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "ohao" / "pipeline_cache";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "ohao" / "pipeline_cache";
#endif
    return {};
}

// Vulkan's cache header (VkPipelineCacheHeaderVersionOne) names the device the data is for.
bool headerMatches(const std::vector<char>& data, const VkPhysicalDeviceProperties& props) {
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header)) return false;
    std::memcpy(&header, data.data(), sizeof(header));
    return header.headerSize >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendorID == props.vendorID && header.deviceID == props.deviceID &&
           std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

}  // namespace

void PipelineDiskCache::open(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& name) {
    destroy();
    m_device = device;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    std::vector<char> data;
    if (const std::filesystem::path dir = cacheDirectory(); !dir.empty()) {
        m_file = dir / (name + "-" + std::to_string(props.vendorID) + "-" + std::to_string(props.deviceID) + "-" +
                        std::to_string(props.driverVersion) + ".bin");
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(m_file, ec);
        if (!ec && size > 0 && size <= kMaxCacheBytes) {
            std::ifstream in(m_file, std::ios::binary);
            data.resize(static_cast<size_t>(size));
            if (!in.read(data.data(), static_cast<std::streamsize>(data.size()))) data.clear();
        }
        if (!data.empty() && !headerMatches(data, props)) {
            OHAO_LOG(Info, gpu, "pipeline cache: {} is another device's or driver's; starting empty", m_file.string());
            data.clear();
        }
    }

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device, &info, nullptr, &m_cache) != VK_SUCCESS) {
        // Rejected data: an empty cache still serves this process.
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        if (vkCreatePipelineCache(device, &info, nullptr, &m_cache) != VK_SUCCESS) m_cache = VK_NULL_HANDLE;
        data.clear();
    }
    OHAO_LOG(Debug, gpu, "pipeline cache: {} ({} bytes loaded)", m_file.string(), data.size());
}

void PipelineDiskCache::save() {
    if (m_cache == VK_NULL_HANDLE || m_file.empty()) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(m_device, m_cache, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(m_device, m_cache, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);

    std::error_code ec;
    std::filesystem::create_directories(m_file.parent_path(), ec);
    std::filesystem::path tmp = m_file;
    tmp += "." + std::to_string(OHAO_PID()) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(data.data(), static_cast<std::streamsize>(data.size()))) {
            out.close();
            std::filesystem::remove(tmp, ec);
            OHAO_LOG(Warn, gpu, "pipeline cache: could not write {}", tmp.string());
            return;
        }
    }
    std::filesystem::rename(tmp, m_file, ec);
    if (ec) {
        // Another process holds the file open: its copy is as good as ours.
        std::filesystem::remove(tmp, ec);
        return;
    }
    OHAO_LOG(Debug, gpu, "pipeline cache: saved {} bytes to {}", data.size(), m_file.string());
}

void PipelineDiskCache::destroy() {
    if (m_cache != VK_NULL_HANDLE) vkDestroyPipelineCache(m_device, m_cache, nullptr);
    m_cache = VK_NULL_HANDLE;
    m_file.clear();
}

}  // namespace ohao
