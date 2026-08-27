#pragma once

#include "diff/grad/gradient_arena.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace ohao::diff {

/// Headless Vulkan context for standalone GPU probe executables.
///
/// Owns a VkInstance, VkDevice (with the differentiable-renderer device
/// extensions enabled unconditionally -- this binary is meant to fail loudly
/// on hardware that cannot run the subsystem, not degrade gracefully), a
/// GpuAllocator, a command pool, and one compute-capable queue.
class GpuProbeContext {
public:
    GpuProbeContext() = default;
    // init() can return false after having already created the instance,
    // messenger, device, and/or command pool -- shutdown() is idempotent and
    // safely tears down whatever partial state exists, so running it from
    // the destructor closes that leak even when a caller's `if (!ctx.init())
    // return 1;` never reaches an explicit ctx.shutdown().
    ~GpuProbeContext() { shutdown(); }

    // Not copyable or movable: the compiler-generated versions would copy
    // the raw Vulkan handles, leaving two objects that each believe they own
    // (and will each destroy) the same instance/device/pool.
    GpuProbeContext(const GpuProbeContext&) = delete;
    GpuProbeContext& operator=(const GpuProbeContext&) = delete;
    GpuProbeContext(GpuProbeContext&&) = delete;
    GpuProbeContext& operator=(GpuProbeContext&&) = delete;

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

    /// Runs shaders/diff/rng_probe.comp for one path and returns its first
    /// `drawCount` RNG values. Compared bit-exactly against ohao::diff::PathRng,
    /// this is what proves the GLSL mirror in shaders/includes/diff/rng.glsl
    /// agrees with the CPU reference. Path replay backpropagation replays each
    /// light path from its seed instead of storing a tape, so a single differing
    /// bit means the backward pass walks a different path than the forward pass
    /// and every gradient is silently wrong.
    [[nodiscard]] bool runRngParityProbe(uint32_t pixelIndex, uint32_t sampleIndex,
                                         uint32_t iterationSeed, uint32_t drawCount,
                                         GradientArena& scratch, std::size_t blockIndex,
                                         std::vector<float>& outDraws);

    /// Builds a BLAS/TLAS for a single axis-aligned quad spanning
    /// x in [-1,1], y in [quadMinY,1] at z = -planeDistance, traces one ray
    /// per pixel from the origin looking down -Z, and fills `outHits` with
    /// width*height distances (-1.0 on miss). `quadMinY` defaults to -1.0
    /// (a full-frustum-covering quad, so every ray hits); passing 0.0 makes
    /// only the top half of the quad present, which turns hit/miss into an
    /// orientation-sensitive signal -- see diff_gpu_probe.cpp's half-quad
    /// check for why the plain distance check alone can't catch a flipped
    /// camera convention.
    /// Returns false on any Vulkan error.
    [[nodiscard]] bool runVisibilityProbe(float planeDistance, uint32_t width, uint32_t height,
                                          float tanHalfFov, std::vector<float>& outHits,
                                          float quadMinY = -1.0f);

private:
    /// Shared boilerplate for the single-storage-buffer compute probes:
    /// load SPIR-V, one STORAGE_BUFFER at binding 0, push constants, dispatch,
    /// barrier to host reads, wait. Every object is destroyed on every path.
    [[nodiscard]] bool dispatchStorageBufferCompute(const char* spvName, VkBuffer buffer,
                                                    const void* pushData, uint32_t pushSize,
                                                    uint32_t groupCountX);

    VkInstance m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    uint32_t m_queueFamily{0};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    GpuAllocator m_allocator;

    // Best-effort validation: present only when VK_LAYER_KHRONOS_validation is
    // available on the host. See init()/shutdown() -- absence is a warning,
    // never a failure.
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    bool m_validationEnabled{false};
};

}  // namespace ohao::diff
