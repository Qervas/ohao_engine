#pragma once

// The path tracer's two profiles and their default settings. Both run rt/path_tracer.slang.

#include "render/rt/denoise/denoise_types.hpp"

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
    bool enableFireflyClamp{false};
    float fireflyClampLuminance{10.0f};
    DenoiseMode denoiseMode{DenoiseMode::None};
};

inline constexpr RTRenderSettings kRealtimeRTSettings{
    .profile = RTRenderProfile::Realtime,
    .maxBounces = 2,
    .preferAccumulation = true,
    .enableAuxiliaryAOVs = true,
    .allowExternalDenoiser = true,
    .enableFireflyClamp = true,
    .fireflyClampLuminance = 10.0f,
    .denoiseMode = DenoiseMode::NRD,   // a moving view is one sample a frame: the denoiser keeps the history
};

inline constexpr RTRenderSettings kOfflineRTSettings{
    .profile = RTRenderProfile::Offline,
    .maxBounces = 32,
    .preferAccumulation = true,
    .enableAuxiliaryAOVs = true,
    .allowExternalDenoiser = true,
    .enableFireflyClamp = false,
    .fireflyClampLuminance = 0.0f,
    .denoiseMode = DenoiseMode::OIDN,
};

[[nodiscard]] constexpr bool isRealtimeProfile(RTRenderProfile p) noexcept {
    return p == RTRenderProfile::Realtime;
}

[[nodiscard]] constexpr bool isOfflineProfile(RTRenderProfile p) noexcept {
    return p == RTRenderProfile::Offline;
}

} // namespace ohao
