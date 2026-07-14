#pragma once

#include "core/concepts.hpp"
#include "gpu/vulkan/vk_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace ohao {

struct AllocationStats {
    std::size_t totalAllocated{0};
    std::size_t totalFreed{0};
    std::size_t currentUsage{0};
    std::size_t peakUsage{0};
    std::uint32_t allocationCount{0};
    std::uint32_t freeCount{0};

    [[nodiscard]] constexpr std::size_t liveBytes() const noexcept { return currentUsage; }
    [[nodiscard]] constexpr bool hasLeaks() const noexcept { return currentUsage > 0; }
};

enum class AllocationUsage {
    GpuOnly,   // GPU-only, fastest for GPU access
    CpuToGpu,  // CPU-visible staging / UBO
    GpuToCpu,  // readback
    CpuOnly,   // rarely-changing host data
};

struct GpuAllocation {
    VmaAllocation allocation{VK_NULL_HANDLE};
    VmaAllocationInfo info{};

    [[nodiscard]] bool isValid() const noexcept { return allocation != VK_NULL_HANDLE; }
    [[nodiscard]] void* getMappedData() const noexcept { return info.pMappedData; }
    [[nodiscard]] VkDeviceSize getSize() const noexcept { return info.size; }
    [[nodiscard]] VkDeviceSize getOffset() const noexcept { return info.offset; }

    template<GpuPod T>
    [[nodiscard]] std::span<T> asSpan(std::size_t count) const noexcept {
        return as_mapped_span<T>(info.pMappedData, count);
    }
};

struct GpuBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    GpuAllocation allocation;

    [[nodiscard]] bool isValid() const noexcept {
        return buffer != VK_NULL_HANDLE && allocation.isValid();
    }
    [[nodiscard]] void* getMappedData() const noexcept { return allocation.getMappedData(); }
    [[nodiscard]] VkDeviceSize sizeBytes() const noexcept { return allocation.getSize(); }

    template<GpuPod T>
    [[nodiscard]] std::span<T> mappedAs(std::size_t count) const noexcept {
        return allocation.asSpan<T>(count);
    }
};

struct GpuImage {
    VkImage image{VK_NULL_HANDLE};
    GpuAllocation allocation;

    [[nodiscard]] bool isValid() const noexcept {
        return image != VK_NULL_HANDLE && allocation.isValid();
    }
};

/**
 * GPU Memory Allocator using VMA.
 */
class GpuAllocator {
public:
    GpuAllocator() = default;
    ~GpuAllocator();

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;
    GpuAllocator(GpuAllocator&&) noexcept;
    GpuAllocator& operator=(GpuAllocator&&) noexcept;

    [[nodiscard]] bool initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_allocator != VK_NULL_HANDLE; }

    [[nodiscard]] GpuBuffer createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags bufferUsage,
        AllocationUsage memoryUsage,
        bool persistentlyMapped = false
    );

    /// Create a host-visible buffer and copy `bytes` into it (staging convenience).
    [[nodiscard]] GpuBuffer createBufferFromBytes(
        std::span<const std::byte> bytes,
        VkBufferUsageFlags bufferUsage,
        AllocationUsage memoryUsage = AllocationUsage::CpuToGpu
    );

    template<GpuPod T>
    [[nodiscard]] GpuBuffer createBufferFromSpan(
        std::span<const T> data,
        VkBufferUsageFlags bufferUsage,
        AllocationUsage memoryUsage = AllocationUsage::CpuToGpu
    ) {
        return createBufferFromBytes(as_bytes(data), bufferUsage, memoryUsage);
    }

    void destroyBuffer(GpuBuffer& buffer);

    [[nodiscard]] GpuImage createImage(const VkImageCreateInfo& imageInfo, AllocationUsage memoryUsage);
    void destroyImage(GpuImage& image);

    [[nodiscard]] void* mapBuffer(GpuBuffer& buffer);
    void unmapBuffer(GpuBuffer& buffer);

    void flushBuffer(GpuBuffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
    void invalidateBuffer(GpuBuffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

    [[nodiscard]] const AllocationStats& getStats() const noexcept { return m_stats; }
    void printStats() const;

    [[nodiscard]] VmaAllocator getVmaAllocator() const noexcept { return m_allocator; }

private:
    [[nodiscard]] VmaMemoryUsage toVmaUsage(AllocationUsage usage) const noexcept;

    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    AllocationStats m_stats{};
};

} // namespace ohao
