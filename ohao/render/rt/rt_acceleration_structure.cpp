#include "rt_acceleration_structure.hpp"
#include "core/log.hpp"
#include <cstring>
#include <mutex>
#include <algorithm>

namespace ohao {

RTAccelerationStructure::~RTAccelerationStructure() {
    destroy();
}

bool RTAccelerationStructure::init(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;

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

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR pipelineProps{};
    pipelineProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    pipelineProps.pNext = &asProps;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &pipelineProps;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    m_scratchAlignment = std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 256);

    // ONCE, not per init. These are properties of the DEVICE, not of this
    // acceleration structure, so the second printing says nothing the first
    // did not -- and a caller that builds many structures says it many times.
    // A test harness that built several hundred had these three lines as 94%
    // of its entire output, which is 94% of a regression diff carrying no
    // information.
    static std::once_flag rtPropertiesReported;
    std::call_once(rtPropertiesReported, [&pipelineProps]() {
        OHAO_LOG(Info, render, "RT: ray tracing supported!");
        OHAO_LOG(Debug, render, "RT:   maxRayRecursionDepth: {}", pipelineProps.maxRayRecursionDepth);
        OHAO_LOG(Debug, render, "RT:   shaderGroupHandleSize: {}", pipelineProps.shaderGroupHandleSize);
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

GpuBuffer RTAccelerationStructure::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    return m_allocator->createBuffer(size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, AllocationUsage::GpuOnly);
}

GpuBuffer RTAccelerationStructure::createScratch(VkDeviceSize size) {
    // The buffer's own address need not be aligned for a build: one alignment more.
    return createBuffer(size + m_scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

VkDeviceAddress RTAccelerationStructure::scratchAddress(const GpuBuffer& scratch) {
    return (getBufferDeviceAddress(scratch.buffer) + m_scratchAlignment - 1) / m_scratchAlignment * m_scratchAlignment;
}

VkDeviceAddress RTAccelerationStructure::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer;
    return vkGetBufferDeviceAddressFn(m_device, &addressInfo);
}

// === BLAS ===

BlasHandle RTAccelerationStructure::createBLAS(const render::Frame& frame, GpuBuffer& vertices, uint32_t vertexCount,
                                               VkDeviceSize vertexStride, GpuBuffer& indices, uint32_t indexCount,
                                               VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset,
                                               uint32_t firstVertex, bool updatable, RetiredTlas& scratch) {
    if (!m_supported || frame.cmd == VK_NULL_HANDLE) return INVALID_BLAS;

    // Geometry: point to BASE of combined buffers (not sub-range).
    // Indices are global (pre-offset to reference correct vertices).
    // Index byte offset applied via primitiveOffset in rangeInfo.
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = getBufferDeviceAddress(vertices.buffer);  // base, no offset
    triangles.vertexStride = vertexStride;
    triangles.maxVertex = vertexCount + static_cast<uint32_t>(vertexByteOffset / vertexStride) - 1;  // max global index
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = getBufferDeviceAddress(indices.buffer);  // base, no offset

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;  // anyHit multiplies panes' transmission once each
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
    entry.bytes = sizeInfo.accelerationStructureSize;

    entry.buffer = createBuffer(sizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    if (!entry.buffer.isValid()) {
        OHAO_LOG(Error, render, "RT: failed to create BLAS buffer");
        return INVALID_BLAS;
    }

    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = entry.buffer.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    if (vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &entry.handle) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "RT: failed to create BLAS");
        m_allocator->destroyBuffer(entry.buffer);
        return INVALID_BLAS;
    }

    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = entry.handle;
    entry.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(m_device, &addressInfo);

    scratch.buffer = createScratch(sizeInfo.buildScratchSize);
    if (!scratch.buffer.isValid()) {
        OHAO_LOG(Error, render, "RT: failed to create a BLAS build's scratch buffer");
        vkDestroyAccelerationStructureKHR(m_device, entry.handle, nullptr);
        m_allocator->destroyBuffer(entry.buffer);
        return INVALID_BLAS;
    }

    // Build
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = entry.handle;
    buildInfo.scratchData.deviceAddress = scratchAddress(scratch.buffer);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = primitiveCount;
    rangeInfo.primitiveOffset = static_cast<uint32_t>(indexByteOffset);  // byte offset into index buffer
    rangeInfo.firstVertex = firstVertex;  // 0 when indices are already global
    rangeInfo.transformOffset = 0;

    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    {
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.read(vertices, render::BufferUse::AccelInput).read(indices, render::BufferUse::AccelInput);
        pass.write(entry.buffer, render::BufferUse::Accel).write(scratch.buffer, render::BufferUse::Accel);
        pass.begin();
        vkCmdBuildAccelerationStructuresKHR(frame.cmd, 1, &buildInfo, &pRangeInfo);
    }

    // Store and return handle
    BlasHandle handle = static_cast<BlasHandle>(m_blasEntries.size());
    m_blasEntries.push_back(entry);

    OHAO_LOG(Debug, render, "RT: BLAS {} created: {} verts, {} tris",
             handle, vertexCount, primitiveCount);

    return handle;
}

void RTAccelerationStructure::retireBLAS(BlasHandle handle) {
    if (handle >= m_blasEntries.size()) return;
    BlasEntry& entry = m_blasEntries[handle];
    if (!entry.retired && entry.handle != VK_NULL_HANDLE) ++m_retiredAlive;
    entry.retired = true;
}

void RTAccelerationStructure::retiredBlasAddresses(std::vector<VkDeviceAddress>& out) const {
    out.clear();
    if (m_retiredAlive == 0) return;
    for (const BlasEntry& e : m_blasEntries)
        if (e.retired && e.handle != VK_NULL_HANDLE) out.push_back(e.deviceAddress);
    std::sort(out.begin(), out.end());
}

void RTAccelerationStructure::destroyBLAS(BlasHandle handle) {
    if (handle >= m_blasEntries.size()) return;
    auto& entry = m_blasEntries[handle];
    if (entry.handle != VK_NULL_HANDLE) {
        if (entry.retired) --m_retiredAlive;
        vkDestroyAccelerationStructureKHR(m_device, entry.handle, nullptr);
        entry.handle = VK_NULL_HANDLE;
    }
    m_allocator->destroyBuffer(entry.buffer);
    entry.buffer = GpuBuffer{};
    entry.deviceAddress = 0;
}

uint32_t RTAccelerationStructure::recordBlasUpdates(const render::Frame& frame, std::span<BlasUpdate> updates,
                                                   GpuBuffer& vertexBuffer, GpuBuffer& indexBuffer) {
    const VkCommandBuffer cmd = frame.cmd;
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
        if (m_refitScratch.buffer != VK_NULL_HANDLE)   // frames in flight may still use it
            m_retired.push_back(RetiredTlas{VK_NULL_HANDLE, m_refitScratch});
        const VkDeviceSize size = std::max(total, m_refitScratchSize * 2);
        m_refitScratch = createScratch(size);
        if (!m_refitScratch.isValid()) {
            m_refitScratch = GpuBuffer{};
            m_refitScratchSize = 0;
            OHAO_LOG(Error, render, "RT: BLAS refit scratch allocation failed ({} bytes)", size);
            return 0;
        }
        m_refitScratchSize = size;
    }
    const VkDeviceAddress scratchBase = scratchAddress(m_refitScratch);
    const VkDeviceAddress vertices = getBufferDeviceAddress(vertexBuffer.buffer);
    const VkDeviceAddress indices = getBufferDeviceAddress(indexBuffer.buffer);

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
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;  // anyHit multiplies panes' transmission once each
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

    // The BLASes in one call share nothing: each its own structure and its own slice of the scratch.
    render::Pass pass(frame, nullptr, render::Pass::Transfer);
    pass.read(vertexBuffer, render::BufferUse::AccelInput).read(indexBuffer, render::BufferUse::AccelInput);
    for (const BlasUpdate& u : updates)
        if (u.recorded) pass.write(m_blasEntries[u.handle].buffer, render::BufferUse::Accel);
    pass.write(m_refitScratch, render::BufferUse::Accel);
    pass.begin();
    vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(m_batchInfos.size()), m_batchInfos.data(),
                                        m_batchRangePtrs.data());
    return static_cast<uint32_t>(m_batchInfos.size());
}

// === TLAS from device-resident instances (recorded in the frame, never submitted) ===

bool RTAccelerationStructure::buildTLASFromDevice(const render::Frame& frame, GpuBuffer& instances, uint32_t count,
                                                  bool rebuild) {
    const VkCommandBuffer cmd = frame.cmd;
    if (!m_supported || cmd == VK_NULL_HANDLE || instances.buffer == VK_NULL_HANDLE || count == 0) return false;

    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = getBufferDeviceAddress(instances.buffer);

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
        const VkDeviceSize size = std::max({sizeInfo.buildScratchSize, sizeInfo.updateScratchSize, m_tlasScratchSize * 2});
        GpuBuffer scratch = createScratch(size);
        if (!scratch.isValid()) {
            OHAO_LOG(Error, render, "RT: TLAS scratch allocation failed ({} bytes)", size);
            return false;
        }
        // An earlier frame's build may still be using the old scratch.
        if (m_tlasScratch.buffer != VK_NULL_HANDLE) m_retired.push_back(RetiredTlas{VK_NULL_HANDLE, m_tlasScratch});
        m_tlasScratch = scratch;
        m_tlasScratchSize = size;
    }

    // New storage when the scene outgrew the old one. It is published (and the old
    // TLAS retired) only after the build into it is recorded.
    VkAccelerationStructureKHR dst = m_tlas;
    GpuBuffer newBuffer;
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

        newBuffer = createBuffer(newStorage, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        if (!newBuffer.isValid()) {
            OHAO_LOG(Error, render, "RT: TLAS storage allocation failed ({} bytes)", newStorage);
            return false;
        }
        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = newBuffer.buffer;
        createInfo.size = newStorage;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        dst = VK_NULL_HANDLE;
        if (vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr, &dst) != VK_SUCCESS) {
            m_allocator->destroyBuffer(newBuffer);
            OHAO_LOG(Error, render, "RT: TLAS creation failed");
            return false;
        }
    }

    buildInfo.mode = build ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    buildInfo.srcAccelerationStructure = build ? VK_NULL_HANDLE : m_tlas;
    buildInfo.dstAccelerationStructure = dst;
    buildInfo.scratchData.deviceAddress = scratchAddress(m_tlasScratch);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    {
        // The instances and the BLASes they name in; the TLAS (rewritten in place unless it grew) and the scratch out.
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.read(instances, render::BufferUse::AccelInput);
        for (BlasEntry& entry : m_blasEntries)
            if (entry.handle != VK_NULL_HANDLE && !entry.retired) pass.read(entry.buffer, render::BufferUse::Accel);
        pass.write(grow ? newBuffer : m_tlasBuffer, render::BufferUse::Accel);
        pass.write(m_tlasScratch, render::BufferUse::Accel);
        pass.begin();
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    }
    m_tlasBuiltCount = count;

    if (grow) {
        // Earlier frames may still trace the old TLAS: the caller destroys it later.
        if (m_tlas != VK_NULL_HANDLE || m_tlasBuffer.buffer != VK_NULL_HANDLE)
            m_retired.push_back(RetiredTlas{m_tlas, m_tlasBuffer});
        m_tlas = dst;
        m_tlasBuffer = newBuffer;
        m_tlasStorageSize = newStorage;
    }
    return true;
}

void RTAccelerationStructure::traced(render::Pass& pass) {
    if (m_tlas == VK_NULL_HANDLE) return;
    pass.read(m_tlasBuffer, render::BufferUse::Accel);
    for (BlasEntry& entry : m_blasEntries)
        if (entry.handle != VK_NULL_HANDLE && !entry.retired) pass.read(entry.buffer, render::BufferUse::Accel);
}

std::vector<RetiredTlas> RTAccelerationStructure::takeRetired() {
    std::vector<RetiredTlas> out;
    out.swap(m_retired);
    return out;
}

void RTAccelerationStructure::destroyRetired(RetiredTlas& r) {
    if (m_device == VK_NULL_HANDLE) return;
    if (r.as != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR)
        vkDestroyAccelerationStructureKHR(m_device, r.as, nullptr);
    r.as = VK_NULL_HANDLE;
    m_allocator->destroyBuffer(r.buffer);
    r.buffer = GpuBuffer{};
}

// === Cleanup ===

void RTAccelerationStructure::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    if (m_tlas != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR) {
        vkDestroyAccelerationStructureKHR(m_device, m_tlas, nullptr);
        m_tlas = VK_NULL_HANDLE;
    }
    for (GpuBuffer* b : {&m_tlasBuffer, &m_tlasScratch, &m_refitScratch}) {
        m_allocator->destroyBuffer(*b);
        *b = GpuBuffer{};
    }
    m_tlasStorageSize = 0;
    m_tlasBuiltCount = 0;
    m_tlasScratchSize = 0;
    m_refitScratchSize = 0;
    for (RetiredTlas& r : m_retired) destroyRetired(r);
    m_retired.clear();

    for (auto& entry : m_blasEntries) {
        if (entry.handle != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR) {
            vkDestroyAccelerationStructureKHR(m_device, entry.handle, nullptr);
        }
        m_allocator->destroyBuffer(entry.buffer);
    }
    m_blasEntries.clear();
    m_retiredAlive = 0;

    m_supported = false;
    m_device = VK_NULL_HANDLE;
}

} // namespace ohao
