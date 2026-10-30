#pragma once

// Owen scramble — apply a per-seed nested-uniform-scramble permutation to
// an unscrambled Sobol value.
//
// Implementation follows Burley 2020 "Practical Hash-based Owen Scrambling",
// which composes several rounds of bit-level XOR/shift/multiply. Cheap on
// GPU, deterministic, produces per-pixel decorrelation.
//
// rt/path_tracer.slang's sampler (frontier F1 T2) mirrors
// this exactly; keeping the two implementations in lock-step lets the CPU
// unit tests validate the GPU algorithm.

#include <cstdint>

namespace ohao {

uint32_t owenScramble(uint32_t v, uint32_t seed);

} // namespace ohao
