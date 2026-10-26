#include "render/scene/instance_table.hpp"

#include <algorithm>
#include <cstring>

#include "core/log.hpp"

namespace ohao::render {

namespace {
constexpr VkDeviceSize kRefBytesPerSlot = VkDeviceSize{kMaxMaterialRefsPerInstance} * sizeof(uint32_t);
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

// Calls fn(first, count) for each run of consecutive values in `s`.
template <typename Fn>
void forEachRun(const std::set<uint32_t>& s, Fn&& fn) {
    for (auto it = s.begin(); it != s.end();) {
        const uint32_t first = *it;
        uint32_t last = first;
        for (++it; it != s.end() && *it == last + 1; ++it) last = *it;
        fn(first, last - first + 1);
    }
}
} // namespace

bool InstanceTable::initialize(GpuAllocator& allocator) {
    m_allocator = &allocator;
    growHost(kInitialCapacity);
    if (!createGpuBuffers(m_capacity, m_instanceBuffer, m_refBuffer)) return false;
    m_gpuCapacity = m_capacity;
    return true;
}

void InstanceTable::shutdown() {
    if (m_allocator) {
        m_allocator->destroyBuffer(m_instanceBuffer);
        m_allocator->destroyBuffer(m_refBuffer);
    }
    *this = InstanceTable{};
}

bool InstanceTable::createGpuBuffers(uint32_t capacity, GpuBuffer& instances, GpuBuffer& refs) {
    instances = m_allocator->createBuffer(VkDeviceSize{capacity} * sizeof(InstanceGpu), kUsage,
                                          AllocationUsage::GpuOnly);
    refs = m_allocator->createBuffer(VkDeviceSize{capacity} * kRefBytesPerSlot, kUsage,
                                     AllocationUsage::GpuOnly);
    if (!instances.isValid() || !refs.isValid()) {
        m_allocator->destroyBuffer(instances);
        m_allocator->destroyBuffer(refs);
        OHAO_LOG(Error, render, "RenderScene: instance table allocation failed ({} slots)", capacity);
        return false;
    }
    return true;
}

void InstanceTable::growHost(uint32_t minCapacity) {
    uint32_t cap = std::max(m_capacity, kInitialCapacity);
    while (cap < minCapacity) cap *= 2;
    if (cap == m_capacity) return;
    m_capacity = cap;
    m_gpu.resize(cap, InstanceGpu{});
    m_refs.resize(static_cast<std::size_t>(cap) * kMaxMaterialRefsPerInstance, 0u);
    m_live.resize(cap, 0);
    m_drawsDirty = true;   // Draw::world points into m_gpu
}

void InstanceTable::write(uint32_t slot, const InstanceDesc& desc, uint32_t materialCount) {
    if (desc.materials.size() > kMaxMaterialRefsPerInstance) {
        static bool logged = false;
        if (!logged) {
            OHAO_LOG(Warn, render, "RenderScene: instance with {} materials: only the first {} are used",
                     desc.materials.size(), kMaxMaterialRefsPerInstance);
            logged = true;
        }
    }
    const auto refCount = static_cast<uint32_t>(
        std::min<std::size_t>(desc.materials.size(), kMaxMaterialRefsPerInstance));

    InstanceGpu g{};
    g.world = desc.world;
    g.prevWorld = desc.prevWorld;
    g.mesh = desc.mesh.index;
    g.materialRefOffset = slot * kMaxMaterialRefsPerInstance;
    g.materialRefCount = refCount;
    g.flags = (desc.visible ? kInstanceVisible : 0u) | (desc.emitter ? kInstanceEmitter : 0u);
    g.giAlbedoPacked = desc.giAlbedoPacked;

    InstanceGpu& cur = m_gpu[slot];
    if (cur.flags != g.flags || cur.mesh != g.mesh || cur.materialRefCount != g.materialRefCount)
        m_drawsDirty = true;
    if (std::memcmp(&cur, &g, sizeof(g)) != 0) {
        cur = g;
        m_dirty.insert(slot);
    }

    uint32_t refs[kMaxMaterialRefsPerInstance] = {};
    for (uint32_t i = 0; i < refCount; ++i) {
        const MaterialHandle h = desc.materials[i];
        refs[i] = (h.valid() && h.index < materialCount) ? h.index : 0u;
    }
    uint32_t* dst = &m_refs[static_cast<std::size_t>(slot) * kMaxMaterialRefsPerInstance];
    if (std::memcmp(dst, refs, sizeof(refs)) != 0) {
        std::memcpy(dst, refs, sizeof(refs));
        m_dirtyRefs.insert(slot);
    }
}

uint32_t InstanceTable::create(const InstanceDesc& desc, uint32_t materialCount) {
    uint32_t slot;
    if (!m_free.empty()) {
        slot = *m_free.begin();
        m_free.erase(m_free.begin());
    } else {
        slot = m_highWater++;
        if (slot >= m_capacity) growHost(slot + 1);
    }
    m_live[slot] = 1;
    ++m_liveCount;
    m_drawsDirty = true;
    // Force the first upload of the slot even if it matches stale contents.
    m_gpu[slot] = InstanceGpu{};
    m_gpu[slot].mesh = kInvalidIndex;
    m_dirty.insert(slot);
    m_dirtyRefs.insert(slot);
    write(slot, desc, materialCount);
    return slot;
}

void InstanceTable::update(uint32_t slot, const InstanceDesc& desc, uint32_t materialCount) {
    if (!live(slot)) {
        OHAO_LOG(Warn, render, "RenderScene: updateInstance on dead slot {} ignored", slot);
        return;
    }
    write(slot, desc, materialCount);
}

void InstanceTable::destroy(uint32_t slot) {
    if (!live(slot)) return;
    m_live[slot] = 0;
    --m_liveCount;
    m_gpu[slot].flags = 0;   // the GPU copy reads as invisible until the slot is reused
    m_dirty.insert(slot);
    m_free.insert(slot);
    m_drawsDirty = true;
}

uint32_t InstanceTable::materialIndexFor(uint32_t slot, uint32_t materialSlot) const {
    if (!live(slot)) return 0;
    if (materialSlot >= m_gpu[slot].materialRefCount) return 0;
    return m_refs[static_cast<std::size_t>(slot) * kMaxMaterialRefsPerInstance + materialSlot];
}

VkDeviceSize InstanceTable::pendingBytes() const noexcept {
    return m_dirty.size() * sizeof(InstanceGpu) + m_dirtyRefs.size() * kRefBytesPerSlot;
}

std::size_t InstanceTable::recordUploads(detail::StagingBatch& batch, std::vector<GpuBuffer>& retired) {
    if (needsGpuGrowth()) {
        GpuBuffer instances, refs;
        if (!createGpuBuffers(m_capacity, instances, refs)) return 0;   // keep dirty, retry next frame
        const VkBufferCopy iCopy{0, 0, VkDeviceSize{m_gpuCapacity} * sizeof(InstanceGpu)};
        const VkBufferCopy rCopy{0, 0, VkDeviceSize{m_gpuCapacity} * kRefBytesPerSlot};
        vkCmdCopyBuffer(batch.cmd, m_instanceBuffer.buffer, instances.buffer, 1, &iCopy);
        vkCmdCopyBuffer(batch.cmd, m_refBuffer.buffer, refs.buffer, 1, &rCopy);
        // The dirty writes below may land inside the copied range: order them after it.
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
        retired.push_back(m_instanceBuffer);
        retired.push_back(m_refBuffer);
        m_instanceBuffer = instances;
        m_refBuffer = refs;
        m_gpuCapacity = m_capacity;
        ++m_gpuGrowths;
    }

    std::size_t instanceBytes = 0;
    forEachRun(m_dirty, [&](uint32_t first, uint32_t n) {
        const VkDeviceSize bytes = VkDeviceSize{n} * sizeof(InstanceGpu);
        batch.upload(m_instanceBuffer.buffer, VkDeviceSize{first} * sizeof(InstanceGpu), &m_gpu[first], bytes);
        instanceBytes += static_cast<std::size_t>(bytes);
    });
    forEachRun(m_dirtyRefs, [&](uint32_t first, uint32_t n) {
        batch.upload(m_refBuffer.buffer, VkDeviceSize{first} * kRefBytesPerSlot,
                     &m_refs[static_cast<std::size_t>(first) * kMaxMaterialRefsPerInstance],
                     VkDeviceSize{n} * kRefBytesPerSlot);
    });
    m_dirty.clear();
    m_dirtyRefs.clear();
    return instanceBytes;
}

} // namespace ohao::render
