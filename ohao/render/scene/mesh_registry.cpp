#include "render/scene/mesh_registry.hpp"
#include "asset/meshlets.hpp"
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
    m_work.initialize(ctx.device, ctx.queue, ctx.commandPool);
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
    m_work.shutdown();
    if (m_ctx.rt) {
        for (auto& mesh : m_meshes) {
            if (mesh.blas != INVALID_BLAS) m_ctx.rt->destroyBLAS(mesh.blas);
            mesh.blas = INVALID_BLAS;
        }
    }
    m_meshes.clear();
    m_generations.clear();
    m_freeIndices.clear();
    m_live = 0;
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

bool MeshRegistry::reserve(Arena& a, VkDeviceSize count, std::vector<Growth>& growths,
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
    if (a.used > 0) growths.push_back(Growth{a.buf, &a, a.used * a.elemSize});
    retired.push_back(a.buf);
    a.buf = bigger;
    a.capacity = newCapacity;
    ++grown;
    return true;
}

void MeshRegistry::recordGrowth(const render::Frame& frame, std::vector<Growth>& growths) {
    if (growths.empty()) return;
    render::Pass pass(frame, nullptr, render::Pass::Transfer);
    for (Growth& g : growths) pass.read(g.from, render::BufferUse::CopySrc).write(g.arena->buf, render::BufferUse::CopyDst);
    pass.begin();
    for (const Growth& g : growths) {
        const VkBufferCopy region{0, 0, g.bytes};
        vkCmdCopyBuffer(frame.cmd, g.from.buffer, g.arena->buf.buffer, 1, &region);
    }
    growths.clear();
}

bool MeshRegistry::take(Arena& a, VkDeviceSize count, std::vector<Growth>& growths, std::vector<GpuBuffer>& retired,
                        uint32_t& grown, uint32_t& first) {
    first = 0;
    if (count == 0) return true;
    for (auto it = a.free.begin(); it != a.free.end(); ++it) {
        if (it->second < count) continue;
        first = static_cast<uint32_t>(it->first);
        const VkDeviceSize rest = it->second - count;
        a.free.erase(it);
        if (rest > 0) a.free.emplace(first + count, rest);
        a.live += count;
        return true;
    }
    if (!reserve(a, count, growths, retired, grown)) return false;
    first = static_cast<uint32_t>(a.used);
    a.used += count;
    a.live += count;
    return true;
}

void MeshRegistry::give(Arena& a, VkDeviceSize first, VkDeviceSize count) {
    if (count == 0) return;
    a.live -= count;
    auto next = a.free.lower_bound(first);
    if (next != a.free.begin()) {
        const auto before = std::prev(next);
        if (before->first + before->second == first) {   // merges with the range before it
            first = before->first;
            count += before->second;
            a.free.erase(before);
        }
    }
    if (next != a.free.end() && first + count == next->first) {   // and with the one after
        count += next->second;
        a.free.erase(next);
    }
    if (first + count == a.used) a.used = first;   // the arena's end moves back
    else a.free.emplace(first, count);
}

bool MeshRegistry::takeIndex(std::vector<Growth>& growths, std::vector<GpuBuffer>& retired, uint32_t& grown,
                             uint32_t& index) {
    if (!m_freeIndices.empty()) {
        index = m_freeIndices.back();
        m_freeIndices.pop_back();
        return true;
    }
    if (!reserve(m_meshTable, 1, growths, retired, grown)) return false;
    index = static_cast<uint32_t>(m_meshTable.used++);
    return true;
}

MeshHandle MeshRegistry::place(uint32_t index, MeshInfo&& mesh) {
    if (index >= m_meshes.size()) {
        m_meshes.resize(index + 1);
        m_generations.resize(index + 1, 0);
    }
    m_meshes[index] = std::move(mesh);
    ++m_meshTable.live;
    ++m_live;
    return MeshHandle{index, m_generations[index]};
}

void MeshRegistry::submit(VkCommandBuffer cmd, GpuBuffer staging, RetiredTlas scratch) {
    GpuAllocator* allocator = m_ctx.allocator;
    RTAccelerationStructure* rt = m_ctx.rt;
    m_work.submit(cmd, "mesh upload", staging.sizeBytes(), [allocator, rt, staging, scratch]() mutable {
        allocator->destroyBuffer(staging);
        if (rt) rt->destroyRetired(scratch);
    });
}

void MeshRegistry::buildBlas(MeshInfo& mesh, const render::Frame& frame, RetiredTlas& scratch, bool updatable) {
    mesh.blas = INVALID_BLAS;
    if (!m_ctx.rt || !m_ctx.rt->isSupported() || mesh.indexCount < 3) return;
    mesh.blas = m_ctx.rt->createBLAS(
        frame, m_vertices.buf, mesh.vertexCount, kVertexStride,
        m_indices.buf, mesh.indexCount,
        static_cast<VkDeviceSize>(mesh.vertexOffset) * kVertexStride,
        static_cast<VkDeviceSize>(mesh.indexOffset) * sizeof(uint32_t),
        mesh.vertexOffset,       // arena indices are mesh-local
        updatable, scratch);
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

    VkCommandBuffer cmd = m_work.begin();
    if (cmd == VK_NULL_HANDLE) return {};
    // A load's commands: a frame with no timer.
    const render::Frame frame{cmd, nullptr, 0};
    std::vector<Growth> growths;
    uint32_t index = 0, firstCopy = 0;
    const bool tookVertices = take(m_vertices, mesh.vertexCount, growths, retired, grown, mesh.vertexOffset);
    const bool tookMeshlets = tookVertices && take(m_meshlets, meshletCount, growths, retired, grown, firstCopy);
    bool ok = tookMeshlets && takeIndex(growths, retired, grown, index);
    const bool tookIndex = ok;
    recordGrowth(frame, growths);
    GpuBuffer staging{};
    if (ok) {
        const MeshGpu entry{mesh.vertexOffset, mesh.indexOffset, mesh.indexCount, mesh.triangleSlotOffset};
        staging = m_ctx.allocator->createBuffer(sizeof(MeshGpu), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                AllocationUsage::CpuToGpu, true);
        ok = staging.isValid() && staging.getMappedData();
        if (ok) {
            std::memcpy(staging.getMappedData(), &entry, sizeof(entry));
            m_ctx.allocator->flushBuffer(staging);
        }
    }
    if (ok) {
        // The base's vertices and meshlets to the copy's ranges of the same arenas, and the mesh's table row.
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.readWrite(m_vertices.buf, render::BufferUse::CopyDst);
        if (meshletCount > 0) pass.readWrite(m_meshlets.buf, render::BufferUse::CopyDst);
        pass.read(staging, render::BufferUse::CopySrc).write(m_meshTable.buf, render::BufferUse::CopyDst);
        pass.begin();
        const VkBufferCopy vCopy{VkDeviceSize{base->vertexOffset} * kVertexStride,
                                 VkDeviceSize{mesh.vertexOffset} * kVertexStride,
                                 VkDeviceSize{mesh.vertexCount} * kVertexStride};
        vkCmdCopyBuffer(cmd, m_vertices.buf.buffer, m_vertices.buf.buffer, 1, &vCopy);
        // Meshlets: the base's, one range per mesh (registerMesh places a mesh's at once).
        if (meshletCount > 0) {
            const VkBufferCopy mCopy{VkDeviceSize{baseFirst} * sizeof(gpu::shared::MeshletGpu),
                                     VkDeviceSize{firstCopy} * sizeof(gpu::shared::MeshletGpu),
                                     VkDeviceSize{meshletCount} * sizeof(gpu::shared::MeshletGpu)};
            vkCmdCopyBuffer(cmd, m_meshlets.buf.buffer, m_meshlets.buf.buffer, 1, &mCopy);
        }
        for (auto& sm : mesh.submeshMeshlets)
            if (sm[1] > 0) sm[0] = firstCopy + (sm[0] - baseFirst);
        mesh.firstMeshlet = firstCopy;
        mesh.meshletCount = meshletCount;
        mesh.meshletRange = {firstCopy, meshletCount};
        mesh.meshletVertexRange = {};     // the base's
        mesh.meshletTriangleRange = {};
        const VkBufferCopy eCopy{0, VkDeviceSize{index} * sizeof(MeshGpu), sizeof(MeshGpu)};
        vkCmdCopyBuffer(cmd, staging.buffer, m_meshTable.buf.buffer, 1, &eCopy);
    }
    RetiredTlas scratch;
    if (ok) buildBlas(mesh, frame, scratch, true);
    submit(cmd, staging, scratch);
    if (!ok) {
        if (tookVertices) give(m_vertices, mesh.vertexOffset, mesh.vertexCount);
        if (tookMeshlets) give(m_meshlets, firstCopy, meshletCount);
        if (tookIndex) m_freeIndices.push_back(index);
        return {};
    }
    return place(index, std::move(mesh));
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
    MeshletSet built;
    std::vector<std::array<uint32_t, 2>> submeshMeshlets = data.submeshMeshlets;
    std::span<const Meshlet> meshlets = data.meshlets;
    std::span<const uint32_t> meshletVertices = data.meshletVertices, meshletTriangles = data.meshletTriangles;
    if (submeshMeshlets.size() != mesh.submeshes.size()) {   // not cooked (a mesh made in code): built here
        submeshMeshlets.clear();
        for (const SubmeshDesc& s : mesh.submeshes) {
            const auto first = static_cast<uint32_t>(built.meshlets.size());
            buildMeshlets(data.vertices, data.indices, s.firstIndex, s.indexCount, built);
            submeshMeshlets.push_back({first, static_cast<uint32_t>(built.meshlets.size()) - first});
        }
        meshlets = built.meshlets;
        meshletVertices = built.vertices;
        meshletTriangles = built.triangles;
    }
    const auto meshletCount = static_cast<uint32_t>(meshlets.size());
    const auto meshletVertexCount = static_cast<uint32_t>(meshletVertices.size());
    const auto meshletTriangleCount = static_cast<uint32_t>(meshletTriangles.size());

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

    VkCommandBuffer cmd = m_work.begin();
    if (cmd == VK_NULL_HANDLE) {
        m_ctx.allocator->destroyBuffer(staging);
        return {};
    }

    // A load's commands: a frame with no timer.
    const render::Frame frame{cmd, nullptr, 0};
    // Each range as it is taken, so that a failure gives back exactly what was.
    std::vector<Growth> growths;
    uint32_t index = 0, meshletBase = 0, vertexBase = 0, triangleBase = 0;
    int took = 0;
    bool ok = true;
    const auto takeNext = [&](Arena& a, VkDeviceSize count, uint32_t& first) {
        ok = ok && take(a, count, growths, retired, grown, first);
        took += ok ? 1 : 0;
    };
    takeNext(m_vertices, vertexCount, mesh.vertexOffset);
    takeNext(m_indices, indexCount, mesh.indexOffset);
    takeNext(m_triangleSlots, triangleCount, mesh.triangleSlotOffset);
    takeNext(m_meshlets, meshletCount, meshletBase);
    takeNext(m_meshletVertices, meshletVertexCount, vertexBase);
    takeNext(m_meshletTriangles, meshletTriangleCount, triangleBase);
    ok = ok && takeIndex(growths, retired, grown, index);
    took += ok ? 1 : 0;
    recordGrowth(frame, growths);

    if (ok) {
        mesh.meshletRange = {meshletBase, meshletCount};
        mesh.meshletVertexRange = {vertexBase, meshletVertexCount};
        mesh.meshletTriangleRange = {triangleBase, meshletTriangleCount};
        const MeshGpu entry{mesh.vertexOffset, mesh.indexOffset, mesh.indexCount, mesh.triangleSlotOffset};
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
        for (uint32_t i = 0; i < meshletCount; ++i) {
            const Meshlet& s = meshlets[i];
            gpu::shared::MeshletGpu g{};
            g.range = {vertexBase + s.vertexOffset, triangleBase + s.triangleOffset, s.vertexCount, s.triangleCount};
            g.sphere = {s.center.x, s.center.y, s.center.z, s.radius};
            g.cone = {s.coneAxis.x, s.coneAxis.y, s.coneAxis.z, s.coneCutoff};
            std::memcpy(gpuMeshlets + i, &g, sizeof(g));
        }
        if (mvBytes) std::memcpy(dst + meshletAt + mBytes, meshletVertices.data(), static_cast<std::size_t>(mvBytes));
        if (mtBytes)
            std::memcpy(dst + meshletAt + mBytes + mvBytes, meshletTriangles.data(), static_cast<std::size_t>(mtBytes));
        m_ctx.allocator->flushBuffer(staging);

        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.read(staging, render::BufferUse::CopySrc);
        pass.write(m_vertices.buf, render::BufferUse::CopyDst).write(m_indices.buf, render::BufferUse::CopyDst);
        if (tBytes) pass.write(m_triangleSlots.buf, render::BufferUse::CopyDst);
        pass.write(m_meshTable.buf, render::BufferUse::CopyDst);
        if (mBytes) pass.write(m_meshlets.buf, render::BufferUse::CopyDst);
        if (mvBytes) pass.write(m_meshletVertices.buf, render::BufferUse::CopyDst);
        if (mtBytes) pass.write(m_meshletTriangles.buf, render::BufferUse::CopyDst);
        pass.begin();
        const VkBufferCopy vCopy{0, VkDeviceSize{mesh.vertexOffset} * kVertexStride, vBytes};
        vkCmdCopyBuffer(cmd, staging.buffer, m_vertices.buf.buffer, 1, &vCopy);
        const VkBufferCopy iCopy{vBytes, VkDeviceSize{mesh.indexOffset} * sizeof(uint32_t), iBytes};
        vkCmdCopyBuffer(cmd, staging.buffer, m_indices.buf.buffer, 1, &iCopy);
        if (tBytes) {
            const VkBufferCopy tCopy{vBytes + iBytes, VkDeviceSize{mesh.triangleSlotOffset} * sizeof(uint32_t), tBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_triangleSlots.buf.buffer, 1, &tCopy);
        }
        const VkBufferCopy eCopy{vBytes + iBytes + tBytes, VkDeviceSize{index} * sizeof(MeshGpu), eBytes};
        vkCmdCopyBuffer(cmd, staging.buffer, m_meshTable.buf.buffer, 1, &eCopy);
        if (mBytes) {
            const VkBufferCopy mCopy{meshletAt, VkDeviceSize{meshletBase} * sizeof(gpu::shared::MeshletGpu), mBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshlets.buf.buffer, 1, &mCopy);
        }
        if (mvBytes) {
            const VkBufferCopy mvCopy{meshletAt + mBytes, VkDeviceSize{vertexBase} * sizeof(uint32_t), mvBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshletVertices.buf.buffer, 1, &mvCopy);
        }
        if (mtBytes) {
            const VkBufferCopy mtCopy{meshletAt + mBytes + mvBytes, VkDeviceSize{triangleBase} * sizeof(uint32_t), mtBytes};
            vkCmdCopyBuffer(cmd, staging.buffer, m_meshletTriangles.buf.buffer, 1, &mtCopy);
        }
    }

    // A built BLAS (FAST_TRACE, no ALLOW_UPDATE) does not reference its inputs, so
    // existing BLASes survive arena growth untouched.
    RetiredTlas scratch;
    if (ok) buildBlas(mesh, frame, scratch);
    submit(cmd, staging, scratch);
    if (!ok) {
        if (took > 0) give(m_vertices, mesh.vertexOffset, vertexCount);
        if (took > 1) give(m_indices, mesh.indexOffset, indexCount);
        if (took > 2) give(m_triangleSlots, mesh.triangleSlotOffset, triangleCount);
        if (took > 3) give(m_meshlets, meshletBase, meshletCount);
        if (took > 4) give(m_meshletVertices, vertexBase, meshletVertexCount);
        if (took > 5) give(m_meshletTriangles, triangleBase, meshletTriangleCount);
        if (took > 6) m_freeIndices.push_back(index);
        return {};
    }

    return place(index, std::move(mesh));
}

BlasHandle MeshRegistry::release(MeshHandle h) {
    if (!info(h)) return INVALID_BLAS;
    MeshInfo& mesh = m_meshes[h.index];
    mesh.released = true;
    const BlasHandle blas = mesh.blas;
    if (m_ctx.rt && blas != INVALID_BLAS) m_ctx.rt->retireBLAS(blas);
    mesh.blas = INVALID_BLAS;
    mesh.submeshes.clear();
    mesh.submeshes.shrink_to_fit();
    mesh.submeshMeshlets.clear();
    mesh.submeshMeshlets.shrink_to_fit();
    --m_live;
    return blas;
}

void MeshRegistry::reclaim(uint32_t index) {
    if (index >= m_meshes.size() || !m_meshes[index].released || m_meshes[index].vertexCount == 0) return;
    MeshInfo& mesh = m_meshes[index];
    give(m_vertices, mesh.vertexOffset, mesh.vertexCount);
    give(m_meshlets, mesh.meshletRange[0], mesh.meshletRange[1]);
    if (!mesh.skinned) {   // a skinned copy's indices, slots and meshlet data are its base's
        give(m_indices, mesh.indexOffset, mesh.indexCount);
        give(m_triangleSlots, mesh.triangleSlotOffset, mesh.indexCount / 3);
        give(m_meshletVertices, mesh.meshletVertexRange[0], mesh.meshletVertexRange[1]);
        give(m_meshletTriangles, mesh.meshletTriangleRange[0], mesh.meshletTriangleRange[1]);
    }
    mesh = MeshInfo{};
    mesh.released = true;   // vertexCount 0: reclaimed
    --m_meshTable.live;
    ++m_generations[index];
    m_freeIndices.push_back(index);
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
    VkCommandBuffer cmd = m_work.begin();
    if (cmd == VK_NULL_HANDLE) {
        m_ctx.allocator->destroyBuffer(staging);
        return {};
    }
    const render::Frame frame{cmd, nullptr, 0};
    {
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.read(m_vertices.buf, render::BufferUse::CopySrc).write(staging, render::BufferUse::CopyDst);
        pass.begin();
        const VkBufferCopy copy{VkDeviceSize{mesh->vertexOffset} * kVertexStride, 0, bytes};
        vkCmdCopyBuffer(cmd, m_vertices.buf.buffer, staging.buffer, 1, &copy);
    }
    {
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.read(staging, render::BufferUse::HostRead);
        pass.begin();
    }
    m_work.submit(cmd, "mesh read", 0, {});
    m_work.waitAll();
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
    if (!h.valid() || h.index >= m_meshes.size() || h.generation != m_generations[h.index] || m_meshes[h.index].released)
        return nullptr;
    return &m_meshes[h.index];
}

} // namespace ohao::render
