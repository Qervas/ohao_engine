#pragma once

// RT Acceleration Structure Manager for OHAO Engine
//
// Manages Bottom-Level (BLAS) and Top-Level (TLAS) acceleration structures
// for Vulkan ray tracing. NVIDIA-only (VK_KHR_acceleration_structure).
//
// Usage:
//   RTAccelerationStructure accel;
//   accel.init(device, physicalDevice, graphicsQueue, commandPool);
//   uint32_t blasHandle = accel.createBLAS(vertexBuffer, indexBuffer, ...);
//   accel.addInstance(blasHandle, transform);
//   accel.buildTLAS(cmd);
//
// Each BLAS is a single geometry (mesh). TLAS holds instances referencing BLASes.

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <cstdint>
#include <iostream>

namespace ohao {

// Handle to a BLAS entry
using BlasHandle = uint32_t;
constexpr BlasHandle INVALID_BLAS = UINT32_MAX;

// Per-BLAS data
struct BlasEntry {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress deviceAddress = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
};

// TLAS storage replaced by buildTLASFromDevice. Earlier frames may still trace or
// build with it, so the caller destroys it (destroyRetired) once they retired.
// `as` is VK_NULL_HANDLE for a retired scratch buffer.
struct RetiredTlas {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// Instance referencing a BLAS with a transform
struct RTInstance {
    BlasHandle blasHandle = INVALID_BLAS;
    glm::mat4 transform = glm::mat4(1.0f);
    uint32_t customIndex = 0;   // user data (the RenderScene instance slot)
    uint32_t mask = 0xFF;       // visibility mask for ray flags
    uint32_t sbtOffset = 0;     // shader binding table offset
};

class RTAccelerationStructure {
public:
    RTAccelerationStructure() = default;
    ~RTAccelerationStructure();

    // Initialize — must be called before anything else.
    // Returns false if RT extensions are not available.
    // asConsumerStages: the pipeline stages that will READ the acceleration
    // structure, used as the destination mask of the post-build barrier. Only
    // the caller knows this -- querying device features reports what the GPU
    // supports, not what was enabled at vkCreateDevice, and naming a stage whose
    // extension was not enabled is invalid usage. Default suits the engine, which
    // enables both the RT pipeline and ray query. A ray-query-only context must
    // pass VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT alone.
    [[nodiscard]] bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              VkQueue graphicsQueue, uint32_t queueFamily, VkCommandPool commandPool,
              VkPipelineStageFlags asConsumerStages =
                  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Check if RT is supported on this device
    [[nodiscard]] bool isSupported() const noexcept { return m_supported; }

    // Get RT pipeline properties (for SBT sizing, etc.)
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& getPipelineProperties() const {
        return m_rtPipelineProperties;
    }

    // === BLAS management ===

    // Create a BLAS from vertex/index data already in GPU buffers.
    // vertexBuffer must contain packed vec3 positions (float32).
    // indexBuffer must contain uint32_t indices.
    // vertexOffset/indexOffset are BYTE offsets into the combined buffer.
    // firstVertex is added to every index: 0 for buffers holding global indices,
    // the mesh's first vertex for arenas holding mesh-local indices.
    BlasHandle createBLAS(VkBuffer vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
                          VkBuffer indexBuffer, uint32_t indexCount,
                          VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset,
                          VkCommandBuffer cmd, uint32_t firstVertex = 0);

    // Destroy a specific BLAS
    void destroyBLAS(BlasHandle handle);

    // Create a new BLAS from tightly-packed vec3 positions (stride=12).
    // For per-frame animated mesh BLAS — uses PREFER_FAST_BUILD.
    // If cmd is provided, records the build on that command buffer (caller submits).
    // If cmd is VK_NULL_HANDLE, uses single-time commands (submit + wait internally).
    BlasHandle createBLASFromPositions(VkBuffer positionBuffer, uint32_t vertexCount,
                                       VkBuffer indexBuffer, uint32_t indexCount,
                                       VkDeviceSize indexByteOffset,
                                       VkCommandBuffer cmd = VK_NULL_HANDLE);

    // === TLAS management ===

    // Clear all instances (call before re-adding for a new frame)
    void clearInstances();

    // Add an instance referencing a BLAS
    void addInstance(BlasHandle blasHandle, const glm::mat4& transform,
                     uint32_t customIndex = 0, uint32_t mask = 0xFF, uint32_t sbtOffset = 0);

    // Build/rebuild the TLAS from current instances.
    // Call once per frame after addInstance() calls.
    void buildTLAS(VkCommandBuffer cmd);

    // Record a TLAS build (rebuild = true, or no TLAS yet, or a different count) or
    // refit (UPDATE mode) on `cmd` from `count` VkAccelerationStructureInstanceKHR
    // already in device memory at `instances`. Nothing is submitted. The caller
    // must have made the instance writes visible to ACCELERATION_STRUCTURE_BUILD
    // (SHADER_READ). Barriers recorded here: before -- earlier readers (the
    // asConsumerStages from init) and earlier builds finish with the TLAS and its
    // scratch; after -- the TLAS is visible to asConsumerStages. Storage too small
    // for `count` is replaced and the old storage goes to takeRetired(); it is
    // never destroyed here. Do not mix with buildTLAS() on the same object.
    // Returns false when nothing was recorded (no RT, empty input, allocation failure).
    bool buildTLASFromDevice(VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t count, bool rebuild);
    // TLAS storage replaced since the last call; the caller owns destroying it.
    [[nodiscard]] std::vector<RetiredTlas> takeRetired();
    void destroyRetired(const RetiredTlas& r);

    // Pre-allocate scratch buffer (call before multi-BLAS rebuilds to avoid mid-recording realloc)
    void ensureScratchBuffer(VkDeviceSize requiredSize);

    // Force full TLAS rebuild (not update) on next buildTLAS call
    void forceTlasRebuild() { m_forceTlasRebuild = true; }

    // === Getters ===

    VkAccelerationStructureKHR getTLAS() const { return m_tlas; }
    uint32_t getBlasCount() const { return static_cast<uint32_t>(m_blasEntries.size()); }
    // Device address of a live BLAS; 0 when the handle is unknown or destroyed.
    VkDeviceAddress getBlasAddress(BlasHandle handle) const {
        return handle < m_blasEntries.size() ? m_blasEntries[handle].deviceAddress : 0;
    }
    uint32_t getInstanceCount() const { return static_cast<uint32_t>(m_instances.size()); }

    // Cleanup everything
    void destroy();

private:
    // Vulkan handles (not owned — caller manages lifetime)
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;

    bool m_supported = false;

    // RT properties
    VkPhysicalDeviceAccelerationStructureFeaturesKHR m_asFeatures{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtPipelineProperties{};

    // Destination stage mask of the post-TLAS-build barrier; see init().
    VkPipelineStageFlags m_asConsumerStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    // Function pointers (loaded dynamically)
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddressFn = nullptr;

    // BLAS storage
    std::vector<BlasEntry> m_blasEntries;

    // Instance list (rebuilt each frame)
    std::vector<RTInstance> m_instances;

    // TLAS
    VkAccelerationStructureKHR m_tlas = VK_NULL_HANDLE;
    bool m_forceTlasRebuild{false};
    VkBuffer m_tlasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_tlasMemory = VK_NULL_HANDLE;
    // buildTLASFromDevice state: storage size of m_tlas, instance count of its
    // last build (an UPDATE must use the same count), and a scratch buffer of its
    // own -- the BLAS scratch is resized under blocking submits, this one inside
    // frames, so it is retired rather than destroyed when it grows.
    VkDeviceSize m_tlasStorageSize = 0;
    uint32_t m_tlasBuiltCount = 0;
    VkBuffer m_tlasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_tlasScratchMemory = VK_NULL_HANDLE;
    VkDeviceSize m_tlasScratchSize = 0;
    std::vector<RetiredTlas> m_retired;

    // Instance buffer (GPU-side VkAccelerationStructureInstanceKHR array)
    VkBuffer m_instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_instanceMemory = VK_NULL_HANDLE;
    uint32_t m_instanceBufferCapacity = 0;

    // Scratch buffer (reused across builds)
    VkBuffer m_scratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_scratchMemory = VK_NULL_HANDLE;
    VkDeviceSize m_scratchSize = 0;

    // Helpers
    bool loadFunctionPointers();
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                      VkBuffer& buffer, VkDeviceMemory& memory);
    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
    void ensureInstanceBuffer(uint32_t requiredCount);

    // One-shot command helpers
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer cmd);
};

} // namespace ohao
