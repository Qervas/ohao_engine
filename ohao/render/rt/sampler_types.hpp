#pragma once

// SamplerType — selects the sampling strategy baked into the RT pipeline.
//
// Chosen at pipeline creation time via a Vulkan specialization constant.
// Every SamplerType value must have a matching GLSL branch in
// shaders/includes/rt/sampler_api.glsl — add both or neither.

#include <cstdint>

namespace ohao {

enum class SamplerType : uint32_t {
    PCG   = 0,   // Legacy pseudo-random; realtime default.
    Sobol = 1,   // Owen-scrambled Sobol (Cycles-class); offline default.
};

// Vulkan specialization constant ID used by all RT pipelines to bind
// the chosen SamplerType into the raygen shader.
inline constexpr uint32_t kSamplerSpecConstantId = 0;

} // namespace ohao
