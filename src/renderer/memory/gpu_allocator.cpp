// Define VMA implementation in this translation unit
#define VMA_IMPLEMENTATION
#include "gpu_allocator.hpp"

#include <iostream>
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

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorInfo.instance = instance;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;

    // Enable buffer device address if available (for future raytracing support)
    // allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create VMA allocator: " << result << std::endl;
        return false;
    }

    std::cout << "VMA allocator initialized successfully" << std::endl;
    return true;
}

void GpuAllocator::shutdown() {
    if (m_allocator == VK_NULL_HANDLE) {
        return;
    }

    // Print final stats before shutdown
    if (m_stats.currentUsage > 0) {
        std::cerr << "Warning: GPU memory leak detected - "
                  << m_stats.currentUsage << " bytes still allocated" << std::endl;
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

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = bufferUsage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

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

    if (vkResult != VK_SUCCESS) {
        std::cerr << "Failed to create VMA buffer: " << vkResult << std::endl;
        return result;
    }

    // Update stats
    m_stats.totalAllocated += size;
    m_stats.currentUsage += size;
    m_stats.allocationCount++;
    if (m_stats.currentUsage > m_stats.peakUsage) {
        m_stats.peakUsage = m_stats.currentUsage;
    }

    return result;
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
        std::cerr << "Failed to create VMA image: " << vkResult << std::endl;
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
        std::cerr << "Failed to map VMA buffer: " << result << std::endl;
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
    std::cout << "=== GPU Allocator Stats ===" << std::endl;
    std::cout << "Total allocated: " << (m_stats.totalAllocated / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "Total freed: " << (m_stats.totalFreed / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "Current usage: " << (m_stats.currentUsage / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "Peak usage: " << (m_stats.peakUsage / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "Allocations: " << m_stats.allocationCount << std::endl;
    std::cout << "Frees: " << m_stats.freeCount << std::endl;
    std::cout << "===========================" << std::endl;
}

VmaMemoryUsage GpuAllocator::toVmaUsage(AllocationUsage usage) const {
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
