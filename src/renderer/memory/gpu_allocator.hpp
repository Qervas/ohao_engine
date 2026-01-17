#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ohao {

/**
 * GPU memory allocation tracking for debugging and profiling
 */
struct AllocationStats {
    size_t totalAllocated{0};
    size_t totalFreed{0};
    size_t currentUsage{0};
    size_t peakUsage{0};
    uint32_t allocationCount{0};
    uint32_t freeCount{0};
};

/**
 * Allocation usage hints for memory type selection
 */
enum class AllocationUsage {
    // GPU-only memory, fastest for GPU access
    GpuOnly,

    // CPU-visible, mappable for uniform buffers and staging
    CpuToGpu,

    // GPU-to-CPU readback (staging buffers for pixel readback)
    GpuToCpu,

    // CPU-only, for data that rarely changes
    CpuOnly
};

/**
 * Wrapper around a VMA allocation
 */
struct GpuAllocation {
    VmaAllocation allocation{VK_NULL_HANDLE};
    VmaAllocationInfo info{};

    bool isValid() const { return allocation != VK_NULL_HANDLE; }
    void* getMappedData() const { return info.pMappedData; }
    VkDeviceSize getSize() const { return info.size; }
    VkDeviceSize getOffset() const { return info.offset; }
};

/**
 * Buffer with its allocation info
 */
struct GpuBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    GpuAllocation allocation;

    bool isValid() const { return buffer != VK_NULL_HANDLE; }
    void* getMappedData() const { return allocation.getMappedData(); }
};

/**
 * Image with its allocation info
 */
struct GpuImage {
    VkImage image{VK_NULL_HANDLE};
    GpuAllocation allocation;

    bool isValid() const { return image != VK_NULL_HANDLE; }
};

/**
 * GPU Memory Allocator using VulkanMemoryAllocator (VMA)
 *
 * Provides:
 * - Efficient memory pooling and suballocation
 * - Automatic memory type selection
 * - Memory defragmentation (optional)
 * - Allocation tracking for debugging
 *
 * Usage:
 *   GpuAllocator allocator;
 *   allocator.initialize(instance, physicalDevice, device);
 *
 *   GpuBuffer buffer = allocator.createBuffer(size, usage, AllocationUsage::CpuToGpu);
 *   void* data = buffer.getMappedData();
 *   // ... use buffer ...
 *   allocator.destroyBuffer(buffer);
 *
 *   allocator.shutdown();
 */
class GpuAllocator {
public:
    GpuAllocator() = default;
    ~GpuAllocator();

    // Non-copyable
    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    // Move allowed
    GpuAllocator(GpuAllocator&&) noexcept;
    GpuAllocator& operator=(GpuAllocator&&) noexcept;

    /**
     * Initialize the allocator
     *
     * @param instance Vulkan instance
     * @param physicalDevice Physical device
     * @param device Logical device
     * @return true on success
     */
    bool initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);

    /**
     * Shutdown the allocator and free all memory
     */
    void shutdown();

    /**
     * Check if the allocator is initialized
     */
    bool isInitialized() const { return m_allocator != VK_NULL_HANDLE; }

    /**
     * Create a buffer with automatic memory allocation
     *
     * @param size Buffer size in bytes
     * @param bufferUsage Vulkan buffer usage flags
     * @param memoryUsage Memory usage hint for allocation
     * @param persistentlyMapped If true, keep the buffer mapped
     * @return GpuBuffer with buffer handle and allocation info
     */
    GpuBuffer createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags bufferUsage,
        AllocationUsage memoryUsage,
        bool persistentlyMapped = false
    );

    /**
     * Destroy a buffer and free its memory
     */
    void destroyBuffer(GpuBuffer& buffer);

    /**
     * Create an image with automatic memory allocation
     *
     * @param imageInfo Vulkan image create info
     * @param memoryUsage Memory usage hint for allocation
     * @return GpuImage with image handle and allocation info
     */
    GpuImage createImage(
        const VkImageCreateInfo& imageInfo,
        AllocationUsage memoryUsage
    );

    /**
     * Destroy an image and free its memory
     */
    void destroyImage(GpuImage& image);

    /**
     * Map a buffer for CPU access
     *
     * @param buffer Buffer to map
     * @return Pointer to mapped memory, or nullptr on failure
     */
    void* mapBuffer(GpuBuffer& buffer);

    /**
     * Unmap a previously mapped buffer
     */
    void unmapBuffer(GpuBuffer& buffer);

    /**
     * Flush mapped memory range to make writes visible to GPU
     *
     * @param buffer Buffer to flush
     * @param offset Offset in bytes from buffer start
     * @param size Size in bytes to flush (VK_WHOLE_SIZE for entire buffer)
     */
    void flushBuffer(GpuBuffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

    /**
     * Invalidate mapped memory range to make GPU writes visible to CPU
     *
     * @param buffer Buffer to invalidate
     * @param offset Offset in bytes from buffer start
     * @param size Size in bytes to invalidate (VK_WHOLE_SIZE for entire buffer)
     */
    void invalidateBuffer(GpuBuffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

    /**
     * Get allocation statistics
     */
    const AllocationStats& getStats() const { return m_stats; }

    /**
     * Print allocation statistics to console
     */
    void printStats() const;

    /**
     * Get the VMA allocator handle (for advanced usage)
     */
    VmaAllocator getVmaAllocator() const { return m_allocator; }

private:
    VmaMemoryUsage toVmaUsage(AllocationUsage usage) const;

    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    AllocationStats m_stats{};
};

} // namespace ohao
