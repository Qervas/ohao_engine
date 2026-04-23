#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <array>
#include <vector>

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
    /// Sub-plan 4.C T3b: also creates an NRI-wrapped device + nrd::Integration
    /// helper so denoise() can record real REBLUR_DIFFUSE_SPECULAR dispatches.
    ///
    /// `deviceExtensions` / `instanceExtensions` must be the same lists passed
    /// to vkCreateInstance / vkCreateDevice — NRI validates them against what
    /// it uses internally (memory, sync2, etc).
    ///
    /// Returns false on NRD-side failure (logs error).
    bool initialize(VkInstance                        instance,
                    VkDevice                          device,
                    VkPhysicalDevice                  physicalDevice,
                    uint32_t                          graphicsQueueFamilyIndex,
                    const std::vector<const char*>&   instanceExtensions,
                    const std::vector<const char*>&   deviceExtensions,
                    uint32_t                          width,
                    uint32_t                          height);

    /// Destroy the NRD instance. Safe to call multiple times; safe to call
    /// without a prior successful initialize.
    void shutdown();

    /// Per-frame: push camera state into NRD's CommonSettings. Returns true on success.
    bool setCommonSettings(const NrdCameraInputs& inputs);

    /// Per-frame: record the Vulkan image views NRD will consume/produce during 4.C dispatch.
    void setInputImages(const NrdInputImages& images);

    /// Sub-plan 4.C T3b: Record NRD REBLUR_DIFFUSE_SPECULAR compute dispatches onto cmd.
    /// Preconditions:
    ///   - initialize() succeeded
    ///   - setCommonSettings() called this frame
    ///   - setInputImages() called this frame with all IN_* + OUT_* VkImage handles + VkFormats
    ///   - IN_* images in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (or any layout NRI accepts)
    ///   - OUT_* images in VK_IMAGE_LAYOUT_GENERAL (SHADER_RESOURCE_STORAGE)
    /// NRI/NRD will transition internally; after this call resources are in the
    /// "unique" final states recorded in the ResourceSnapshot (restoreInitialState=false).
    /// Returns false on failure (logs error).
    bool denoise(VkCommandBuffer cmd);

    /// Sub-plan 4.C T3b: setInputImages must carry raw VkImage + VkFormat for
    /// NRD's VK path (which wraps them as nri::Texture internally). The
    /// existing NrdInputImages struct stashes VkImageView which we don't
    /// forward to NRD — setInputResources below is what denoise() reads.
    struct NrdInputResource {
        VkImage  image  = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    struct NrdInputResources {
        NrdInputResource motionVector;           // IN_MV
        NrdInputResource viewZ;                  // IN_VIEWZ
        NrdInputResource normalRoughness;        // IN_NORMAL_ROUGHNESS
        NrdInputResource diffRadianceHitDist;    // IN_DIFF_RADIANCE_HITDIST
        NrdInputResource specRadianceHitDist;    // IN_SPEC_RADIANCE_HITDIST
        NrdInputResource outDiffRadianceHitDist; // OUT_DIFF_RADIANCE_HITDIST
        NrdInputResource outSpecRadianceHitDist; // OUT_SPEC_RADIANCE_HITDIST
    };
    void setInputResources(const NrdInputResources& resources);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
