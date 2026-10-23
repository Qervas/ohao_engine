// Define VMA implementation in this translation unit.
//
// NRI_VK.lib compiles a second VMA implementation from the same checkout (vma-src),
// configured as below. Under /FORCE:MULTIPLE the linker keeps whichever object it pulls
// first, and that depends on CMake's link order for the target: the examples get this
// one, the ohao CLI got NRI's. The two must therefore behave identically. NRI's is
// built with dynamic function loading, which needs vkGet*ProcAddr in pVulkanFunctions;
// without them its vmaCreateAllocator calls through a null pointer. So this copy uses
// NRI's exact configuration and initialize() always passes the two entry points.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "gpu_allocator.hpp"

#include "core/log.hpp"
#include <cstring>

namespace ohao {

GpuAllocator::~GpuAllocator() {
    shutdown();
}

GpuAllocator::GpuAllocator(GpuAllocator&& other) noexcept
    : m_allocator(other.m_allocator)
    , m_device(other.m_device)
    , m_stats(other.m_stats)
{
    other.m_allocator = VK_NULL_HANDLE;
    other.m_device = VK_NULL_HANDLE;
    other.m_stats = {};
}

GpuAllocator& GpuAllocator::operator=(GpuAllocator&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_allocator = other.m_allocator;
        m_device = other.m_device;
        m_stats = other.m_stats;
        other.m_allocator = VK_NULL_HANDLE;
        other.m_device = VK_NULL_HANDLE;
        other.m_stats = {};
    }
    return *this;
}

bool GpuAllocator::initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device) {
    if (m_allocator != VK_NULL_HANDLE) {
        return true;
    }

    m_device = device;

    // VMA create info uses many optional fields; keep explicit assignment for stability
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorInfo.instance = instance;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;

    // VMA loads every other entry point through these (see the top of this file).
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    allocatorInfo.pVulkanFunctions = &functions;

    // Enable buffer device address support. Required so VMA tags memory
    // allocated for VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT buffers with
    // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT (VUID-VkMemoryAllocateInfo-flags-03331).
    // Without this flag VMA silently skips that allocate-flag (the assert
    // guarding it compiles out in Release/NDEBUG builds), and any later
    // vkGetBufferDeviceAddress() call on such a buffer is validation-layer
    // invalid. First load-bearing user: RTAccelerationStructure's BLAS/TLAS
    // build inputs.
    //
    // Caller contract: `device` must have been created with
    // VkPhysicalDeviceVulkan12Features::bufferDeviceAddress = VK_TRUE (or the
    // equivalent VK_KHR_buffer_device_address feature struct) -- this flag
    // makes VMA tag *every* block allocation with
    // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, which is only legal if the
    // device actually enabled the feature. tests/renderer/renderer_pipeline_tests.cpp
    // creates a device without that feature and calls initialize() with it;
    // that file is not referenced by any CMakeLists.txt today so it is inert,
    // but it becomes a live VUID break the moment someone wires it into the
    // build.
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator);
    if (result != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to create VMA allocator: {}", static_cast<int>(result));
        return false;
    }

    OHAO_LOG(Debug, gpu, "VMA allocator initialized successfully");
    return true;
}

void GpuAllocator::shutdown() {
    if (m_allocator == VK_NULL_HANDLE) {
        return;
    }

    // Print final stats before shutdown
    if (m_stats.currentUsage > 0) {
        OHAO_LOG(Warn, gpu, "GPU memory leak detected - {} bytes still allocated",
                 m_stats.currentUsage);
        printStats();
    }

    vmaDestroyAllocator(m_allocator);
    m_allocator = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_stats = {};
}

GpuBuffer GpuAllocator::createBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags bufferUsage,
    AllocationUsage memoryUsage,
    bool persistentlyMapped)
{
    GpuBuffer result{};

    if (m_allocator == VK_NULL_HANDLE || size == 0) {
        return result;
    }

    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = bufferUsage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = toVmaUsage(memoryUsage);

    if (persistentlyMapped) {
        allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    // Prefer dedicated allocation for large buffers
    if (size > 256 * 1024) { // 256KB threshold
        allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }

    VkResult vkResult = vmaCreateBuffer(
        m_allocator,
        &bufferInfo,
        &allocInfo,
        &result.buffer,
        &result.allocation.allocation,
        &result.allocation.info
    );

    if (vk_failed(vkResult)) {
        OHAO_LOG(Error, gpu, "Failed to create VMA buffer: {}", vk_result_name(vkResult));
        return result;
    }

    // Update stats.
    //
    // ACCOUNT THE VMA ALLOCATION SIZE, NOT THE REQUESTED SIZE. VMA rounds a
    // request up to the memory type's alignment, and destroyBuffer() below
    // subtracts `buffer.allocation.getSize()` -- that rounded-up number. This
    // side used to add the caller's `size` instead, so currentUsage lost the
    // per-buffer padding on every create/destroy pair and, being unsigned,
    // WRAPPED: a run that freed everything it allocated ended with
    // currentUsage a little under 2^64 and shutdown() printed
    // "Warning: GPU memory leak detected - 18446744073709536560 bytes still
    // allocated" directly above "Allocations: 290 == Frees: 290". A false
    // leak warning on a green run is how a real one gets ignored. createImage
    // below always used getSize() on both sides and never had the bug; this
    // makes the buffer path match it.
    const VkDeviceSize accountedSize = result.allocation.getSize();
    m_stats.totalAllocated += accountedSize;
    m_stats.currentUsage += accountedSize;
    m_stats.allocationCount++;
    if (m_stats.currentUsage > m_stats.peakUsage) {
        m_stats.peakUsage = m_stats.currentUsage;
    }

    return result;
}

GpuBuffer GpuAllocator::createBufferFromBytes(
    std::span<const std::byte> bytes,
    VkBufferUsageFlags bufferUsage,
    AllocationUsage memoryUsage)
{
    GpuBuffer buf = createBuffer(
        static_cast<VkDeviceSize>(bytes.size()),
        bufferUsage,
        memoryUsage,
        /*persistentlyMapped=*/true);

    if (!buf.isValid() || !buf.getMappedData() || bytes.empty()) {
        return buf;
    }
    std::memcpy(buf.getMappedData(), bytes.data(), bytes.size());
    flushBuffer(buf);
    return buf;
}

void GpuAllocator::destroyBuffer(GpuBuffer& buffer) {
    if (m_allocator == VK_NULL_HANDLE || buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    VkDeviceSize size = buffer.allocation.getSize();

    vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation.allocation);

    // Update stats
    m_stats.totalFreed += size;
    m_stats.currentUsage -= size;
    m_stats.freeCount++;

    buffer.buffer = VK_NULL_HANDLE;
    buffer.allocation = {};
}

GpuImage GpuAllocator::createImage(
    const VkImageCreateInfo& imageInfo,
    AllocationUsage memoryUsage)
{
    GpuImage result{};

    if (m_allocator == VK_NULL_HANDLE) {
        return result;
    }

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = toVmaUsage(memoryUsage);

    // Images should always use dedicated memory for better performance
    allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VkResult vkResult = vmaCreateImage(
        m_allocator,
        &imageInfo,
        &allocInfo,
        &result.image,
        &result.allocation.allocation,
        &result.allocation.info
    );

    if (vkResult != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to create VMA image: {}", static_cast<int>(vkResult));
        return result;
    }

    // Update stats
    VkDeviceSize size = result.allocation.getSize();
    m_stats.totalAllocated += size;
    m_stats.currentUsage += size;
    m_stats.allocationCount++;
    if (m_stats.currentUsage > m_stats.peakUsage) {
        m_stats.peakUsage = m_stats.currentUsage;
    }

    return result;
}

void GpuAllocator::destroyImage(GpuImage& image) {
    if (m_allocator == VK_NULL_HANDLE || image.image == VK_NULL_HANDLE) {
        return;
    }

    VkDeviceSize size = image.allocation.getSize();

    vmaDestroyImage(m_allocator, image.image, image.allocation.allocation);

    // Update stats
    m_stats.totalFreed += size;
    m_stats.currentUsage -= size;
    m_stats.freeCount++;

    image.image = VK_NULL_HANDLE;
    image.allocation = {};
}

void* GpuAllocator::mapBuffer(GpuBuffer& buffer) {
    if (m_allocator == VK_NULL_HANDLE || buffer.buffer == VK_NULL_HANDLE) {
        return nullptr;
    }

    // Check if already persistently mapped
    if (buffer.allocation.info.pMappedData) {
        return buffer.allocation.info.pMappedData;
    }

    void* data = nullptr;
    VkResult result = vmaMapMemory(m_allocator, buffer.allocation.allocation, &data);
    if (result != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to map VMA buffer: {}", static_cast<int>(result));
        return nullptr;
    }

    return data;
}

void GpuAllocator::unmapBuffer(GpuBuffer& buffer) {
    if (m_allocator == VK_NULL_HANDLE || buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    // Don't unmap if persistently mapped
    if (buffer.allocation.info.pMappedData) {
        return;
    }

    vmaUnmapMemory(m_allocator, buffer.allocation.allocation);
}

void GpuAllocator::flushBuffer(GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (m_allocator == VK_NULL_HANDLE || buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    vmaFlushAllocation(m_allocator, buffer.allocation.allocation, offset, size);
}

void GpuAllocator::invalidateBuffer(GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (m_allocator == VK_NULL_HANDLE || buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    vmaInvalidateAllocation(m_allocator, buffer.allocation.allocation, offset, size);
}

void GpuAllocator::printStats() const {
    OHAO_LOG(Debug, gpu, "=== GPU Allocator Stats ===");
    OHAO_LOG(Debug, gpu, "Total allocated: {} MB", m_stats.totalAllocated / 1024.0 / 1024.0);
    OHAO_LOG(Debug, gpu, "Total freed: {} MB", m_stats.totalFreed / 1024.0 / 1024.0);
    OHAO_LOG(Debug, gpu, "Current usage: {} MB", m_stats.currentUsage / 1024.0 / 1024.0);
    OHAO_LOG(Debug, gpu, "Peak usage: {} MB", m_stats.peakUsage / 1024.0 / 1024.0);
    OHAO_LOG(Debug, gpu, "Allocations: {}", m_stats.allocationCount);
    OHAO_LOG(Debug, gpu, "Frees: {}", m_stats.freeCount);
    OHAO_LOG(Debug, gpu, "===========================");
}

VmaMemoryUsage GpuAllocator::toVmaUsage(AllocationUsage usage) const noexcept {
    switch (usage) {
        case AllocationUsage::GpuOnly:
            return VMA_MEMORY_USAGE_GPU_ONLY;
        case AllocationUsage::CpuToGpu:
            return VMA_MEMORY_USAGE_CPU_TO_GPU;
        case AllocationUsage::GpuToCpu:
            return VMA_MEMORY_USAGE_GPU_TO_CPU;
        case AllocationUsage::CpuOnly:
            return VMA_MEMORY_USAGE_CPU_ONLY;
        default:
            return VMA_MEMORY_USAGE_AUTO;
    }
}

} // namespace ohao
