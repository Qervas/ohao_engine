#pragma once

// A VkPipelineCache kept in a file, so a process does not compile what an earlier one did.
//
// The path tracer's ray tracing pipeline takes minutes to compile cold. The driver keeps its
// own disk cache, but a second process creating the same pipeline while another one is alive
// was measured compiling it from scratch (239 s against 6 s alone; check_live's headless
// host beside its window). The file is per device and driver version, at
// `$OHAO_PIPELINE_CACHE_DIR` if set, else `%LOCALAPPDATA%/ohao/pipeline_cache` (Windows) or
// `$XDG_CACHE_HOME/ohao/pipeline_cache` (`~/.cache` without it). Data whose header is not this
// device's is ignored; a write goes to a temporary file renamed over the old one, so two
// processes saving at once leave one whole file.

#include <vulkan/vulkan.h>

#include <filesystem>
#include <string>

namespace ohao {

class PipelineDiskCache {
public:
    PipelineDiskCache() = default;
    PipelineDiskCache(const PipelineDiskCache&) = delete;
    PipelineDiskCache& operator=(const PipelineDiskCache&) = delete;
    ~PipelineDiskCache() { destroy(); }

    /// Creates the cache from `<dir>/<name>-<vendor>-<device>-<driver>.bin` (empty when missing,
    /// unreadable, too large or another device's). Without a usable directory the cache still
    /// works in memory and save() does nothing.
    void open(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& name);

    /// Writes the cache's data to its file (no-op without one). Call after creating pipelines.
    void save();

    void destroy();

    [[nodiscard]] VkPipelineCache handle() const noexcept { return m_cache; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    std::filesystem::path m_file;
};

}  // namespace ohao
