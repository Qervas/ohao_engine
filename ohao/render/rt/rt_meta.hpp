#pragma once

// The path tracer's denoise policy (what each denoiser asks of the trace, from the settings in
// rt_settings.hpp).

#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/render_technique.hpp"
#include "render/rt/rt_settings.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace ohao {

// ─── What each denoiser asks of the trace ───────────────────────────────────

[[nodiscard]] constexpr bool denoiseNeedsCpuReadback(DenoiseMode m) noexcept { return m == DenoiseMode::OIDN; }

[[nodiscard]] constexpr bool denoiseNeedsMotionVectors(DenoiseMode m) noexcept {
    return m == DenoiseMode::NRD || m == DenoiseMode::DLSSRR;
}

[[nodiscard]] constexpr bool denoiseNeedsDiffSpecSplit(DenoiseMode m) noexcept { return m == DenoiseMode::NRD; }

[[nodiscard]] constexpr bool denoiseIsRealtimeCapable(DenoiseMode m) noexcept { return m != DenoiseMode::OIDN; }

/// Halton sub-pixel jitter (NRD's and DLSS RR's temporal reprojection).
[[nodiscard]] constexpr bool denoiseNeedsPixelJitter(DenoiseMode m) noexcept {
    return m == DenoiseMode::NRD || m == DenoiseMode::DLSSRR;
}

/// Whether the trace keeps its own running mean of the beauty. A temporal denoiser carries the
/// history instead (through camera moves, by the motion vectors) and takes this frame's sample
/// alone; offline NRD denoises the mean of a still view's samples.
[[nodiscard]] constexpr bool tracerAccumulates(const RTRenderSettings& s) noexcept {
    switch (s.denoiseMode) {
        case DenoiseMode::DLSSRR: return false;
        case DenoiseMode::NRD: return !isRealtimeProfile(s.profile);
        default: return true;
    }
}

/// Whether NRD's inputs (bindings 22-26) are the mean of the frames accumulated so far.
[[nodiscard]] constexpr bool tracerAccumulatesAOVs(const RTRenderSettings& s) noexcept {
    return s.denoiseMode == DenoiseMode::NRD && !isRealtimeProfile(s.profile);
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

} // namespace ohao
