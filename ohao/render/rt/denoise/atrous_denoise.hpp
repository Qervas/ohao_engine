#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Per-dispatch inputs for AtrousDenoiser. All handles are borrowed — the
/// denoiser does not take ownership. They must be valid + resident until
/// dispatch() returns.
///
/// The à-trous filter runs on the RT BEAUTY image in-place: it reads the
/// noisy beauty, edge-aware-filters it against the normal + depth AOVs, and
/// writes the denoised result back into the SAME beauty image. Because it
/// denoises the final (correct) PBR image — not demodulated diffuse/specular
/// like NRD — metals, emissive, and color stay physically correct.
struct AtrousInputs {
    VkImage     beautyImage = VK_NULL_HANDLE;  // m_outputImage (RGBA8) — in AND final out
    VkImageView beautyView  = VK_NULL_HANDLE;  // m_outputImageView
    VkImageView normalView  = VK_NULL_HANDLE;  // m_normalAOVView   (RGBA32F, N*0.5+0.5)
    VkImageView depthView   = VK_NULL_HANDLE;  // m_depthAOVView    (R32F, linear view Z)
};

/// À-trous (SVGF-style) edge-aware spatial denoiser for the RT beauty image.
///
/// Standalone compute pipeline with its own descriptor layout, independent of
/// PathTracer's RT descriptor set. Owns two ping-pong scratch images (same
/// size/format as the beauty) so a 5x5 tap filter can be applied over N
/// iterations with growing step size without read/write aliasing.
///
/// Precondition for dispatch(): beauty/normal/depth already written by raygen
/// and made visible to the COMPUTE stage (RAY_TRACING -> COMPUTE memory
/// barrier), all three in VK_IMAGE_LAYOUT_GENERAL. After dispatch() returns
/// the denoised result is in `beautyImage`, GENERAL layout, last written by
/// the COMPUTE stage.
class AtrousDenoiser {
public:
    AtrousDenoiser();
    ~AtrousDenoiser();

    AtrousDenoiser(const AtrousDenoiser&)            = delete;
    AtrousDenoiser& operator=(const AtrousDenoiser&) = delete;

    /// Load SPV, create descriptor set layout, pipeline layout, compute
    /// pipeline, descriptor pool, N descriptor sets, and the two ping-pong
    /// scratch images. Returns false on failure.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    /// Destroy all Vulkan objects. Safe to call multiple times.
    void shutdown();

    /// Record the full multi-iteration à-trous filter onto `cmd`.
    void dispatch(VkCommandBuffer cmd, const AtrousInputs& inputs);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
