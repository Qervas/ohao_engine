#pragma once
#include "render/scene/render_scene_types.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <set>
#include <vector>

namespace ohao::render {

// Instance table: one InstanceGpu per slot plus a per-slot material reference list
// (kMaxMaterialRefsPerInstance material indices at [slot * 64, slot * 64 + 64)).
// Released slots go to a free list and are reused lowest-first. Only dirty slots
// are uploaded; the reference list only when it changed.
class InstanceTable {
public:
    static constexpr uint32_t kInitialCapacity = 1024;

    [[nodiscard]] bool initialize(GpuAllocator& allocator);
    void shutdown();

    // `materialCount` bounds the material indices: a missing or unknown handle is 0.
    uint32_t create(const InstanceDesc& desc, uint32_t materialCount);
    void update(uint32_t slot, const InstanceDesc& desc, uint32_t materialCount);
    void destroy(uint32_t slot);

    [[nodiscard]] bool live(uint32_t slot) const noexcept {
        return slot < m_live.size() && m_live[slot] != 0;
    }
    [[nodiscard]] uint32_t highWater() const noexcept { return m_highWater; }
    [[nodiscard]] uint32_t liveCount() const noexcept { return m_liveCount; }
    [[nodiscard]] const InstanceGpu& gpu(uint32_t slot) const { return m_gpu[slot]; }
    [[nodiscard]] uint32_t materialIndexFor(uint32_t slot, uint32_t materialSlot) const;

    // True once after any change that alters the draw list (create, destroy,
    // visibility or mesh change, host storage growth). Clears the flag.
    [[nodiscard]] bool consumeDrawsDirty() noexcept { const bool d = m_drawsDirty; m_drawsDirty = false; return d; }

    [[nodiscard]] VkDeviceSize pendingBytes() const noexcept;
    [[nodiscard]] bool needsGpuGrowth() const noexcept { return m_capacity > m_gpuCapacity; }
    [[nodiscard]] uint32_t gpuGrowths() const noexcept { return m_gpuGrowths; }
    // Grows the device buffers (old -> new copy recorded, old pushed to `retired`),
    // then uploads dirty slots. Returns the InstanceGpu bytes uploaded.
    std::size_t recordUploads(detail::StagingBatch& batch, std::vector<GpuBuffer>& retired);

    [[nodiscard]] VkBuffer instanceBuffer() const noexcept { return m_instanceBuffer.buffer; }
    [[nodiscard]] VkBuffer materialRefBuffer() const noexcept { return m_refBuffer.buffer; }

private:
    void write(uint32_t slot, const InstanceDesc& desc, uint32_t materialCount);
    void growHost(uint32_t minCapacity);
    [[nodiscard]] bool createGpuBuffers(uint32_t capacity, GpuBuffer& instances, GpuBuffer& refs);

    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_instanceBuffer;
    GpuBuffer m_refBuffer;
    uint32_t m_gpuCapacity{0};
    uint32_t m_gpuGrowths{0};

    uint32_t m_capacity{0};
    uint32_t m_highWater{0};
    uint32_t m_liveCount{0};
    std::vector<InstanceGpu> m_gpu;      // m_capacity entries
    std::vector<uint32_t> m_refs;        // m_capacity * kMaxMaterialRefsPerInstance
    std::vector<uint8_t> m_live;
    std::set<uint32_t> m_free;
    std::set<uint32_t> m_dirty;
    std::set<uint32_t> m_dirtyRefs;
    bool m_drawsDirty{false};
};

} // namespace ohao::render
