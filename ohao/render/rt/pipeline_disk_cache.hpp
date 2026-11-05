#pragma once

// A VkPipelineCache kept in a file, so a process does not compile what an earlier one did.
//
// The path tracer's ray tracing pipeline takes minutes to compile cold. NVIDIA's driver does not use this data for
// it (a fresh executable with the 38 MB file loaded compiled 692 s, 2026-11-05): its own per-executable cache is what
// spares a compile there, and not for a second process while another holds the pipeline (AGENTS.md). The file stays
// for drivers that honour a VkPipelineCache. It is per device and driver version, at
// `$OHAO_PIPELINE_CACHE_DIR` if set, else `%LOCALAPPDATA%/ohao/pipeline_cache` (Windows) or
// `$XDG_CACHE_HOME/ohao/pipeline_cache` (`~/.cache` without it). Data whose header is not this
// device's is ignored; a write goes to a temporary file renamed over the old one, so two
// processes saving at once leave one whole file.
//
// One file per shader version (its key a hash of the pipeline's SPIR-V): a single file per pipeline
// kept every version's entries, passed its 64 MB cap, was then ignored, and every process compiled
// cold. A save prunes that pipeline's other versions but the newest one.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ohao {

// A key for a pipeline's shaders: FNV-1a over each module's size and bytes.
[[nodiscard]] uint64_t pipelineContentKey(std::span<const std::vector<char>> modules);
// `<name>@<key, 16 hex>-<vendor>-<device>-<driver>.bin`: '@' so no pipeline's name prefixes another's files.
[[nodiscard]] std::string pipelineCacheFileName(const std::string& name, uint64_t key, uint32_t vendor,
                                                uint32_t device, uint32_t driver);
// Removes `name`'s files in `dir` for this device and driver (`deviceSuffix`, "-<vendor>-<device>-<driver>.bin")
// other than `keep` and the `older` newest others, and the file named before versions were.
void prunePipelineCaches(const std::filesystem::path& dir, const std::string& name, const std::string& deviceSuffix,
                         const std::filesystem::path& keep, int older);

class PipelineDiskCache {
public:
    PipelineDiskCache() = default;
    PipelineDiskCache(const PipelineDiskCache&) = delete;
    PipelineDiskCache& operator=(const PipelineDiskCache&) = delete;
    ~PipelineDiskCache() { destroy(); }

    /// Creates the cache from `<dir>/<pipelineCacheFileName>` for these shaders (`key`: pipelineContentKey)
    /// (empty when missing, unreadable, too large or another device's). Without a usable directory the
    /// cache still works in memory and save() does nothing.
    void open(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& name, uint64_t key);

    /// Writes the cache's data to its file (no-op without one). Call after creating pipelines.
    void save();

    void destroy();

    [[nodiscard]] VkPipelineCache handle() const noexcept { return m_cache; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    std::filesystem::path m_file;
    std::string m_name, m_deviceSuffix;
};

}  // namespace ohao
