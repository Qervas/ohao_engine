#pragma once

// The acceleration structures of a RenderScene (VK_KHR_acceleration_structure): one BLAS a mesh, built against
// the mesh arenas, and one TLAS over instances already in device memory. Every build is recorded through a
// render::Pass (render/graph/pass.hpp): a structure's storage is a GpuBuffer, written by its build and read by
// the TLAS build, by ray queries and by a ray tracing pipeline (BufferUse::Accel), and its inputs are read as
// BufferUse::AccelInput. Nothing here submits or waits.

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include <vector>

namespace ohao {

// Handle to a BLAS entry
using BlasHandle = uint32_t;
constexpr BlasHandle INVALID_BLAS = UINT32_MAX;

// Per-BLAS data
struct BlasEntry {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    GpuBuffer buffer;               // the structure's storage
    VkDeviceAddress deviceAddress = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    // An updatable BLAS (a skinned instance's): its geometry, for refits and rebuilds.
    bool updatable = false;
    VkDeviceSize vertexStride = 0;
    uint32_t maxVertex = 0;
    uint32_t primitiveOffset = 0;   // index byte offset
    VkDeviceSize bytes = 0;         // the structure's storage
    uint32_t firstVertex = 0;
    VkDeviceSize scratchSize = 0;   // max(build, update) scratch
    bool retired = false;           // no instance names it any more: it waits for the frames in flight (retireBLAS)
};

// TLAS storage or a scratch buffer replaced inside a frame. Earlier frames may still trace or
// build with it, so the caller destroys it (destroyRetired) once they retired.
// `as` is VK_NULL_HANDLE for a retired scratch buffer.
struct RetiredTlas {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    GpuBuffer buffer;
};

class RTAccelerationStructure {
public:
    RTAccelerationStructure() = default;
    ~RTAccelerationStructure();

    // Before anything else; the structures' storage and scratch come from `allocator`. False when the device has
    // no acceleration structures or no ray tracing pipeline.
    [[nodiscard]] bool init(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator);

    [[nodiscard]] bool isSupported() const noexcept { return m_supported; }

    // === BLAS management ===

    // Records the build of a BLAS over triangles already in `vertices` (packed vec3 positions first in each
    // `vertexStride`) and `indices` (uint32). vertexByteOffset/indexByteOffset are BYTE offsets into the buffers.
    // firstVertex is added to every index: 0 for buffers holding global indices, the mesh's first vertex for
    // arenas holding mesh-local indices.
    // updatable: ALLOW_UPDATE | PREFER_FAST_BUILD (a skinned instance's, refit every frame its pose changes:
    // recordBlasUpdates); else PREFER_FAST_TRACE.
    // `scratch` is the build's own, the caller's to destroy (destroyRetired) once the build has run: a build
    // recorded for later must not share scratch with the next one.
    BlasHandle createBLAS(const render::Frame& frame, GpuBuffer& vertices, uint32_t vertexCount,
                          VkDeviceSize vertexStride, GpuBuffer& indices, uint32_t indexCount,
                          VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset, uint32_t firstVertex,
                          bool updatable, RetiredTlas& scratch);

    // The BLAS's mesh was released: no TLAS built from now on names it and no pass declares it, so that it can
    // be destroyed (destroyBLAS) once the frames in flight have retired.
    void retireBLAS(BlasHandle handle);
    void destroyBLAS(BlasHandle handle);

    // One updatable BLAS to update: a refit (MODE_UPDATE, in place) or, `rebuild`, a full build in
    // place. `recorded` is set by recordBlasUpdates.
    struct BlasUpdate {
        BlasHandle handle = INVALID_BLAS;
        bool rebuild = false;
        bool recorded = false;
    };
    // Records every update in one build call, from the BLASes' geometry in `vertices` / `indices` (the arenas
    // as they are now: they may have grown since): each gets its own slice of this object's refit scratch, so
    // the GPU builds them side by side. A scratch that must grow is retired (takeRetired). Returns how many
    // were recorded (a handle that is not an updatable BLAS is skipped).
    uint32_t recordBlasUpdates(const render::Frame& frame, std::span<BlasUpdate> updates, GpuBuffer& vertices,
                               GpuBuffer& indices);

    // === TLAS management ===

    // Records a TLAS build (rebuild = true, or no TLAS yet, or a different count) or refit (UPDATE mode) from
    // `count` VkAccelerationStructureInstanceKHR in `instances`. Storage too small for `count` is replaced and
    // the old storage goes to takeRetired(); it is never destroyed here.
    // Returns false when nothing was recorded (no RT, empty input, allocation failure).
    bool buildTLASFromDevice(const render::Frame& frame, GpuBuffer& instances, uint32_t count, bool rebuild);
    // TLAS storage and scratch replaced since the last call; the caller owns destroying it.
    [[nodiscard]] std::vector<RetiredTlas> takeRetired();
    void destroyRetired(RetiredTlas& r);

    // Declares the TLAS and every BLAS as read by `pass` (ray queries in its shaders, a ray tracing pipeline).
    void traced(render::Pass& pass);

    // === Getters ===

    VkAccelerationStructureKHR getTLAS() const { return m_tlas; }
    // The live BLASes and their storage.
    struct BlasMemory {
        uint32_t count{0};
        VkDeviceSize bytes{0};
    };
    BlasMemory blasMemory() const {
        BlasMemory m;
        for (const BlasEntry& e : m_blasEntries)
            if (e.handle != VK_NULL_HANDLE) { ++m.count; m.bytes += e.bytes; }
        return m;
    }
    // Device address of a live BLAS; 0 when the handle is unknown or destroyed.
    VkDeviceAddress getBlasAddress(BlasHandle handle) const {
        return handle < m_blasEntries.size() ? m_blasEntries[handle].deviceAddress : 0;
    }

    // Destroys everything; the caller idles the device.
    void destroy();

private:
    VkDevice m_device = VK_NULL_HANDLE;
    GpuAllocator* m_allocator = nullptr;
    bool m_supported = false;

    // Function pointers (loaded dynamically)
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddressFn = nullptr;

    // BLAS storage
    std::vector<BlasEntry> m_blasEntries;

    // TLAS: its storage's size and the instance count of its last build (an UPDATE must use the same count).
    VkAccelerationStructureKHR m_tlas = VK_NULL_HANDLE;
    GpuBuffer m_tlasBuffer;
    VkDeviceSize m_tlasStorageSize = 0;
    uint32_t m_tlasBuiltCount = 0;
    // The TLAS builds' scratch and recordBlasUpdates' (sliced per BLAS at the device's scratch offset
    // alignment): inside frames, so each is retired, not destroyed, when it grows. Sizes are what a build may
    // use past the buffer's first aligned address.
    GpuBuffer m_tlasScratch;
    VkDeviceSize m_tlasScratchSize = 0;
    GpuBuffer m_refitScratch;
    VkDeviceSize m_refitScratchSize = 0;
    VkDeviceSize m_scratchAlignment = 256;
    std::vector<RetiredTlas> m_retired;
    // recordBlasUpdates' reused arrays.
    std::vector<VkDeviceSize> m_batchOffsets;
    std::vector<VkAccelerationStructureGeometryKHR> m_batchGeometries;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> m_batchInfos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> m_batchRanges;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> m_batchRangePtrs;

    bool loadFunctionPointers();
    // A device-local buffer with a device address.
    [[nodiscard]] GpuBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    // A scratch buffer of `size` usable bytes past its first address aligned for a build's scratch.
    [[nodiscard]] GpuBuffer createScratch(VkDeviceSize size);
    [[nodiscard]] VkDeviceAddress scratchAddress(const GpuBuffer& scratch);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
};

} // namespace ohao
