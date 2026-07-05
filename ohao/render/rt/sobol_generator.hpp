#pragma once

// Sobol generator — CPU reference implementation of the Sobol low-discrepancy
// sequence for dimensions 0 through 3. Values match the Joe-Kuo
// new-joe-kuo-6.21201 direction numbers, direct-bit-expansion formulation.
//
// Used for:
//   - Unit testing the GLSL Sobol sampler (shader mirrors this math exactly)
//   - Generating the constant table committed as
//     shaders/includes/rt/sampler_sobol_tables.glsl
//
// Pure CPU. No Vulkan dependency.

#include <cstdint>

namespace ohao {

class SobolGenerator {
public:
    // Supported dimensions: 0, 1, 2, 3
    static constexpr uint32_t kDimensions = 4;

    // Returns the n-th Sobol sample in the given dimension. Value in [0, 1).
    static float sample1D(uint32_t index, uint32_t dim);

    // Direction-number matrix: 32 uint32s per dimension, kDimensions dimensions.
    // Flat layout: directions[dim * 32 + bit].
    static const uint32_t* directionNumbers();
};

} // namespace ohao
