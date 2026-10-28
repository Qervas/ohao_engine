#pragma once
#include "render/scene/render_scene_types.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>
#include <vector>

namespace ohao::render {

// Mesh arenas: every registered mesh is appended to one shared vertex arena, index
// arena (mesh-local indices), triangle material-slot stream and mesh table, and its
// submeshes' meshlets (render/scene/meshlet_builder) to the meshlet arenas: the meshlets
// (gpu::shared::MeshletGpu), their vertices (mesh-local indices) and their packed local
// triangles. Each mesh owns one BLAS built against the arenas, from its original
// triangles. Registration is load-time work: it uploads and builds synchronously.
class MeshRegistry {
public:
    struct Context {
        GpuAllocator* allocator; VkDevice device; VkQueue queue; VkCommandPool commandPool;
        RTAccelerationStructure* rt;   // null when RT is unavailable
    };

    [[nodiscard]] bool initialize(const Context& ctx);
    void shutdown();   // destroys the arenas and every BLAS; the caller idles the device

    // Appends `data`. Arenas that must grow are replaced; the replaced buffers are
    // pushed to `retired` (the caller defers their destruction); built BLASes do not
    // reference their inputs and stay as they are. `grown` counts the arenas that grew.
    MeshHandle registerMesh(const MeshData& data, std::vector<GpuBuffer>& retired, uint32_t& grown);

    // A skinned instance's own mesh (MeshInfo::skinned): a new vertex range the size of `base`'s,
    // filled with its vertices (the bind pose, until the skin pass writes the instance's pose), a
    // mesh-table entry sharing its indices and triangle slots, copies of its meshlets, and an
    // updatable BLAS. Synchronous, as registerMesh.
    MeshHandle registerSkinnedCopy(MeshHandle base, std::vector<GpuBuffer>& retired, uint32_t& grown);

    // Marks the mesh released (info() returns null from now on; its index is never
    // reused, so stale handles stay detectably stale) and hands back its BLAS for the
    // caller to destroy once no frame in flight uses it. INVALID_BLAS if none or unknown.
    [[nodiscard]] BlasHandle release(MeshHandle h);
    [[nodiscard]] const MeshInfo* info(MeshHandle h) const;
    // A mesh's vertices as they are in the arena now (a skinned mesh's: its last skinned pose,
    // previous positions in boneWeights.xyz). Blocking: waits for the queue to idle. Tests, tools.
    [[nodiscard]] std::vector<Vertex> readVertices(MeshHandle h);
    // A skinned mesh's local box (its pose); true when it changed.
    bool setBounds(MeshHandle h, const glm::vec3& mn, const glm::vec3& mx);
    [[nodiscard]] uint32_t count() const noexcept { return static_cast<uint32_t>(m_meshes.size()); }

    [[nodiscard]] VkBuffer vertexArena() const noexcept { return m_vertices.buf.buffer; }
    [[nodiscard]] VkBuffer indexArena() const noexcept { return m_indices.buf.buffer; }
    [[nodiscard]] VkBuffer triangleSlotBuffer() const noexcept { return m_triangleSlots.buf.buffer; }
    [[nodiscard]] VkBuffer meshTable() const noexcept { return m_meshTable.buf.buffer; }
    [[nodiscard]] VkBuffer meshletArena() const noexcept { return m_meshlets.buf.buffer; }
    [[nodiscard]] VkBuffer meshletVertexArena() const noexcept { return m_meshletVertices.buf.buffer; }
    [[nodiscard]] VkBuffer meshletTriangleArena() const noexcept { return m_meshletTriangles.buf.buffer; }

private:
    struct Arena {
        GpuBuffer buf;
        VkBufferUsageFlags usage{0};
        VkDeviceSize elemSize{0};
        VkDeviceSize capacity{0};   // elements
        VkDeviceSize used{0};       // elements
    };

    [[nodiscard]] bool createArena(Arena& a, VkDeviceSize elemSize, VkDeviceSize capacity, VkBufferUsageFlags usage);
    // Makes room for `count` more elements: on growth records old -> new on `cmd`.
    [[nodiscard]] bool reserve(Arena& a, VkDeviceSize count, VkCommandBuffer cmd,
                               std::vector<GpuBuffer>& retired, uint32_t& grown);
    void buildBlas(MeshInfo& mesh, bool updatable = false);
    [[nodiscard]] VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer cmd);

    Context m_ctx{};
    Arena m_vertices, m_indices, m_triangleSlots, m_meshTable;
    Arena m_meshlets, m_meshletVertices, m_meshletTriangles;
    std::vector<MeshInfo> m_meshes;
};

} // namespace ohao::render
