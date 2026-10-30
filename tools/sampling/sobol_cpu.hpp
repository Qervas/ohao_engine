#pragma once

// shaders/sampling/sobol.slang on the CPU (tools/sampling/sobol_kernel.slang, compiled to C++ by
// slangc): what tests/renderer/sobol_test calls to check the path tracer's sampler.

#include <array>
#include <cstdint>

namespace ohao::sampling {

// The 4-D point of group `group` that the sampler of sample `index` and pixel seed `seed` draws.
std::array<float, 4> sobolGroup(uint32_t index, uint32_t seed, uint32_t group);

} // namespace ohao::sampling
