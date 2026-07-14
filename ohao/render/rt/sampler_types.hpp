#pragma once

// SamplerType — sampling strategy via Vulkan specialization constant.
// GLSL mirror: shaders/includes/rt/sampler_api.glsl

#include "core/concepts.hpp"

#include <cstdint>

namespace ohao {

enum class SamplerType : uint32_t {
    PCG   = 0,   // pseudo-random; realtime default
    Sobol = 1,   // Owen-scrambled Sobol; offline default
};

inline constexpr uint32_t kSamplerSpecConstantId = 0;

[[nodiscard]] constexpr uint32_t samplerTypeIndex(SamplerType t) noexcept {
    return to_underlying(t);
}

[[nodiscard]] constexpr bool isQmcSampler(SamplerType t) noexcept {
    return t == SamplerType::Sobol;
}

} // namespace ohao
