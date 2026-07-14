#pragma once

// DenoiseMode — selects the denoiser backend used by the path tracer.
// Trait queries live in render/rt/rt_meta.hpp (denoiseNeedsMotionVectors, …).

#include "core/concepts.hpp"

#include <cstdint>
#include <string_view>

namespace ohao {

enum class DenoiseMode : uint32_t {
    None   = 0,
    OIDN   = 1,
    NRD    = 3,   // NVIDIA RayTracingDenoiser (Sub-plan 4)
    Atrous = 4,   // À-trous / SVGF-style
    DLSSRR = 5,   // DLSS Ray Reconstruction / NGX "dlssd"
};

[[nodiscard]] DenoiseMode parseDenoiseMode(std::string_view s);
[[nodiscard]] const char* denoiseModeName(DenoiseMode mode);

[[nodiscard]] constexpr int denoiseModeIndex(DenoiseMode mode) noexcept {
    return static_cast<int>(to_underlying(mode));
}

[[nodiscard]] constexpr bool isValidDenoiseMode(DenoiseMode mode) noexcept {
    switch (mode) {
        case DenoiseMode::None:
        case DenoiseMode::OIDN:
        case DenoiseMode::NRD:
        case DenoiseMode::Atrous:
        case DenoiseMode::DLSSRR:
            return true;
    }
    return false;
}

} // namespace ohao
