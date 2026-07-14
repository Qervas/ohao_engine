#pragma once

/**
 * RT metaprogramming: profile tags, denoise traits, compile-time settings.
 *
 * Compile-time:
 *   auto s = makeProfileSettings<RTRenderProfile::Realtime>();
 *   if constexpr (DenoiseModeTraits<DenoiseMode::NRD>::needs_motion_vectors) { ... }
 *   using F = RTFeatureFlags<RTRenderProfile::Realtime, DenoiseMode::DLSSRR>;
 *
 * Runtime (same numbers, switch → traits):
 *   if (denoiseNeedsMotionVectors(mode)) { ... }
 */

#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/render_technique.hpp"
#include "render/rt/rt_settings.hpp"
#include "render/rt/sampler_types.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace ohao {

// ─── RT profile traits ──────────────────────────────────────────────────────

template<RTRenderProfile Profile>
struct RTProfileTraits;

template<>
struct RTProfileTraits<RTRenderProfile::Realtime> {
    static constexpr RTRenderProfile profile = RTRenderProfile::Realtime;
    static constexpr bool is_realtime = true;
    static constexpr bool is_offline  = false;

    static constexpr uint32_t default_max_bounces     = 2;
    static constexpr bool     prefer_accumulation     = true;
    static constexpr bool     enable_auxiliary_aovs   = true;
    static constexpr bool     allow_external_denoiser = false;
    static constexpr bool     enable_internal_denoise = true;
    static constexpr bool     enable_firefly_clamp    = true;
    static constexpr float    firefly_clamp_luminance = 10.0f;
    static constexpr SamplerType default_sampler      = SamplerType::PCG;
    static constexpr DenoiseMode default_denoise      = DenoiseMode::None;

    [[nodiscard]] static constexpr RTRenderSettings defaults() noexcept {
        return kRealtimeRTSettings;
    }
};

template<>
struct RTProfileTraits<RTRenderProfile::Offline> {
    static constexpr RTRenderProfile profile = RTRenderProfile::Offline;
    static constexpr bool is_realtime = false;
    static constexpr bool is_offline  = true;

    static constexpr uint32_t default_max_bounces     = 4;
    static constexpr bool     prefer_accumulation     = true;
    static constexpr bool     enable_auxiliary_aovs   = true;
    static constexpr bool     allow_external_denoiser = true;
    static constexpr bool     enable_internal_denoise = false;
    static constexpr bool     enable_firefly_clamp    = false;
    static constexpr float    firefly_clamp_luminance = 0.0f;
    static constexpr SamplerType default_sampler      = SamplerType::Sobol;
    static constexpr DenoiseMode default_denoise      = DenoiseMode::OIDN;

    [[nodiscard]] static constexpr RTRenderSettings defaults() noexcept {
        return kOfflineRTSettings;
    }
};

// Lock trait defaults to the designated-init constants (one source of truth).
static_assert(RTProfileTraits<RTRenderProfile::Realtime>::default_max_bounces ==
              kRealtimeRTSettings.maxBounces);
static_assert(RTProfileTraits<RTRenderProfile::Offline>::default_max_bounces ==
              kOfflineRTSettings.maxBounces);
static_assert(RTProfileTraits<RTRenderProfile::Offline>::default_denoise ==
              DenoiseMode::OIDN);

/// Compile-time settings for a known profile (zero runtime branch).
template<RTRenderProfile Profile>
[[nodiscard]] constexpr RTRenderSettings makeProfileSettings() noexcept {
    if constexpr (Profile == RTRenderProfile::Realtime) {
        return RTProfileTraits<RTRenderProfile::Realtime>::defaults();
    } else {
        static_assert(Profile == RTRenderProfile::Offline);
        return RTProfileTraits<RTRenderProfile::Offline>::defaults();
    }
}

/// Runtime profile → settings (CLI / mode switches).
[[nodiscard]] constexpr RTRenderSettings makeProfileSettings(RTRenderProfile profile) noexcept {
    switch (profile) {
        case RTRenderProfile::Realtime: return makeProfileSettings<RTRenderProfile::Realtime>();
        case RTRenderProfile::Offline:  return makeProfileSettings<RTRenderProfile::Offline>();
    }
    return makeProfileSettings<RTRenderProfile::Offline>();
}

// ─── Denoise mode traits ────────────────────────────────────────────────────

template<DenoiseMode Mode>
struct DenoiseModeTraits {
    static constexpr DenoiseMode mode = Mode;

    static constexpr bool needs_cpu_readback =
        Mode == DenoiseMode::OIDN;

    static constexpr bool needs_motion_vectors =
        Mode == DenoiseMode::NRD ||
        Mode == DenoiseMode::Atrous ||
        Mode == DenoiseMode::DLSSRR;

    static constexpr bool needs_diff_spec_split =
        Mode == DenoiseMode::NRD;

    /// Halton sub-pixel jitter (NRD temporal / DLSS-RR).
    static constexpr bool needs_pixel_jitter =
        Mode == DenoiseMode::NRD ||
        Mode == DenoiseMode::DLSSRR;

    /// Multi-sample AOV mean for NRD (kPTFlagAccumulateAOVs).
    static constexpr bool needs_aov_accumulation =
        Mode == DenoiseMode::NRD;

    /// Denoiser owns temporal accumulation — feed fresh 1-spp beauty each frame.
    static constexpr bool wants_fresh_sample =
        Mode == DenoiseMode::Atrous ||
        Mode == DenoiseMode::DLSSRR;

    static constexpr bool is_realtime_capable =
        Mode != DenoiseMode::OIDN;

    static constexpr bool is_offline_capable =
        Mode == DenoiseMode::None ||
        Mode == DenoiseMode::OIDN ||
        Mode == DenoiseMode::NRD;

    static constexpr bool is_gpu_backend =
        Mode == DenoiseMode::NRD ||
        Mode == DenoiseMode::Atrous ||
        Mode == DenoiseMode::DLSSRR;
};

[[nodiscard]] constexpr bool denoiseNeedsCpuReadback(DenoiseMode m) noexcept {
    switch (m) {
        case DenoiseMode::OIDN: return DenoiseModeTraits<DenoiseMode::OIDN>::needs_cpu_readback;
        default:                return false;
    }
}

[[nodiscard]] constexpr bool denoiseNeedsMotionVectors(DenoiseMode m) noexcept {
    switch (m) {
        case DenoiseMode::NRD:    return DenoiseModeTraits<DenoiseMode::NRD>::needs_motion_vectors;
        case DenoiseMode::Atrous: return DenoiseModeTraits<DenoiseMode::Atrous>::needs_motion_vectors;
        case DenoiseMode::DLSSRR: return DenoiseModeTraits<DenoiseMode::DLSSRR>::needs_motion_vectors;
        default:                  return false;
    }
}

[[nodiscard]] constexpr bool denoiseNeedsDiffSpecSplit(DenoiseMode m) noexcept {
    switch (m) {
        case DenoiseMode::NRD: return DenoiseModeTraits<DenoiseMode::NRD>::needs_diff_spec_split;
        default:               return false;
    }
}

[[nodiscard]] constexpr bool denoiseIsRealtimeCapable(DenoiseMode m) noexcept {
    switch (m) {
        case DenoiseMode::None:   return DenoiseModeTraits<DenoiseMode::None>::is_realtime_capable;
        case DenoiseMode::OIDN:   return DenoiseModeTraits<DenoiseMode::OIDN>::is_realtime_capable;
        case DenoiseMode::NRD:    return DenoiseModeTraits<DenoiseMode::NRD>::is_realtime_capable;
        case DenoiseMode::Atrous: return DenoiseModeTraits<DenoiseMode::Atrous>::is_realtime_capable;
        case DenoiseMode::DLSSRR: return DenoiseModeTraits<DenoiseMode::DLSSRR>::is_realtime_capable;
    }
    return false;
}

[[nodiscard]] constexpr bool denoiseNeedsPixelJitter(DenoiseMode m) noexcept {
    switch (m) {
        case DenoiseMode::NRD:    return DenoiseModeTraits<DenoiseMode::NRD>::needs_pixel_jitter;
        case DenoiseMode::DLSSRR: return DenoiseModeTraits<DenoiseMode::DLSSRR>::needs_pixel_jitter;
        default:                  return false;
    }
}

[[nodiscard]] constexpr bool denoiseNeedsAovAccumulation(DenoiseMode m) noexcept {
    return m == DenoiseMode::NRD;
}

[[nodiscard]] constexpr bool denoiseWantsFreshSample(DenoiseMode m) noexcept {
    switch (m) {
        case DenoiseMode::Atrous: return DenoiseModeTraits<DenoiseMode::Atrous>::wants_fresh_sample;
        case DenoiseMode::DLSSRR: return DenoiseModeTraits<DenoiseMode::DLSSRR>::wants_fresh_sample;
        default:                  return false;
    }
}

/// Mutate settings so denoise-mode requirements are satisfied (AOVs, external denoise).
[[nodiscard]] constexpr RTRenderSettings applyDenoisePolicy(RTRenderSettings s) noexcept {
    if (denoiseNeedsMotionVectors(s.denoiseMode) || denoiseNeedsDiffSpecSplit(s.denoiseMode)) {
        s.enableAuxiliaryAOVs = true;
    }
    if (denoiseNeedsCpuReadback(s.denoiseMode)) {
        s.allowExternalDenoiser = true;
    }
    return s;
}

// ─── Technique concepts (document virtual surfaces for static helpers) ──────

template<typename T>
concept ShadowTechniqueLike = requires(T t, VkCommandBuffer cmd, const ShadowInput& in,
                                        VkDevice dev, VkPhysicalDevice phys,
                                        uint32_t w, uint32_t h) {
    { t.getName() } -> std::convertible_to<const char*>;
    { t.needsRT() } -> std::convertible_to<bool>;
    { t.init(dev, phys, w, h) } -> std::convertible_to<bool>;
    { t.resize(w, h) } -> std::same_as<void>;
    { t.render(cmd, in) } -> std::same_as<void>;
    { t.getOutput() } -> std::convertible_to<ShadowOutput>;
    { t.destroy() } -> std::same_as<void>;
};

template<typename T>
concept GITechniqueLike = requires(T t, VkCommandBuffer cmd, const GIInput& in,
                                   VkDevice dev, VkPhysicalDevice phys,
                                   uint32_t w, uint32_t h) {
    { t.getName() } -> std::convertible_to<const char*>;
    { t.needsRT() } -> std::convertible_to<bool>;
    { t.init(dev, phys, w, h) } -> std::convertible_to<bool>;
    { t.resize(w, h) } -> std::same_as<void>;
    { t.render(cmd, in) } -> std::same_as<void>;
    { t.getOutput() } -> std::convertible_to<GIOutput>;
    { t.destroy() } -> std::same_as<void>;
};

template<ShadowTechniqueLike T>
inline constexpr bool kIsShadowTechnique = true;

template<GITechniqueLike T>
inline constexpr bool kIsGITechnique = true;

// ─── Feature flags (profile × denoise, if constexpr friendly) ───────────────

template<RTRenderProfile Profile, DenoiseMode Mode>
struct RTFeatureFlags {
    using ProfileT = RTProfileTraits<Profile>;
    using DenoiseT = DenoiseModeTraits<Mode>;

    static constexpr bool want_auxiliary_aovs =
        ProfileT::enable_auxiliary_aovs ||
        DenoiseT::needs_diff_spec_split ||
        DenoiseT::needs_motion_vectors;

    static constexpr bool want_motion_vectors = DenoiseT::needs_motion_vectors;
    static constexpr bool want_diff_spec      = DenoiseT::needs_diff_spec_split;
    static constexpr bool want_cpu_readback   = DenoiseT::needs_cpu_readback;
    static constexpr bool interactive         = ProfileT::is_realtime;
    static constexpr uint32_t max_bounces     = ProfileT::default_max_bounces;
};

template<RTRenderProfile Profile, DenoiseMode Mode>
[[nodiscard]] constexpr RTRenderSettings makeFeatureSettings() noexcept {
    RTRenderSettings s = makeProfileSettings<Profile>();
    if constexpr (DenoiseModeTraits<Mode>::needs_motion_vectors ||
                  DenoiseModeTraits<Mode>::needs_diff_spec_split) {
        s.enableAuxiliaryAOVs = true;
    }
    if constexpr (DenoiseModeTraits<Mode>::needs_cpu_readback) {
        s.allowExternalDenoiser = true;
    }
    s.denoiseMode = Mode;
    return s;
}

} // namespace ohao
