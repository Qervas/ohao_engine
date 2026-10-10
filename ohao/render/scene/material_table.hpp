#pragma once
#include "render/scene/render_scene_types.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <array>
#include <set>
#include <vector>

namespace ohao::render {

// Material table: a fixed-capacity device-local array of 48-byte packs (the
// MaterialGpuPack layout, emissive strength in row 2 .y, the legacy RT tint texture
// in row 2 .z). Index 0 is the default
// material. Writes are staged through the frame slot's staging buffer.
class MaterialTable {
public:
    static constexpr uint32_t kCapacity = 4096;
    using Pack = std::array<float, 12>;
    static_assert(sizeof(Pack) == layout::MaterialGpuPack::kBytes);

    [[nodiscard]] bool initialize(GpuAllocator& allocator);   // registers MaterialDesc{} as index 0
    void shutdown();

    // Full table: logs and returns the default material.
    MaterialHandle add(const MaterialDesc& desc);
    [[nodiscard]] uint32_t count() const noexcept { return static_cast<uint32_t>(m_packs.size()); }
    [[nodiscard]] static Pack pack(const MaterialDesc& desc);

    [[nodiscard]] VkDeviceSize pendingBytes() const noexcept { return m_dirty.size() * sizeof(Pack); }
    void recordUploads(detail::StagingBatch& batch);   // one copy per contiguous dirty run

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer.buffer; }

private:
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_buffer;
    std::vector<Pack> m_packs;
    std::set<uint32_t> m_dirty;
};

} // namespace ohao::render
