#pragma once
#include <array>
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Sub-plan 4.G: per-frame inputs for NrdCinematicPost::dispatch*().
///
/// The cinematic post chain reads NRD's composed HDR (binding 29) and the
/// 3-level bloom mip chain owned by PathTracer, and writes the final RGBA8
/// LDR output at binding 30. All image views / samplers are borrowed.
///
/// Layout constraints (caller must enforce barriers):
///   composedHDR        — VK_IMAGE_LAYOUT_GENERAL    (storage read)
///   tonemappedOut      — VK_IMAGE_LAYOUT_GENERAL    (storage write)
///   depthAOV           — VK_IMAGE_LAYOUT_GENERAL    (storage read)
///   envMapView         — VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (sampled)
///   bloomMip0/1/2 views— VK_IMAGE_LAYOUT_GENERAL    (storage write for the
///                                                    bloom passes); but for
///                                                    composite they must be
///                                                    in SHADER_READ_ONLY for
///                                                    the sampled descriptor.
struct NrdCinematicInputs {
    // ── HDR + LDR + depth + env ──────────────────────────────────────
    VkImageView composedHDR     = VK_NULL_HANDLE;  // binding 29
    VkImageView tonemappedOut   = VK_NULL_HANDLE;  // binding 30
    VkImageView depthAOV        = VK_NULL_HANDLE;  // binding 20
    VkImageView envMapView      = VK_NULL_HANDLE;
    VkSampler   envMapSampler   = VK_NULL_HANDLE;

    // ── Bloom mip chain ──────────────────────────────────────────────
    // For dispatchBloomExtract, only bloomMip0View matters (as output).
    // For dispatchBloomBlur, callers pass src + dst views explicitly via the
    // per-call API (see dispatchBloomBlur below).
    // For dispatchComposite, all 3 views must be in SHADER_READ_ONLY_OPTIMAL
    // and the bloom sampler is shared.
    VkImageView bloomMip0View   = VK_NULL_HANDLE;
    VkImageView bloomMip1View   = VK_NULL_HANDLE;
    VkImageView bloomMip2View   = VK_NULL_HANDLE;
    VkSampler   bloomSampler    = VK_NULL_HANDLE;

    // ── Camera + framebuffer ─────────────────────────────────────────
    std::array<float, 16> invView   {};
    std::array<float, 16> invProj   {};
    std::array<float, 2>  extent    {};   // {W, H}

    // ── Cinematic params (4.G spec §3.7) ─────────────────────────────
    float envIntensity     = 1.0f;
    float exposure         = 1.0f;
    float bloomStrength    = 0.6f;
    float vignetteStrength = 0.4f;
    float saturation       = 1.1f;
    float contrast         = 1.08f;
    std::array<float, 3> tint {1.02f, 1.0f, 0.98f};
};

/// Sub-plan 4.G: cinematic post-process — replaces NrdTonemap with a full
/// bloom + AgX filmic + vignette + color-grade chain.
///
/// Manages 3 compute pipelines internally:
///   1. bloom_extract — HDR (full-res) → bloom mip 0 (half-res, soft-threshold)
///   2. bloom_blur    — generic 2x downsample + blur, called per mip level
///   3. composite     — combines HDR + bloom + env sky + grade → RGBA8 LDR
///
/// Initialization is unconditional; OHAO_NRD=OFF still allows constructing the
/// PIMPL but initialize() returns false and dispatch*() are no-ops.
class NrdCinematicPost {
public:
    NrdCinematicPost();
    ~NrdCinematicPost();

    NrdCinematicPost(const NrdCinematicPost&)            = delete;
    NrdCinematicPost& operator=(const NrdCinematicPost&) = delete;

    /// Allocates Vulkan resources for all 3 compute pipelines (descriptor
    /// layouts, pipeline layouts, pipelines, descriptor pool/sets). Stores
    /// the source HDR extent and the half-res bloom extent. Pre-allocates
    /// per-blur descriptor sets so dispatchBloomBlur can rotate src/dst
    /// without allocating mid-frame.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    void shutdown();

    /// Dispatch the bloom extract pass.
    ///   composedHDR — full-res HDR (GENERAL)
    ///   bloomMip0   — half-res RGBA16F output (GENERAL)
    void dispatchBloomExtract(VkCommandBuffer cmd,
                              VkImageView composedHDR,
                              VkImageView bloomMip0View,
                              uint32_t srcW, uint32_t srcH,
                              uint32_t dstW, uint32_t dstH);

    /// Dispatch one downsample+blur pass: src mip → dst mip.
    /// `slot` selects which pre-allocated descriptor set to reuse (0 = mip0→mip1,
    /// 1 = mip1→mip2). Callers must keep `slot` stable across frames.
    void dispatchBloomBlur(VkCommandBuffer cmd,
                           uint32_t slot,
                           VkImageView srcMipView, VkImageView dstMipView,
                           uint32_t srcW, uint32_t srcH,
                           uint32_t dstW, uint32_t dstH);

    /// Dispatch the final composite pass — reads HDR + 3 bloom mips (sampled)
    /// + depth + env → writes binding 30. Preconditions on `inputs`:
    ///   composedHDR + tonemappedOut + depthAOV in VK_IMAGE_LAYOUT_GENERAL
    ///   bloomMipN views + envMapView in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    void dispatchComposite(VkCommandBuffer cmd, const NrdCinematicInputs& inputs);

    // PIMPL is exposed (public) so internal-TU helpers in nrd_cinematic.cpp
    // can take an Impl& argument without violating C++ access rules. The
    // struct is intentionally only defined in the .cpp file, so callers
    // can declare pointers to Impl but not poke at its members.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
