#include "render/scene/mesh_registry.hpp"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <iostream>

namespace ohao::render {

namespace {
constexpr VkDeviceSize kInitialVertices      = 1u << 20;   // 1 Mi
constexpr VkDeviceSize kInitialIndices       = 4u << 20;   // 4 Mi
constexpr VkDeviceSize kInitialTriangles     = 1u << 20;   // 1 Mi
constexpr VkDeviceSize kInitialMeshes        = 4096;
constexpr VkDeviceSize kVertexStride         = sizeof(Vertex);   // 100

constexpr VkBufferUsageFlags kGeometryUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr VkBufferUsageFlags kTableUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
} // namespace

bool MeshRegistry::initialize(const Context& ctx) {
    m_ctx = ctx;
    return createArena(m_vertices, kVertexStride, kInitialVertices,
                       kGeometryUsage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) &&
           createArena(m_indices, sizeof(uint32_t), kInitialIndices,
                       kGeometryUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT) &&
           createArena(m_triangleSlots, sizeof(uint32_t), kInitialTriangles, kTableUsage) &&
           createArena(m_meshTable, sizeof(MeshGpu), kInitialMeshes, kTableUsage);
}

void MeshRegistry::shutdown() {
    if (m_ctx.rt) {
        for (auto& mesh : m_meshes) {
            if (mesh.blas != INVALID_BLAS) m_ctx.rt->destroyBLAS(mesh.blas);
            mesh.blas = INVALID_BLAS;
        }
    }
    m_meshes.clear();
    if (m_ctx.allocator) {
        for (Arena* a : {&m_vertices, &m_indices, &m_triangleSlots, &m_meshTable}) {
            m_ctx.allocator->destroyBuffer(a->buf);
            *a = Arena{};
        }
    }
    m_ctx = Context{};
}

bool MeshRegistry::createArena(Arena& a, VkDeviceSize elemSize, VkDeviceSize capacity,
                               VkBufferUsageFlags usage) {
    a.usage = usage;
    a.elemSize = elemSize;
    a.capacity = capacity;
    a.used = 0;
    a.buf = m_ctx.allocator->createBuffer(elemSize * capacity, usage, AllocationUsage::GpuOnly);
    if (!a.buf.isValid()) {
        std::cerr << "[RenderScene] arena allocation failed (" << elemSize * capacity << " bytes)\n";
        return false;
    }
    return true;
}

bool MeshRegistry::reserve(Arena& a, VkDeviceSize count, VkCommandBuffer cmd,
                           std::vector<GpuBuffer>& retired, uint32_t& grown) {
    const VkDeviceSize required = a.used + count;
    if (required <= a.capacity) return true;

    const VkDeviceSize newCapacity = required * 2;
    GpuBuffer bigger = m_ctx.allocator->createBuffer(a.elemSize * newCapacity, a.usage,
                                                     AllocationUsage::GpuOnly);
    if (!bigger.isValid()) {
        std::cerr << "[RenderScene] arena growth failed (" << a.elemSize * newCapacity << " bytes)\n";
        return false;
    }
    if (a.used > 0) {
        const VkBufferCopy region{0, 0, a.used * a.elemSize};
        vkCmdCopyBuffer(cmd, a.buf.buffer, bigger.buffer, 1, &region);
    }
    retired.push_back(a.buf);
    a.buf = bigger;
    a.capacity = newCapacity;
    ++grown;
    return true;
}

VkCommandBuffer MeshRegistry::beginOneShot() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_ctx.commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_ctx.device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void MeshRegistry::endOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(m_ctx.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx.queue);
    vkFreeCommandBuffers(m_ctx.device, m_ctx.commandPool, 1, &cmd);
}

void MeshRegistry::buildBlas(MeshInfo& mesh) {
    mesh.blas = INVALID_BLAS;
    if (!m_ctx.rt || !m_ctx.rt->isSupported() || mesh.indexCount < 3) return;
    mesh.blas = m_ctx.rt->createBLAS(
        m_vertices.buf.buffer, mesh.vertexCount, kVertexStride,
        m_indices.buf.buffer, mesh.indexCount,
        static_cast<VkDeviceSize>(mesh.vertexOffset) * kVertexStride,
        static_cast<VkDeviceSize>(mesh.indexOffset) * sizeof(uint32_t),
        VK_NULL_HANDLE,          // own one-shot command buffer (load time)
        mesh.vertexOffset);      // arena indices are mesh-local
}

MeshHandle MeshRegistry::registerMesh(const MeshData& data, std::vector<GpuBuffer>& retired,
                                      uint32_t& grown) {
    if (!m_ctx.allocator || !m_vertices.buf.isValid()) return {};
    if (data.vertices.empty() || data.indices.empty()) {
        std::cerr << "[RenderScene] registerMesh '" << data.debugName << "': no geometry\n";
        return {};
    }
    const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
    const auto indexCount = static_cast<uint32_t>(data.indices.size());
    const uint32_t triangleCount = indexCount / 3;

    for (uint32_t idx : data.indices) {
        if (idx >= vertexCount) {
            std::cerr << "[RenderScene] registerMesh '" << data.debugName
                      << "': index " << idx << " out of range (" << vertexCount << " vertices)\n";
            return {};
        }
    }

    MeshInfo mesh{};
    mesh.vertexCount = vertexCount;
    mesh.indexCount = indexCount;
    mesh.debugName = data.debugName;
    mesh.submeshes = data.submeshes;
    if (mesh.submeshes.empty()) mesh.submeshes.push_back(SubmeshDesc{0, indexCount, 0});
    for (const SubmeshDesc& s : mesh.submeshes) {
        if (static_cast<uint64_t>(s.firstIndex) + s.indexCount > indexCount) {
            std::cerr << "[RenderScene] registerMesh '" << data.debugName
                      << "': submesh range past the index count\n";
            return {};
        }
    }

    // Per-triangle material slot; missing entries are slot 0.
    std::vector<uint32_t> triangleSlots(triangleCount, 0u);
    if (!data.triangleMaterialSlot.empty()) {
        if (data.triangleMaterialSlot.size() != triangleCount) {
            std::cerr << "[RenderScene] registerMesh '" << data.debugName << "': "
                      << data.triangleMaterialSlot.size() << " triangle slots for "
                      << triangleCount << " triangles (missing = slot 0)\n";
        }
        const std::size_t n = std::min<std::size_t>(triangleCount, data.triangleMaterialSlot.size());
        std::copy_n(data.triangleMaterialSlot.begin(), n, triangleSlots.begin());
    }

    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const Vertex& v : data.vertices) {
        mn = glm::min(mn, v.position);
        mx = glm::max(mx, v.position);
    }
    mesh.aabbMin = mn;
    mesh.aabbMax = mx;

    // One staging buffer for the whole registration: vertices | indices | slots | entry.
    const VkDeviceSize vBytes = VkDeviceSize{vertexCount} * kVertexStride;
    const VkDeviceSize iBytes = VkDeviceSize{indexCount} * sizeof(uint32_t);
    const VkDeviceSize tBytes = VkDeviceSize{triangleCount} * sizeof(uint32_t);
    const VkDeviceSize eBytes = sizeof(MeshGpu);
    GpuBuffer staging = m_ctx.allocator->createBuffer(vBytes + iBytes + tBytes + eBytes,
                                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                      AllocationUsage::CpuToGpu, true);
    if (!staging.isValid() || !staging.getMappedData()) {
        m_ctx.allocator->destroyBuffer(staging);
        std::cerr << "[RenderScene] registerMesh '" << data.debugName << "': staging allocation failed\n";
        return {};
    }

    VkCommandBuffer cmd = beginOneShot();
    if (cmd == VK_NULL_HANDLE) {
        m_ctx.allocator->destroyBuffer(staging);
        return {};
    }

    bool ok = reserve(m_vertices, vertexCount, cmd, retired, grown);
    ok = ok && reserve(m_indices, indexCount, cmd, retired, grown);
    ok = ok && reserve(m_triangleSlots, triangleCount, cmd, retired, grown);
    ok = ok && reserve(m_meshTable, 1, cmd, retired, grown);

    if (ok) {
        mesh.vertexOffset = static_cast<uint32_t>(m_vertices.used);
        mesh.indexOffset = static_cast<uint32_t>(m_indices.used);
        mesh.triangleSlotOffset = static_cast<uint32_t>(m_triangleSlots.used);
        const MeshGpu entry{mesh.vertexOffset, mesh.indexOffset, mesh.indexCount, mesh.triangleSlotOffset};

        auto* dst = static_cast<std::byte*>(staging.getMappedData());
        std::memcpy(dst, data.vertices.data(), static_cast<std::size_t>(vBytes));
        std::memcpy(dst + vBytes, data.indices.data(), static_cast<std::size_t>(iBytes));
        if (tBytes) std::memcpy(dst + vBytes + iBytes, triangleSlots.data(), static_cast<std::size_t>(tBytes));
        std::memcpy(dst + vBytes + iBytes + tBytes, &entry, sizeof(entry));
        m_ctx.allocator->flushBuffer(staging);

        // Growth copies (old -> new) and these writes touch disjoint ranges.
        const VkBufferCopy vCopy{0, m_vertices.used * kVertexStride, vBytes};
        vkCmdCopyBuffer(cmd, staging.buffer, m_vertices.buf.buffer, 1, &vCopy);
        const VkBufferCopy iCopy{vBytes, m_indices.used * sizeof(uint32_t), iBytes};
        vkCmdCopyBuffer(cmd, staging.buffer, m_indices.buf.buffer, 1, &iCopy);
        if (tBytes) {
            const VkBufferCopy tCopy{vBytes + iBytes, m_triangleSlots.used * sizeof(uint32_t), tBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_triangleSlots.buf.buffer, 1, &tCopy);
        }
        const VkBufferCopy eCopy{vBytes + iBytes + tBytes, m_meshTable.used * sizeof(MeshGpu), eBytes};
        vkCmdCopyBuffer(cmd, staging.buffer, m_meshTable.buf.buffer, 1, &eCopy);
    }

    // Make the arenas visible to every later submission (draws, shaders, BLAS builds).
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    endOneShot(cmd);
    m_ctx.allocator->destroyBuffer(staging);
    if (!ok) return {};

    m_vertices.used += vertexCount;
    m_indices.used += indexCount;
    m_triangleSlots.used += triangleCount;
    m_meshTable.used += 1;

    // A built BLAS (FAST_TRACE, no ALLOW_UPDATE) does not reference its inputs, so
    // existing BLASes survive arena growth untouched.
    buildBlas(mesh);

    const auto index = static_cast<uint32_t>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    return MeshHandle{index, 0};
}

const MeshInfo* MeshRegistry::info(MeshHandle h) const {
    if (!h.valid() || h.index >= m_meshes.size() || h.generation != 0) return nullptr;
    return &m_meshes[h.index];
}

} // namespace ohao::render
