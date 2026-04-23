#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <array>

namespace ohao {

/// Per-frame camera state NRD needs in its CommonSettings.
/// Matrices are row-major glm::mat4-compatible layout.
struct NrdCameraInputs {
    std::array<float, 16> viewMatrix      {};
    std::array<float, 16> viewMatrixPrev  {};
    std::array<float, 16> projMatrix      {};
    std::array<float, 2>  jitter          {};
    std::array<float, 2>  jitterPrev      {};
    std::array<float, 3>  motionVectorScale {1.0f, 1.0f, 0.0f};
    uint32_t frameIndex = 0;
    bool isMotionVectorInWorldSpace = false;
};

/// Vulkan image views NRD will read/write during dispatch (4.C).
/// 4.B stores them on Impl but does not yet bind via UserPool.
struct NrdInputImages {
    VkImageView viewZ                  = VK_NULL_HANDLE;
    VkImageView motionVector           = VK_NULL_HANDLE;
    VkImageView normalRoughness        = VK_NULL_HANDLE;
    VkImageView diffRadianceHitDist    = VK_NULL_HANDLE;
    VkImageView specRadianceHitDist    = VK_NULL_HANDLE;
    VkImageView diffAlbedo             = VK_NULL_HANDLE;
    VkImageView specColor              = VK_NULL_HANDLE;
    VkImageView outDiffRadianceHitDist = VK_NULL_HANDLE;  // outputs filled by 4.C
    VkImageView outSpecRadianceHitDist = VK_NULL_HANDLE;
};

/// PIMPL wrapper around NVIDIA RayTracingDenoiser (NRD).
///
/// 4.A scope: lifecycle only (initialize / shutdown).
/// 4.B scope: adds per-frame CommonSettings pump + input image view stash.
/// Denoise dispatch added in 4.C+.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, this header
/// compiles but instantiating NrdDenoiser will produce a link error —
/// callers must guard with `#ifdef OHAO_NRD_ENABLED`.
class NrdDenoiser {
public:
    NrdDenoiser();
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&)            = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    /// Create an NRD instance sized for w x h against the given Vulkan device.
    /// Returns false on NRD-side failure (logs error).
    bool initialize(VkDevice         device,
                    VkPhysicalDevice physicalDevice,
                    uint32_t         width,
                    uint32_t         height);

    /// Destroy the NRD instance. Safe to call multiple times; safe to call
    /// without a prior successful initialize.
    void shutdown();

    /// Per-frame: push camera state into NRD's CommonSettings. Returns true on success.
    bool setCommonSettings(const NrdCameraInputs& inputs);

    /// Per-frame: record the Vulkan image views NRD will consume/produce during 4.C dispatch.
    void setInputImages(const NrdInputImages& images);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
