#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Sub-plan 4.E T1: per-frame inputs for NrdTonemap::dispatch.
/// Both views are borrowed; compositor does not take ownership.
struct NrdTonemapInputs {
    VkImageView composedHDR   = VK_NULL_HANDLE;  // from PT binding 29 (4.D compose output, RGBA32F)
    VkImageView tonemappedOut = VK_NULL_HANDLE;  // PT binding 30 (NEW 4.E, RGBA8 UNORM)
};

/// Sub-plan 4.E T1: compute pipeline that applies ACES tonemap + sRGB gamma to
/// NRD's composed HDR output. Sibling to NrdCompositor (4.D) — same 3-method
/// PIMPL shape. Standalone compute pipeline; its descriptor set layout is
/// independent of PathTracer's RT descriptor layout.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, the implementation
/// compiles to no-op stubs and initialize() returns false.
class NrdTonemap {
public:
    NrdTonemap();
    ~NrdTonemap();

    NrdTonemap(const NrdTonemap&)            = delete;
    NrdTonemap& operator=(const NrdTonemap&) = delete;

    /// Load SPV, create descriptor layout (2 storage-image bindings), pipeline,
    /// descriptor pool, and allocate one persistent descriptor set. Stores w/h.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    /// Destroy all Vulkan objects. Safe to call multiple times.
    void shutdown();

    /// Record a tonemap dispatch onto `cmd`. Preconditions:
    ///   - initialize() succeeded
    ///   - Both image views valid
    ///   - composedHDR in VK_IMAGE_LAYOUT_GENERAL (storage-image access)
    ///   - tonemappedOut in VK_IMAGE_LAYOUT_GENERAL
    /// Writes ACES-tonemapped sRGB to tonemappedOut.
    void dispatch(VkCommandBuffer cmd, const NrdTonemapInputs& inputs);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
