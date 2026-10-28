#include "rt_acceleration_structure.hpp"
#include "core/log.hpp"
#include <cstring>
#include <mutex>
#include <algorithm>

namespace ohao {

RTAccelerationStructure::~RTAccelerationStructure() {
    destroy();
}

bool RTAccelerationStructure::init(VkDevice device, VkPhysicalDevice physicalDevice,
                                    VkQueue graphicsQueue, uint32_t queueFamily,
                                    VkCommandPool commandPool,
                                   VkPipelineStageFlags asConsumerStages) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_asConsumerStages = asConsumerStages;
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
        OHAO_LOG(Warn, render, "RT: acceleration structure or ray tracing pipeline extension not available");
        m_supported = false;
        return false;
    }

    // Load function pointers
    if (!loadFunctionPointers()) {
        OHAO_LOG(Error, render, "RT: failed to load RT function pointers");
        m_supported = false;
        return false;
    }

    // Query RT pipeline properties
    m_rtPipelineProperties = {};
    m_rtPipelineProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    m_rtPipelineProperties.pNext = &asProps;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &m_rtPipelineProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    m_rtPipelineProperties.pNext = nullptr;
    m_scratchAlignment = std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 256);

    // ONCE, not per init. These are properties of the DEVICE, not of this
    // acceleration structure, so the second printing says nothing the first
    // did not -- and a caller that builds many structures says it many times.
    // A test harness that built several hundred had these three lines as 94%
    // of its entire output, which is 94% of a regression diff carrying no
    // information.
    static std::once_flag rtPropertiesReported;
    std::call_once(rtPropertiesReported, [this]() {
        OHAO_LOG(Info, render, "RT: ray tracing supported!");
        OHAO_LOG(Debug, render, "RT:   maxRayRecursionDepth: {}", m_rtPipelineProperties.maxRayRecursionDepth);
        OHAO_LOG(Debug, render, "RT:   shaderGroupHandleSize: {}", m_rtPipelineProperties.shaderGroupHandleSize);
    });

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
                                                VkCommandBuffer cmd, uint32_t firstVertex, bool updatable) {
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
    buildInfo.flags = updatable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                                      VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                                : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
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
    entry.updatable = updatable;
    entry.vertexStride = vertexStride;
    entry.maxVertex = triangles.maxVertex;
    entry.primitiveOffset = static_cast<uint32_t>(indexByteOffset);
    entry.firstVertex = firstVertex;
    entry.scratchSize = std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize);

    if (!createBuffer(sizeInfo.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      entry.buffer, entry.memory)) {
        OHAO_LOG(Error, render, "RT: failed to create BLAS buffer");
        return INVALID_BLAS;
    }

    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = entry.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    if (vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &entry.handle) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "RT: failed to create BLAS");
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
    rangeInfo.firstVertex = firstVertex;  // 0 when indices are already global
    rangeInfo.transformOffset = 0;

    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    bool ownCmd = (cmd == VK_NULL_HANDLE);
    if (ownCmd) cmd = beginSingleTimeCommands();

    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    if (ownCmd) endSingleTimeCommands(cmd);

    // Store and return handle
    BlasHandle handle = static_cast<BlasHandle>(m_blasEntries.size());
    m_blasEntries.push_back(entry);

    OHAO_LOG(Debug, render, "RT: BLAS {} created: {} verts, {} tris",
             handle, vertexCount, primitiveCount);

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

uint32_t RTAccelerationStructure::recordBlasUpdates(VkCommandBuffer cmd, std::span<BlasUpdate> updates,
                                                   VkBuffer vertexBuffer, VkBuffer indexBuffer) {
    for (BlasUpdate& u : updates) u.recorded = false;
    if (!m_supported || cmd == VK_NULL_HANDLE || updates.empty()) return 0;

    // Each BLAS its own slice of one scratch buffer, so one build call updates them all at once.
    m_batchOffsets.clear();
    VkDeviceSize total = 0;
    for (const BlasUpdate& u : updates) {
        const BlasEntry* entry = u.handle < m_blasEntries.size() ? &m_blasEntries[u.handle] : nullptr;
        if (!entry || !entry->updatable || entry->handle == VK_NULL_HANDLE) {
            m_batchOffsets.push_back(VkDeviceSize(-1));
            continue;
        }
        m_batchOffsets.push_back(total);
        total += (entry->scratchSize + m_scratchAlignment - 1) / m_scratchAlignment * m_scratchAlignment;
    }
    if (total == 0) return 0;
    if (m_refitScratchSize < total) {
        if (m_refitScratchBuffer != VK_NULL_HANDLE)   // frames in flight may still use it
            m_retired.push_back(RetiredTlas{VK_NULL_HANDLE, m_refitScratchBuffer, m_refitScratchMemory});
        m_refitScratchBuffer = VK_NULL_HANDLE;
        m_refitScratchMemory = VK_NULL_HANDLE;
        // The buffer's own address is aligned for the base slice: allocate one alignment more.
        const VkDeviceSize size = std::max(total, m_refitScratchSize * 2) + m_scratchAlignment;
        if (!createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_refitScratchBuffer, m_refitScratchMemory)) {
            m_refitScratchSize = 0;
            OHAO_LOG(Error, render, "RT: BLAS refit scratch allocation failed ({} bytes)", size);
            return 0;
        }
        m_refitScratchSize = size - m_scratchAlignment;
    }
    const VkDeviceAddress scratchBase =
        (getBufferDeviceAddress(m_refitScratchBuffer) + m_scratchAlignment - 1) / m_scratchAlignment * m_scratchAlignment;
    const VkDeviceAddress vertices = getBufferDeviceAddress(vertexBuffer);
    const VkDeviceAddress indices = getBufferDeviceAddress(indexBuffer);

    m_batchGeometries.clear();
    m_batchInfos.clear();
    m_batchRanges.clear();
    m_batchGeometries.reserve(updates.size());
    m_batchRanges.reserve(updates.size());
    for (std::size_t i = 0; i < updates.size(); ++i) {
        if (m_batchOffsets[i] == VkDeviceSize(-1)) continue;
        const BlasEntry& entry = m_blasEntries[updates[i].handle];
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = vertices;
        geometry.geometry.triangles.vertexStride = entry.vertexStride;
        geometry.geometry.triangles.maxVertex = entry.maxVertex;
        geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geometry.geometry.triangles.indexData.deviceAddress = indices;
        m_batchGeometries.push_back(geometry);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = entry.indexCount / 3;
        range.primitiveOffset = entry.primitiveOffset;
        range.firstVertex = entry.firstVertex;
        m_batchRanges.push_back(range);

        const bool rebuild = updates[i].rebuild;
        VkAccelerationStructureBuildGeometryInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                     VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        info.geometryCount = 1;
        info.mode = rebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        info.srcAccelerationStructure = rebuild ? VK_NULL_HANDLE : entry.handle;
        info.dstAccelerationStructure = entry.handle;
        info.scratchData.deviceAddress = scratchBase + m_batchOffsets[i];
        m_batchInfos.push_back(info);
        updates[i].recorded = true;
    }
    // Pointers into the vectors only once they stop growing.
    m_batchRangePtrs.clear();
    for (std::size_t k = 0; k < m_batchInfos.size(); ++k) {
        m_batchInfos[k].pGeometries = &m_batchGeometries[k];
        m_batchRangePtrs.push_back(&m_batchRanges[k]);
    }

    // The scratch and the structures: earlier builds (this frame's or an earlier one) and the
    // previous frames' traces finish with them first. The BLASes in one call share nothing.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    before.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | m_asConsumerStages,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(m_batchInfos.size()), m_batchInfos.data(),
                                        m_batchRangePtrs.data());
    return static_cast<uint32_t>(m_batchInfos.size());
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

    // Memory barrier — TLAS must be ready before anything reads it. The
    // destination stages come from init(): a ray-query consumer runs in
    // COMPUTE_SHADER, which a ray-tracing-pipeline-only mask never ordered.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         m_asConsumerStages,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    if (ownCmd) endSingleTimeCommands(cmd);
}

// === TLAS from device-resident instances (recorded in the frame, never submitted) ===

bool RTAccelerationStructure::buildTLASFromDevice(VkCommandBuffer cmd, VkDeviceAddress instances,
                                                  uint32_t count, bool rebuild) {
    if (!m_supported || cmd == VK_NULL_HANDLE || instances == 0 || count == 0) return false;

    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = instances;

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
                                             &buildInfo, &count, &sizeInfo);

    // An UPDATE must use the primitive count of the build it updates.
    const bool grow = sizeInfo.accelerationStructureSize > m_tlasStorageSize;
    const bool build = grow || rebuild || m_tlas == VK_NULL_HANDLE || count != m_tlasBuiltCount;

    // Scratch first: a failure here must leave the published TLAS untouched.
    const VkDeviceSize scratchNeed = build ? sizeInfo.buildScratchSize : sizeInfo.updateScratchSize;
    if (scratchNeed > m_tlasScratchSize) {
        const VkDeviceSize alignment = 256;
        const VkDeviceSize want = std::max({sizeInfo.buildScratchSize, sizeInfo.updateScratchSize, m_tlasScratchSize * 2});
        const VkDeviceSize size = (want + alignment - 1) & ~(alignment - 1);
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (!createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, memory)) {
            destroyBuffer(buffer, memory);
            OHAO_LOG(Error, render, "RT: TLAS scratch allocation failed ({} bytes)", size);
            return false;
        }
        // An earlier frame's build may still be using the old scratch.
        if (m_tlasScratchBuffer != VK_NULL_HANDLE)
            m_retired.push_back(RetiredTlas{VK_NULL_HANDLE, m_tlasScratchBuffer, m_tlasScratchMemory});
        m_tlasScratchBuffer = buffer;
        m_tlasScratchMemory = memory;
        m_tlasScratchSize = size;
    }

    // New storage when the scene outgrew the old one. It is published (and the old
    // TLAS retired) only after the build into it is recorded.
    VkAccelerationStructureKHR dst = m_tlas;
    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    VkDeviceSize newStorage = 0;
    if (grow) {
        // Size the new storage with headroom so a growing scene does not replace
        // the TLAS (and its handle) on every added instance.
        uint32_t capacity = std::max(count, 64u);
        capacity = capacity > (UINT32_MAX / 2) ? count : capacity + capacity / 2;
        VkAccelerationStructureBuildSizesInfoKHR capInfo{};
        capInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                 &buildInfo, &capacity, &capInfo);
        newStorage = std::max(capInfo.accelerationStructureSize, sizeInfo.accelerationStructureSize);

        if (!createBuffer(newStorage,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, newBuffer, newMemory)) {
            destroyBuffer(newBuffer, newMemory);
            OHAO_LOG(Error, render, "RT: TLAS storage allocation failed ({} bytes)", newStorage);
            return false;
        }
        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = newBuffer;
        createInfo.size = newStorage;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        dst = VK_NULL_HANDLE;
        if (vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &dst) != VK_SUCCESS) {
            destroyBuffer(newBuffer, newMemory);
            OHAO_LOG(Error, render, "RT: TLAS creation failed");
            return false;
        }
    }

    // Before: earlier frames' readers of this TLAS (asConsumerStages) and its
    // earlier build (TLAS + scratch writes) finish before it is rewritten in place.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    before.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
                         m_asConsumerStages | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &before, 0, nullptr, 0, nullptr);

    buildInfo.mode = build ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    buildInfo.srcAccelerationStructure = build ? VK_NULL_HANDLE : m_tlas;
    buildInfo.dstAccelerationStructure = dst;
    buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(m_tlasScratchBuffer);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    m_tlasBuiltCount = count;

    if (grow) {
        // Earlier frames may still trace the old TLAS: the caller destroys it later.
        if (m_tlas != VK_NULL_HANDLE || m_tlasBuffer != VK_NULL_HANDLE)
            m_retired.push_back(RetiredTlas{m_tlas, m_tlasBuffer, m_tlasMemory});
        m_tlas = dst;
        m_tlasBuffer = newBuffer;
        m_tlasMemory = newMemory;
        m_tlasStorageSize = newStorage;
    }

    // After: the TLAS is visible to every stage that reads it.
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    after.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, m_asConsumerStages,
                         0, 1, &after, 0, nullptr, 0, nullptr);
    return true;
}

std::vector<RetiredTlas> RTAccelerationStructure::takeRetired() {
    std::vector<RetiredTlas> out;
    out.swap(m_retired);
    return out;
}

void RTAccelerationStructure::destroyRetired(const RetiredTlas& r) {
    if (m_device == VK_NULL_HANDLE) return;
    if (r.as != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR)
        vkDestroyAccelerationStructureKHR(m_device, r.as, nullptr);
    VkBuffer buffer = r.buffer;
    VkDeviceMemory memory = r.memory;
    destroyBuffer(buffer, memory);
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
    m_tlasStorageSize = 0;
    m_tlasBuiltCount = 0;
    destroyBuffer(m_tlasScratchBuffer, m_tlasScratchMemory);
    m_tlasScratchSize = 0;
    for (const RetiredTlas& r : m_retired) destroyRetired(r);   // device idle above
    m_retired.clear();
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
    destroyBuffer(m_refitScratchBuffer, m_refitScratchMemory);
    m_refitScratchSize = 0;

    m_instances.clear();
    m_supported = false;
}

} // namespace ohao
