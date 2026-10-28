#include "render/scene/mesh_registry.hpp"
#include "render/scene/meshlet_builder.hpp"
#include "gpu/shared_layout.hpp"

#include <algorithm>
#include <cfloat>
#include <cstring>

#include "core/log.hpp"

namespace ohao::render {

namespace {
constexpr VkDeviceSize kInitialVertices      = 1u << 20;   // 1 Mi
constexpr VkDeviceSize kInitialIndices       = 4u << 20;   // 4 Mi
constexpr VkDeviceSize kInitialTriangles     = 1u << 20;   // 1 Mi
constexpr VkDeviceSize kInitialMeshes        = 4096;
constexpr VkDeviceSize kInitialMeshlets      = 16384;
constexpr VkDeviceSize kVertexStride         = sizeof(Vertex);   // 100
static_assert(sizeof(gpu::shared::MeshletGpu) == 48);

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
           createArena(m_meshTable, sizeof(MeshGpu), kInitialMeshes, kTableUsage) &&
           createArena(m_meshlets, sizeof(gpu::shared::MeshletGpu), kInitialMeshlets, kTableUsage) &&
           createArena(m_meshletVertices, sizeof(uint32_t), kInitialVertices, kTableUsage) &&
           createArena(m_meshletTriangles, sizeof(uint32_t), kInitialTriangles, kTableUsage);
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
        for (Arena* a : {&m_vertices, &m_indices, &m_triangleSlots, &m_meshTable, &m_meshlets, &m_meshletVertices,
                         &m_meshletTriangles}) {
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
        OHAO_LOG(Error, render, "RenderScene: arena allocation failed ({} bytes)", elemSize * capacity);
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
        OHAO_LOG(Error, render, "RenderScene: arena growth failed ({} bytes)", a.elemSize * newCapacity);
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

void MeshRegistry::buildBlas(MeshInfo& mesh, bool updatable) {
    mesh.blas = INVALID_BLAS;
    if (!m_ctx.rt || !m_ctx.rt->isSupported() || mesh.indexCount < 3) return;
    mesh.blas = m_ctx.rt->createBLAS(
        m_vertices.buf.buffer, mesh.vertexCount, kVertexStride,
        m_indices.buf.buffer, mesh.indexCount,
        static_cast<VkDeviceSize>(mesh.vertexOffset) * kVertexStride,
        static_cast<VkDeviceSize>(mesh.indexOffset) * sizeof(uint32_t),
        VK_NULL_HANDLE,          // own one-shot command buffer (load time)
        mesh.vertexOffset,       // arena indices are mesh-local
        updatable);
}

MeshHandle MeshRegistry::registerSkinnedCopy(MeshHandle baseHandle, std::vector<GpuBuffer>& retired, uint32_t& grown) {
    const MeshInfo* base = info(baseHandle);
    if (!base || !m_vertices.buf.isValid()) return {};
    MeshInfo mesh = *base;
    mesh.blas = INVALID_BLAS;
    mesh.skinned = true;
    mesh.baseVertexOffset = base->vertexOffset;
    mesh.debugName = base->debugName + " (skinned)";
    uint32_t meshletCount = 0;
    uint32_t baseFirst = UINT32_MAX;
    for (const auto& sm : base->submeshMeshlets) {
        if (sm[1] == 0) continue;
        baseFirst = std::min(baseFirst, sm[0]);
        meshletCount += sm[1];
    }
    if (baseFirst == UINT32_MAX) baseFirst = 0;

    VkCommandBuffer cmd = beginOneShot();
    if (cmd == VK_NULL_HANDLE) return {};
    bool ok = reserve(m_vertices, mesh.vertexCount, cmd, retired, grown);
    ok = ok && reserve(m_meshTable, 1, cmd, retired, grown);
    ok = ok && reserve(m_meshlets, meshletCount, cmd, retired, grown);
    GpuBuffer staging{};
    if (ok) {
        // Growth copies (old -> new) land before these reads of the grown buffers.
        VkMemoryBarrier grownBarrier{};
        grownBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        grownBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        grownBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &grownBarrier,
                             0, nullptr, 0, nullptr);
        mesh.vertexOffset = static_cast<uint32_t>(m_vertices.used);
        const VkBufferCopy vCopy{VkDeviceSize{base->vertexOffset} * kVertexStride, m_vertices.used * kVertexStride,
                                 VkDeviceSize{mesh.vertexCount} * kVertexStride};
        vkCmdCopyBuffer(cmd, m_vertices.buf.buffer, m_vertices.buf.buffer, 1, &vCopy);
        // Meshlets: the base's, contiguous per mesh (registerMesh appends a mesh's at once).
        const auto firstCopy = static_cast<uint32_t>(m_meshlets.used);
        if (meshletCount > 0) {
            const VkBufferCopy mCopy{VkDeviceSize{baseFirst} * sizeof(gpu::shared::MeshletGpu),
                                     m_meshlets.used * sizeof(gpu::shared::MeshletGpu),
                                     VkDeviceSize{meshletCount} * sizeof(gpu::shared::MeshletGpu)};
            vkCmdCopyBuffer(cmd, m_meshlets.buf.buffer, m_meshlets.buf.buffer, 1, &mCopy);
        }
        for (auto& sm : mesh.submeshMeshlets)
            if (sm[1] > 0) sm[0] = firstCopy + (sm[0] - baseFirst);
        mesh.firstMeshlet = firstCopy;
        mesh.meshletCount = meshletCount;
        const MeshGpu entry{mesh.vertexOffset, mesh.indexOffset, mesh.indexCount, mesh.triangleSlotOffset};
        staging = m_ctx.allocator->createBuffer(sizeof(MeshGpu), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                AllocationUsage::CpuToGpu, true);
        ok = staging.isValid() && staging.getMappedData();
        if (ok) {
            std::memcpy(staging.getMappedData(), &entry, sizeof(entry));
            m_ctx.allocator->flushBuffer(staging);
            const VkBufferCopy eCopy{0, m_meshTable.used * sizeof(MeshGpu), sizeof(MeshGpu)};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshTable.buf.buffer, 1, &eCopy);
        }
    }
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    endOneShot(cmd);
    m_ctx.allocator->destroyBuffer(staging);
    if (!ok) return {};

    m_vertices.used += mesh.vertexCount;
    m_meshTable.used += 1;
    m_meshlets.used += meshletCount;
    buildBlas(mesh, true);
    const auto index = static_cast<uint32_t>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    return MeshHandle{index, 0};
}

MeshHandle MeshRegistry::registerMesh(const MeshData& data, std::vector<GpuBuffer>& retired,
                                      uint32_t& grown) {
    if (!m_ctx.allocator || !m_vertices.buf.isValid()) return {};
    if (data.vertices.empty() || data.indices.empty()) {
        OHAO_LOG(Error, render, "RenderScene: registerMesh '{}': no geometry", data.debugName);
        return {};
    }
    const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
    const auto indexCount = static_cast<uint32_t>(data.indices.size());
    const uint32_t triangleCount = indexCount / 3;

    for (uint32_t idx : data.indices) {
        if (idx >= vertexCount) {
            OHAO_LOG(Error, render, "RenderScene: registerMesh '{}': index {} out of range ({} vertices)",
                     data.debugName, idx, vertexCount);
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
            OHAO_LOG(Error, render, "RenderScene: registerMesh '{}': submesh range past the index count",
                     data.debugName);
            return {};
        }
    }

    // Per-triangle material slot; missing entries are slot 0.
    std::vector<uint32_t> triangleSlots(triangleCount, 0u);
    if (!data.triangleMaterialSlot.empty()) {
        if (data.triangleMaterialSlot.size() != triangleCount) {
            OHAO_LOG(Warn, render, "RenderScene: registerMesh '{}': {} triangle slots for {} triangles (missing = slot 0)",
                     data.debugName, data.triangleMaterialSlot.size(), triangleCount);
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

    // Meshlets, submesh by submesh (offsets local to this mesh until placed in the arenas).
    MeshletSet meshlets;
    std::vector<std::array<uint32_t, 2>> submeshMeshlets;
    for (const SubmeshDesc& s : mesh.submeshes) {
        const auto first = static_cast<uint32_t>(meshlets.meshlets.size());
        buildMeshlets(data.vertices, data.indices, s.firstIndex, s.indexCount, meshlets);
        submeshMeshlets.push_back({first, static_cast<uint32_t>(meshlets.meshlets.size()) - first});
    }
    const auto meshletCount = static_cast<uint32_t>(meshlets.meshlets.size());
    const auto meshletVertexCount = static_cast<uint32_t>(meshlets.vertices.size());
    const auto meshletTriangleCount = static_cast<uint32_t>(meshlets.triangles.size());

    // One staging buffer for the whole registration:
    // vertices | indices | slots | entry | meshlets | meshlet vertices | meshlet triangles.
    const VkDeviceSize vBytes = VkDeviceSize{vertexCount} * kVertexStride;
    const VkDeviceSize iBytes = VkDeviceSize{indexCount} * sizeof(uint32_t);
    const VkDeviceSize tBytes = VkDeviceSize{triangleCount} * sizeof(uint32_t);
    const VkDeviceSize eBytes = sizeof(MeshGpu);
    const VkDeviceSize mBytes = VkDeviceSize{meshletCount} * sizeof(gpu::shared::MeshletGpu);
    const VkDeviceSize mvBytes = VkDeviceSize{meshletVertexCount} * sizeof(uint32_t);
    const VkDeviceSize mtBytes = VkDeviceSize{meshletTriangleCount} * sizeof(uint32_t);
    const VkDeviceSize meshletAt = vBytes + iBytes + tBytes + eBytes;
    GpuBuffer staging = m_ctx.allocator->createBuffer(meshletAt + mBytes + mvBytes + mtBytes,
                                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                      AllocationUsage::CpuToGpu, true);
    if (!staging.isValid() || !staging.getMappedData()) {
        m_ctx.allocator->destroyBuffer(staging);
        OHAO_LOG(Error, render, "RenderScene: registerMesh '{}': staging allocation failed", data.debugName);
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
    ok = ok && reserve(m_meshlets, meshletCount, cmd, retired, grown);
    ok = ok && reserve(m_meshletVertices, meshletVertexCount, cmd, retired, grown);
    ok = ok && reserve(m_meshletTriangles, meshletTriangleCount, cmd, retired, grown);

    if (ok) {
        mesh.vertexOffset = static_cast<uint32_t>(m_vertices.used);
        mesh.indexOffset = static_cast<uint32_t>(m_indices.used);
        mesh.triangleSlotOffset = static_cast<uint32_t>(m_triangleSlots.used);
        const MeshGpu entry{mesh.vertexOffset, mesh.indexOffset, mesh.indexCount, mesh.triangleSlotOffset};
        const auto meshletBase = static_cast<uint32_t>(m_meshlets.used);
        for (auto& sm : submeshMeshlets) sm[0] += meshletBase;
        mesh.submeshMeshlets = std::move(submeshMeshlets);

        auto* dst = static_cast<std::byte*>(staging.getMappedData());
        std::memcpy(dst, data.vertices.data(), static_cast<std::size_t>(vBytes));
        std::memcpy(dst + vBytes, data.indices.data(), static_cast<std::size_t>(iBytes));
        if (tBytes) std::memcpy(dst + vBytes + iBytes, triangleSlots.data(), static_cast<std::size_t>(tBytes));
        std::memcpy(dst + vBytes + iBytes + tBytes, &entry, sizeof(entry));
        // Meshlets with arena offsets; their vertices stay mesh-local (a draw adds its
        // vertex offset, as for the index arena).
        auto* gpuMeshlets = reinterpret_cast<gpu::shared::MeshletGpu*>(dst + meshletAt);
        const auto vertexBase = static_cast<uint32_t>(m_meshletVertices.used);
        const auto triangleBase = static_cast<uint32_t>(m_meshletTriangles.used);
        for (uint32_t i = 0; i < meshletCount; ++i) {
            const Meshlet& s = meshlets.meshlets[i];
            gpu::shared::MeshletGpu g{};
            g.range = {vertexBase + s.vertexOffset, triangleBase + s.triangleOffset, s.vertexCount, s.triangleCount};
            g.sphere = {s.center.x, s.center.y, s.center.z, s.radius};
            g.cone = {s.coneAxis.x, s.coneAxis.y, s.coneAxis.z, s.coneCutoff};
            std::memcpy(gpuMeshlets + i, &g, sizeof(g));
        }
        if (mvBytes) std::memcpy(dst + meshletAt + mBytes, meshlets.vertices.data(), static_cast<std::size_t>(mvBytes));
        if (mtBytes)
            std::memcpy(dst + meshletAt + mBytes + mvBytes, meshlets.triangles.data(), static_cast<std::size_t>(mtBytes));
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
        if (mBytes) {
            const VkBufferCopy mCopy{meshletAt, m_meshlets.used * sizeof(gpu::shared::MeshletGpu), mBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshlets.buf.buffer, 1, &mCopy);
        }
        if (mvBytes) {
            const VkBufferCopy mvCopy{meshletAt + mBytes, m_meshletVertices.used * sizeof(uint32_t), mvBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshletVertices.buf.buffer, 1, &mvCopy);
        }
        if (mtBytes) {
            const VkBufferCopy mtCopy{meshletAt + mBytes + mvBytes, m_meshletTriangles.used * sizeof(uint32_t), mtBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshletTriangles.buf.buffer, 1, &mtCopy);
        }
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
    m_meshlets.used += meshletCount;
    m_meshletVertices.used += meshletVertexCount;
    m_meshletTriangles.used += meshletTriangleCount;

    // A built BLAS (FAST_TRACE, no ALLOW_UPDATE) does not reference its inputs, so
    // existing BLASes survive arena growth untouched.
    buildBlas(mesh);

    const auto index = static_cast<uint32_t>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    return MeshHandle{index, 0};
}

BlasHandle MeshRegistry::release(MeshHandle h) {
    if (!info(h)) return INVALID_BLAS;
    MeshInfo& mesh = m_meshes[h.index];
    mesh.released = true;
    const BlasHandle blas = mesh.blas;
    mesh.blas = INVALID_BLAS;
    mesh.submeshes.clear();
    mesh.submeshes.shrink_to_fit();
    mesh.submeshMeshlets.clear();
    mesh.submeshMeshlets.shrink_to_fit();
    return blas;
}

std::vector<Vertex> MeshRegistry::readVertices(MeshHandle h) {
    const MeshInfo* mesh = info(h);
    if (!mesh || mesh->vertexCount == 0) return {};
    const VkDeviceSize bytes = VkDeviceSize{mesh->vertexCount} * kVertexStride;
    GpuBuffer staging = m_ctx.allocator->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, AllocationUsage::GpuToCpu,
                                                      true);
    if (!staging.isValid() || !staging.getMappedData()) {
        m_ctx.allocator->destroyBuffer(staging);
        return {};
    }
    VkCommandBuffer cmd = beginOneShot();
    if (cmd == VK_NULL_HANDLE) {
        m_ctx.allocator->destroyBuffer(staging);
        return {};
    }
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0,
                         nullptr, 0, nullptr);
    const VkBufferCopy copy{VkDeviceSize{mesh->vertexOffset} * kVertexStride, 0, bytes};
    vkCmdCopyBuffer(cmd, m_vertices.buf.buffer, staging.buffer, 1, &copy);
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0,
                         nullptr);
    endOneShot(cmd);
    m_ctx.allocator->invalidateBuffer(staging);
    std::vector<Vertex> out(mesh->vertexCount);
    std::memcpy(out.data(), staging.getMappedData(), static_cast<std::size_t>(bytes));
    m_ctx.allocator->destroyBuffer(staging);
    return out;
}

bool MeshRegistry::setBounds(MeshHandle h, const glm::vec3& mn, const glm::vec3& mx) {
    if (!info(h)) return false;
    MeshInfo& mesh = m_meshes[h.index];
    if (mesh.aabbMin == mn && mesh.aabbMax == mx) return false;
    mesh.aabbMin = mn;
    mesh.aabbMax = mx;
    return true;
}

const MeshInfo* MeshRegistry::info(MeshHandle h) const {
    if (!h.valid() || h.index >= m_meshes.size() || h.generation != 0 || m_meshes[h.index].released) return nullptr;
    return &m_meshes[h.index];
}

} // namespace ohao::render
