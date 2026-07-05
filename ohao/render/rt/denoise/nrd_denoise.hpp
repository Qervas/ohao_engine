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
    std::array<float, 16> projMatrixPrev  {};   // 4.E T2 — consumes 4.C I5 follow-up
    std::array<float, 2>  jitter          {};
    std::array<float, 2>  jitterPrev      {};
    std::array<float, 3>  motionVectorScale {1.0f, 1.0f, 0.0f};
    uint32_t frameIndex = 0;
    bool isMotionVectorInWorldSpace = false;
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

    /// Sub-plan 4.C T3b: Record NRD REBLUR_DIFFUSE_SPECULAR compute dispatches onto cmd.
    /// Preconditions:
    ///   - initialize() succeeded
    ///   - setCommonSettings() called this frame
    ///   - setInputResources() called this frame with all IN_* + OUT_* VkImage handles + VkFormats
    ///   - Resources in VK_IMAGE_LAYOUT_GENERAL (SHADER_RESOURCE_STORAGE). NRI transitions
    ///     internally using its nri::CmdBarrier machinery; with restoreInitialState=true
    ///     resources are returned to GENERAL after dispatch.
    /// Returns false on failure (logs error).
    bool denoise(VkCommandBuffer cmd);

    /// Sub-plan 4.C T3b: per-frame resource binding. NRD's VK path wraps raw
    /// VkImage+VkFormat as nri::Texture internally — so we hand over image
    /// handles (not views) plus their format. The 7 slots here map directly
    /// to nrd::ResourceType (IN_MV / IN_VIEWZ / IN_NORMAL_ROUGHNESS /
    /// IN_DIFF_RADIANCE_HITDIST / IN_SPEC_RADIANCE_HITDIST /
    /// OUT_DIFF_RADIANCE_HITDIST / OUT_SPEC_RADIANCE_HITDIST) for
    /// REBLUR_DIFFUSE_SPECULAR.
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

    /// Sub-plan 4.F T4: Production-tuned REBLUR parameters.
    ///
    /// Defaults are NVIDIA's reference values for 1-4spp input (vs the stock
    /// NRD defaults which are tuned for 0.25spp game input with SHARC/ReSTIR).
    /// Longer accumulation windows + pre-pass blur = cleaner output at the
    /// expense of a few more frames of lag on disocclusion. Matches what NVIDIA
    /// ships in the RTXDI / RTXNTC sample applications.
    ///
    /// Field names mirror NRD v4.17 `nrd::ReblurSettings` verbatim where a
    /// 1:1 mapping exists. Fields that did NOT exist in v4.17 (e.g. the
    /// `enableMaterialTest` flag from older NRD versions) are omitted.
    struct NrdReblurProfile {
        // hitDistanceParameters.A (units > 0) — constant term of the hit-distance
        // normalization "f = (A + viewZ*B) * lerp(C, 1, smc)". NRD default is 3.0.
        float    hitDistanceParamA           = 3.0f;
        // Prepass spatial-reuse blur radius in pixels. NRD defaults 30 / 50.
        float    diffusePrepassBlurRadius    = 30.0f;
        float    specularPrepassBlurRadius   = 50.0f;
        // Number of reconstructed frames after history reset. NRD default 3.
        uint32_t historyFixFrameNum          = 3;
        // Maximum linearly accumulated frames. NRD default 30; we bump to 63
        // (REBLUR_MAX_HISTORY_FRAME_NUM) since 1-4spp input can safely accumulate
        // longer without over-smoothing.
        uint32_t maxAccumulatedFrameNum      = 63;
        // Fast-history accumulation (reacts faster on disocclusion). NRD default 6.
        uint32_t maxFastAccumulatedFrameNum  = 8;
        // Antilag sigma scale — higher = less aggressive reject on luminance delta.
        // NRD v4.17 field is `antilagSettings.luminanceSigmaScale`, default 2.0.
        float    antilagLuminanceSigmaScale  = 2.0f;
    };

    /// Apply REBLUR settings. Call once after initialize() succeeds.
    /// Returns false if called before initialize() or on OHAO_NRD=OFF build,
    /// or if the underlying `nrd::SetDenoiserSettings` call reports failure.
    bool setReblurSettings(const NrdReblurProfile& profile);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
