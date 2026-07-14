#pragma once

// RT render profile + default settings (shared by PathTracer and rt_meta traits).

#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/sampler_types.hpp"

#include <cstdint>

namespace ohao {

enum class RTRenderProfile {
    Realtime,
    Offline,
};

struct RTRenderSettings {
    RTRenderProfile profile{RTRenderProfile::Offline};
    uint32_t maxBounces{4};
    bool preferAccumulation{true};
    bool enableAuxiliaryAOVs{true};
    bool allowExternalDenoiser{true};
    bool enableInternalDenoise{false};
    bool enableFireflyClamp{false};
    float fireflyClampLuminance{10.0f};
    SamplerType samplerType{SamplerType::Sobol};
    DenoiseMode denoiseMode{DenoiseMode::None};
    // Sub-plan 4.K: global anisotropic specular override
    float anisotropyStrength{0.0f};
    float anisotropyRotation{0.0f};
    // Sub-plan 4.L: subsurface scattering override
    float subsurfaceStrength{0.0f};
    // Realtime/DLSS: genuine per-frame sample count [1, 64]
    uint32_t samplesPerFrame{1};
};

inline constexpr RTRenderSettings kRealtimeRTSettings{
    .profile = RTRenderProfile::Realtime,
    .maxBounces = 2,
    .preferAccumulation = true,
    .enableAuxiliaryAOVs = true,
    .allowExternalDenoiser = false,
    .enableInternalDenoise = true,
    .enableFireflyClamp = true,
    .fireflyClampLuminance = 10.0f,
    .samplerType = SamplerType::PCG,
    .denoiseMode = DenoiseMode::None,
};

inline constexpr RTRenderSettings kOfflineRTSettings{
    .profile = RTRenderProfile::Offline,
    .maxBounces = 4,
    .preferAccumulation = true,
    .enableAuxiliaryAOVs = true,
    .allowExternalDenoiser = true,
    .enableInternalDenoise = false,
    .enableFireflyClamp = false,
    .fireflyClampLuminance = 0.0f,
    .samplerType = SamplerType::Sobol,
    .denoiseMode = DenoiseMode::OIDN,
};

[[nodiscard]] constexpr bool isRealtimeProfile(RTRenderProfile p) noexcept {
    return p == RTRenderProfile::Realtime;
}

[[nodiscard]] constexpr bool isOfflineProfile(RTRenderProfile p) noexcept {
    return p == RTRenderProfile::Offline;
}

[[nodiscard]] constexpr uint32_t clampSamplesPerFrame(uint32_t n) noexcept {
    return n < 1u ? 1u : (n > 64u ? 64u : n);
}

} // namespace ohao
