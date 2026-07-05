#include "rt_acceleration_structure.hpp"
#include <cstring>
#include <algorithm>

namespace ohao {

RTAccelerationStructure::~RTAccelerationStructure() {
    destroy();
}

bool RTAccelerationStructure::init(VkDevice device, VkPhysicalDevice physicalDevice,
                                    VkQueue graphicsQueue, uint32_t queueFamily,
                                    VkCommandPool commandPool) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_queue = graphicsQueue;
    m_queueFamily = queueFamily;
    m_commandPool = commandPool;

    // Check if RT extensions are available
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, extensions.data());

    bool hasAS = false, hasRTPipeline = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) hasAS = true;
        if (strcmp(ext.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) hasRTPipeline = true;
    }

    if (!hasAS || !hasRTPipeline) {
        std::cerr << "[RT] Acceleration structure or ray tracing pipeline extension not available" << std::endl;
        m_supported = false;
        return false;
    }

    // Load function pointers
    if (!loadFunctionPointers()) {
        std::cerr << "[RT] Failed to load RT function pointers" << std::endl;
        m_supported = false;
        return false;
    }

    // Query RT pipeline properties
    m_rtPipelineProperties = {};
    m_rtPipelineProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &m_rtPipelineProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

    std::cout << "[RT] Ray tracing supported!" << std::endl;
    std::cout << "[RT]   maxRayRecursionDepth: " << m_rtPipelineProperties.maxRayRecursionDepth << std::endl;
    std::cout << "[RT]   shaderGroupHandleSize: " << m_rtPipelineProperties.shaderGroupHandleSize << std::endl;

    m_supported = true;
    return true;
}

bool RTAccelerationStructure::loadFunctionPointers() {
    vkCreateAccelerationStructureKHR =
        (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(m_device, "vkCreateAccelerationStructureKHR");
    vkDestroyAccelerationStructureKHR =
        (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(m_device, "vkDestroyAccelerationStructureKHR");
    vkGetAccelerationStructureBuildSizesKHR =
        (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureBuildSizesKHR");
    vkCmdBuildAccelerationStructuresKHR =
        (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(m_device, "vkCmdBuildAccelerationStructuresKHR");
    vkGetAccelerationStructureDeviceAddressKHR =
        (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureDeviceAddressKHR");
    vkGetBufferDeviceAddressFn =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddress");

    return vkCreateAccelerationStructureKHR && vkDestroyAccelerationStructureKHR &&
           vkGetAccelerationStructureBuildSizesKHR && vkCmdBuildAccelerationStructuresKHR &&
           vkGetAccelerationStructureDeviceAddressKHR && vkGetBufferDeviceAddressFn;
}

// === Buffer helpers ===

uint32_t RTAccelerationStructure::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool RTAccelerationStructure::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                            VkMemoryPropertyFlags memProps,
                                            VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, buffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, memProps);
    allocInfo.pNext = &allocFlags;

    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(m_device, buffer, memory, 0) != VK_SUCCESS) return false;

    return true;
}

void RTAccelerationStructure::destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory) {
    if (buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
    if (memory != VK_NULL_HANDLE) { vkFreeMemory(m_device, memory, nullptr); memory = VK_NULL_HANDLE; }
}

VkDeviceAddress RTAccelerationStructure::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer;
    return vkGetBufferDeviceAddressFn(m_device, &addressInfo);
}

VkCommandBuffer RTAccelerationStructure::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    return cmd;
}

void RTAccelerationStructure::endSingleTimeCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

void RTAccelerationStructure::ensureScratchBuffer(VkDeviceSize requiredSize) {
    if (m_scratchSize >= requiredSize) return;

    destroyBuffer(m_scratchBuffer, m_scratchMemory);

    // Align to acceleration structure scratch alignment
    VkDeviceSize alignment = 256; // minimum for AS scratch
    requiredSize = (requiredSize + alignment - 1) & ~(alignment - 1);

    createBuffer(requiredSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_scratchBuffer, m_scratchMemory);
    m_scratchSize = requiredSize;
}

void RTAccelerationStructure::ensureInstanceBuffer(uint32_t requiredCount) {
    if (m_instanceBufferCapacity >= requiredCount) return;

    destroyBuffer(m_instanceBuffer, m_instanceMemory);

    uint32_t newCap = std::max(requiredCount, m_instanceBufferCapacity * 2);
    newCap = std::max(newCap, 16u); // minimum 16

    VkDeviceSize size = newCap * sizeof(VkAccelerationStructureInstanceKHR);
    createBuffer(size,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 m_instanceBuffer, m_instanceMemory);
    m_instanceBufferCapacity = newCap;
}

// === BLAS ===

BlasHandle RTAccelerationStructure::createBLAS(VkBuffer vertexBuffer, uint32_t vertexCount,
                                                VkDeviceSize vertexStride,
                                                VkBuffer indexBuffer, uint32_t indexCount,
                                                VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset,
                                                VkCommandBuffer cmd) {
    if (!m_supported) return INVALID_BLAS;

    // Geometry: point to BASE of combined buffers (not sub-range).
    // Indices are global (pre-offset to reference correct vertices).
    // Index byte offset applied via primitiveOffset in rangeInfo.
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = getBufferDeviceAddress(vertexBuffer);  // base, no offset
    triangles.vertexStride = vertexStride;
    triangles.maxVertex = vertexCount + static_cast<uint32_t>(vertexByteOffset / vertexStride) - 1;  // max global index
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = getBufferDeviceAddress(indexBuffer);  // base, no offset

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;

    uint32_t primitiveCount = indexCount / 3;

    // Query build sizes
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &buildInfo, &primitiveCount, &sizeInfo);

    // Create BLAS buffer
    BlasEntry entry{};
    entry.vertexCount = vertexCount;
    entry.indexCount = indexCount;

    if (!createBuffer(sizeInfo.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      entry.buffer, entry.memory)) {
        std::cerr << "[RT] Failed to create BLAS buffer" << std::endl;
        return INVALID_BLAS;
    }

    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = entry.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    if (vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &entry.handle) != VK_SUCCESS) {
        std::cerr << "[RT] Failed to create BLAS" << std::endl;
        destroyBuffer(entry.buffer, entry.memory);
        return INVALID_BLAS;
    }

    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = entry.handle;
    entry.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(m_device, &addressInfo);

    // Ensure scratch buffer
    ensureScratchBuffer(sizeInfo.buildScratchSize);

    // Build
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = entry.handle;
    buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(m_scratchBuffer);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = primitiveCount;
    rangeInfo.primitiveOffset = static_cast<uint32_t>(indexByteOffset);  // byte offset into index buffer
    rangeInfo.firstVertex = 0;  // indices are already global
    rangeInfo.transformOffset = 0;

    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    bool ownCmd = (cmd == VK_NULL_HANDLE);
    if (ownCmd) cmd = beginSingleTimeCommands();

    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    if (ownCmd) endSingleTimeCommands(cmd);

    // Store and return handle
    BlasHandle handle = static_cast<BlasHandle>(m_blasEntries.size());
    m_blasEntries.push_back(entry);

    std::cout << "[RT] BLAS " << handle << " created: " << vertexCount << " verts, "
              << primitiveCount << " tris" << std::endl;

    return handle;
}

void RTAccelerationStructure::destroyBLAS(BlasHandle handle) {
    if (handle >= m_blasEntries.size()) return;
    auto& entry = m_blasEntries[handle];
    if (entry.handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(m_device, entry.handle, nullptr);
        entry.handle = VK_NULL_HANDLE;
    }
    destroyBuffer(entry.buffer, entry.memory);
    entry.deviceAddress = 0;
}

BlasHandle RTAccelerationStructure::createBLASFromPositions(
        VkBuffer positionBuffer, uint32_t vertexCount,
        VkBuffer indexBuffer, uint32_t indexCount,
        VkDeviceSize indexByteOffset, VkCommandBuffer cmd) {
    if (!m_supported) return INVALID_BLAS;

    uint32_t primitiveCount = indexCount / 3;

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = getBufferDeviceAddress(positionBuffer);
    triangles.vertexStride = 12;
    triangles.maxVertex = vertexCount - 1;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = getBufferDeviceAddress(indexBuffer);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &buildInfo, &primitiveCount, &sizeInfo);

    BlasEntry entry{};
    entry.vertexCount = vertexCount;
    entry.indexCount = indexCount;

    if (!createBuffer(sizeInfo.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      entry.buffer, entry.memory)) {
        return INVALID_BLAS;
    }

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = entry.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    if (vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &entry.handle) != VK_SUCCESS) {
        destroyBuffer(entry.buffer, entry.memory);
        return INVALID_BLAS;
    }

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = entry.handle;
    entry.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(m_device, &addressInfo);

    ensureScratchBuffer(sizeInfo.buildScratchSize);

    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = entry.handle;
    buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(m_scratchBuffer);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = primitiveCount;
    rangeInfo.primitiveOffset = static_cast<uint32_t>(indexByteOffset);

    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    bool ownCmd = (cmd == VK_NULL_HANDLE);
    if (ownCmd) cmd = beginSingleTimeCommands();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    if (ownCmd) endSingleTimeCommands(cmd);

    BlasHandle handle = static_cast<BlasHandle>(m_blasEntries.size());
    m_blasEntries.push_back(entry);
    return handle;
}

// === TLAS ===

void RTAccelerationStructure::clearInstances() {
    m_instances.clear();
}

void RTAccelerationStructure::addInstance(BlasHandle blasHandle, const glm::mat4& transform,
                                           uint32_t customIndex, uint32_t mask, uint32_t sbtOffset) {
    if (blasHandle >= m_blasEntries.size() || m_blasEntries[blasHandle].handle == VK_NULL_HANDLE) return;
    m_instances.push_back({blasHandle, transform, customIndex, mask, sbtOffset});
}

void RTAccelerationStructure::buildTLAS(VkCommandBuffer cmd) {
    if (!m_supported || m_instances.empty()) return;

    uint32_t instanceCount = static_cast<uint32_t>(m_instances.size());

    // Ensure instance buffer is large enough
    ensureInstanceBuffer(instanceCount);

    // Fill instance buffer
    VkAccelerationStructureInstanceKHR* mapped = nullptr;
    vkMapMemory(m_device, m_instanceMemory, 0, instanceCount * sizeof(VkAccelerationStructureInstanceKHR),
                0, (void**)&mapped);

    for (uint32_t i = 0; i < instanceCount; i++) {
        const auto& inst = m_instances[i];
        const auto& blas = m_blasEntries[inst.blasHandle];

        VkAccelerationStructureInstanceKHR vkInst{};

        // Convert glm::mat4 (column-major) to VkTransformMatrixKHR (row-major 3x4)
        glm::mat4 t = glm::transpose(inst.transform);
        memcpy(&vkInst.transform, &t, sizeof(VkTransformMatrixKHR));

        vkInst.instanceCustomIndex = inst.customIndex;
        vkInst.mask = inst.mask;
        vkInst.instanceShaderBindingTableRecordOffset = inst.sbtOffset;
        vkInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vkInst.accelerationStructureReference = blas.deviceAddress;

        mapped[i] = vkInst;
    }

    vkUnmapMemory(m_device, m_instanceMemory);

    // Build TLAS
    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.data.deviceAddress = getBufferDeviceAddress(m_instanceBuffer);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &buildInfo, &instanceCount, &sizeInfo);

    // Force full rebuild when BLAS references change (e.g., animated BLAS replaced)
    bool needsRebuild = (m_tlas == VK_NULL_HANDLE) || m_forceTlasRebuild;
    m_forceTlasRebuild = false;
    if (needsRebuild) {
        // Destroy old
        if (m_tlas != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(m_device, m_tlas, nullptr);
            m_tlas = VK_NULL_HANDLE;
        }
        destroyBuffer(m_tlasBuffer, m_tlasMemory);

        // Create new
        createBuffer(sizeInfo.accelerationStructureSize,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     m_tlasBuffer, m_tlasMemory);

        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = m_tlasBuffer;
        createInfo.size = sizeInfo.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &m_tlas);
    }

    // Ensure scratch
    ensureScratchBuffer(sizeInfo.buildScratchSize);

    // Build
    buildInfo.mode = needsRebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                                   : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    buildInfo.dstAccelerationStructure = m_tlas;
    if (!needsRebuild) buildInfo.srcAccelerationStructure = m_tlas;
    buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(m_scratchBuffer);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    bool ownCmd = (cmd == VK_NULL_HANDLE);
    if (ownCmd) cmd = beginSingleTimeCommands();

    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    // Memory barrier — TLAS must be ready before any ray trace dispatch
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    if (ownCmd) endSingleTimeCommands(cmd);
}

// === Cleanup ===

void RTAccelerationStructure::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Destroy TLAS
    if (m_tlas != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR) {
        vkDestroyAccelerationStructureKHR(m_device, m_tlas, nullptr);
        m_tlas = VK_NULL_HANDLE;
    }
    destroyBuffer(m_tlasBuffer, m_tlasMemory);
    destroyBuffer(m_instanceBuffer, m_instanceMemory);
    m_instanceBufferCapacity = 0;

    // Destroy all BLASes
    for (auto& entry : m_blasEntries) {
        if (entry.handle != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR) {
            vkDestroyAccelerationStructureKHR(m_device, entry.handle, nullptr);
        }
        destroyBuffer(entry.buffer, entry.memory);
    }
    m_blasEntries.clear();

    // Destroy scratch
    destroyBuffer(m_scratchBuffer, m_scratchMemory);
    m_scratchSize = 0;

    m_instances.clear();
    m_supported = false;
}

} // namespace ohao
