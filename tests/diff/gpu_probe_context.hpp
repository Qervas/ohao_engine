#pragma once

#include "diff/grad/gradient_arena.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

namespace ohao::diff {

/// Headless Vulkan context for standalone GPU probe executables.
///
/// Owns a VkInstance, VkDevice (with the differentiable-renderer device
/// extensions enabled unconditionally -- this binary is meant to fail loudly
/// on hardware that cannot run the subsystem, not degrade gracefully), a
/// GpuAllocator, a command pool, and one compute-capable queue.
class GpuProbeContext {
public:
    [[nodiscard]] bool init();
    void shutdown();

    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return m_physicalDevice; }
    [[nodiscard]] GpuAllocator& allocator() noexcept { return m_allocator; }

    /// Allocates a one-time-submit primary command buffer, records `fn`,
    /// submits it on the compute queue, and waits for completion.
    void runImmediate(const std::function<void(VkCommandBuffer)>& fn);

    /// Loads shaders/diff/atomic_probe.comp's SPIR-V, binds `arena.buffer()`
    /// at binding 0, pushes {targetIndex, invocations}, and dispatches
    /// ceil(invocations / 64) groups. Returns false on any Vulkan error.
    [[nodiscard]] bool runAtomicProbe(GradientArena& arena, uint32_t targetIndex,
                                      uint32_t invocations);

private:
    VkInstance m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    uint32_t m_queueFamily{0};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    GpuAllocator m_allocator;
};

}  // namespace ohao::diff
