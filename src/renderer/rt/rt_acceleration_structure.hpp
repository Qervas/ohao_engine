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

// Instance referencing a BLAS with a transform
struct RTInstance {
    BlasHandle blasHandle = INVALID_BLAS;
    glm::mat4 transform = glm::mat4(1.0f);
    uint32_t customIndex = 0;   // user data (e.g. actor index)
    uint32_t mask = 0xFF;       // visibility mask for ray flags
    uint32_t sbtOffset = 0;     // shader binding table offset
};

class RTAccelerationStructure {
public:
    RTAccelerationStructure() = default;
    ~RTAccelerationStructure();

    // Initialize — must be called before anything else.
    // Returns false if RT extensions are not available.
    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              VkQueue graphicsQueue, uint32_t queueFamily, VkCommandPool commandPool);

    // Check if RT is supported on this device
    bool isSupported() const { return m_supported; }

    // Get RT pipeline properties (for SBT sizing, etc.)
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& getPipelineProperties() const {
        return m_rtPipelineProperties;
    }

    // === BLAS management ===

    // Create a BLAS from vertex/index data already in GPU buffers.
    // vertexBuffer must contain packed vec3 positions (float32).
    // indexBuffer must contain uint32_t indices.
    // vertexOffset/indexOffset are BYTE offsets into the combined buffer.
    BlasHandle createBLAS(VkBuffer vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
                          VkBuffer indexBuffer, uint32_t indexCount,
                          VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset,
                          VkCommandBuffer cmd);

    // Destroy a specific BLAS
    void destroyBLAS(BlasHandle handle);

    // === TLAS management ===

    // Clear all instances (call before re-adding for a new frame)
    void clearInstances();

    // Add an instance referencing a BLAS
    void addInstance(BlasHandle blasHandle, const glm::mat4& transform,
                     uint32_t customIndex = 0, uint32_t mask = 0xFF, uint32_t sbtOffset = 0);

    // Build/rebuild the TLAS from current instances.
    // Call once per frame after addInstance() calls.
    void buildTLAS(VkCommandBuffer cmd);

    // === Getters ===

    VkAccelerationStructureKHR getTLAS() const { return m_tlas; }
    uint32_t getBlasCount() const { return static_cast<uint32_t>(m_blasEntries.size()); }
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
    VkBuffer m_tlasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_tlasMemory = VK_NULL_HANDLE;

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
    void ensureScratchBuffer(VkDeviceSize requiredSize);
    void ensureInstanceBuffer(uint32_t requiredCount);

    // One-shot command helpers
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer cmd);
};

} // namespace ohao
