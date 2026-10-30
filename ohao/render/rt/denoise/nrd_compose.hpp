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
    VkImageView emission     = VK_NULL_HANDLE;  // PT binding 13: the camera ray's emission (RGBA16F)
    VkImageView composedOut  = VK_NULL_HANDLE;  // the composed HDR (RGBA32F)
    VkImageView outputLdr    = VK_NULL_HANDLE;  // the frame's output (RGBA8, the tracer's curve)
};

/// The NRD frame: remodulates NRD's denoised diffuse by its albedo, adds the denoised specular and
/// the camera ray's emission, and writes the HDR and the frame's output (the tracer's curve).
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

    /// The frame-in-flight slot whose fence was just waited: dispatch() writes that
    /// slot's descriptor set (an earlier frame may still read its own).
    void setFrameSlot(uint32_t slot);

    /// Record a compose dispatch onto `cmd`. Preconditions:
    ///   - initialize() succeeded
    ///   - All 7 image views in `inputs` are valid
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
