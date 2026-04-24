#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Per-frame inputs for NrdCompositor::dispatch. All views are borrowed;
/// the compositor does not take ownership. Views must be valid + resident
/// until dispatch() returns.
struct NrdComposeInputs {
    VkImageView diffRadiance = VK_NULL_HANDLE;  // from PT binding 27 (NRD denoised diffuse)
    VkImageView specRadiance = VK_NULL_HANDLE;  // from PT binding 28 (NRD denoised specular)
    VkImageView diffAlbedo   = VK_NULL_HANDLE;  // from PT binding 24 (3.C.6 demod albedo)
    VkImageView specColor    = VK_NULL_HANDLE;  // from PT binding 25 (3.C.6 demod F0)
    VkImageView composedOut  = VK_NULL_HANDLE;  // PT binding 29 (4.D output)
};

/// Sub-plan 4.D: compute pipeline that remodulates NRD's denoised radiance
/// with demod albedo+F0 to produce HDR beauty.
///
/// Standalone compute pipeline — its descriptor set layout is independent
/// of PathTracer's RT descriptor layout. The bindings 24/25/27/28/29 that
/// this pipeline consumes live in their own set.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, the implementation
/// compiles to no-op stubs and initialize() returns false.
class NrdCompositor {
public:
    NrdCompositor();
    ~NrdCompositor();

    NrdCompositor(const NrdCompositor&)            = delete;
    NrdCompositor& operator=(const NrdCompositor&) = delete;

    /// Load SPV, create descriptor set layout, pipeline layout, compute
    /// pipeline, descriptor pool, and allocate one persistent descriptor set.
    /// Stores w/h for dispatch-size calculation. Returns false on failure.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    /// Destroy all Vulkan objects. Safe to call multiple times.
    void shutdown();

    /// Record a compose dispatch onto `cmd`. Preconditions:
    ///   - initialize() succeeded
    ///   - All 5 image views in `inputs` are valid
    ///   - Input images (diff/spec radiance, albedo, F0) in SHADER_READ_ONLY_OPTIMAL
    ///     (or GENERAL — layout param below defaults to GENERAL)
    ///   - Output image in GENERAL
    /// Writes composed HDR (RGBA32F) to inputs.composedOut.
    void dispatch(VkCommandBuffer cmd, const NrdComposeInputs& inputs);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
