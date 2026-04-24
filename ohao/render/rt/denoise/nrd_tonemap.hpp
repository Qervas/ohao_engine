#pragma once
#include <array>
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Sub-plan 4.E T1: per-frame inputs for NrdTonemap::dispatch.
/// Sub-plan 4.F T1: extended with depth AOV + env map + camera inv matrices so
/// the tonemap pass can composite an HDR env background for miss-ray pixels.
/// All image views / samplers are borrowed; NrdTonemap does not take ownership.
struct NrdTonemapInputs {
    VkImageView composedHDR     = VK_NULL_HANDLE;  // PT binding 29 (composed HDR, RGBA32F)
    VkImageView tonemappedOut   = VK_NULL_HANDLE;  // PT binding 30 (tonemapped, RGBA8 UNORM)
    VkImageView depthAOV        = VK_NULL_HANDLE;  // PT binding 20 (view-space Z, R32F)        — NEW 4.F T1
    VkImageView envMapView      = VK_NULL_HANDLE;  // HDR environment map (equirectangular)     — NEW 4.F T1
    VkSampler   envMapSampler   = VK_NULL_HANDLE;  // linear sampler for env map                 — NEW 4.F T1

    // Per-frame push-constant contents (NEW 4.F T1). Column-major glm layout.
    std::array<float, 16> invView   {};
    std::array<float, 16> invProj   {};
    std::array<float, 2>  extent    {};   // {W, H}
    float envIntensity              = 1.0f;  // 0 when no env loaded (sky samples black)
};

/// Sub-plan 4.E T1 + 4.F T1: compute pipeline that applies ACES tonemap +
/// sRGB gamma to NRD's composed HDR output, with env composite for miss rays.
/// Sibling to NrdCompositor (4.D) — same PIMPL shape. Standalone compute
/// pipeline; its descriptor set layout is independent of PathTracer's RT
/// descriptor layout.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, the implementation
/// compiles to no-op stubs and initialize() returns false.
class NrdTonemap {
public:
    NrdTonemap();
    ~NrdTonemap();

    NrdTonemap(const NrdTonemap&)            = delete;
    NrdTonemap& operator=(const NrdTonemap&) = delete;

    /// Load SPV, create descriptor layout (3 storage-image + 1 combined-image-
    /// sampler bindings + push-constant range), pipeline, descriptor pool, and
    /// allocate one persistent descriptor set. Stores w/h.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    /// Destroy all Vulkan objects. Safe to call multiple times.
    void shutdown();

    /// Record a tonemap dispatch onto `cmd`. Preconditions:
    ///   - initialize() succeeded
    ///   - composedHDR/tonemappedOut/depthAOV views valid
    ///   - envMapView + envMapSampler may be VK_NULL_HANDLE only if envIntensity==0;
    ///     but a valid descriptor must still be bound — caller must pass a fallback
    ///     sampler/view if no env is loaded (see path_tracer_render.cpp for policy)
    ///   - composedHDR + tonemappedOut + depthAOV all in VK_IMAGE_LAYOUT_GENERAL
    ///   - envMapView in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    /// Writes ACES-tonemapped sRGB to tonemappedOut; sky pixels (depth >= 1e20)
    /// sample the env map; surface pixels use composedHDR.
    void dispatch(VkCommandBuffer cmd, const NrdTonemapInputs& inputs);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
