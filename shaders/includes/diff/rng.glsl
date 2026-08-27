#ifndef OHAO_DIFF_RNG_GLSL
#define OHAO_DIFF_RNG_GLSL

// GLSL mirror of ohao/diff/rng/diff_rng.cpp. The CPU side is unit-tested;
// this must produce bit-identical values. Change both or neither.
//
// A path is a pure function of (pixel, sample, seed). Forward and backward
// kernels reconstruct the stream from the tuple, never by carrying state.

struct DiffPathRng {
    uint state;
    uint draws;
};

uint diffPcgHash(uint bits) {
    uint state = bits * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

DiffPathRng diffRngForPath(uint pixelIndex, uint sampleIndex, uint iterationSeed) {
    DiffPathRng rng;
    rng.state = diffPcgHash(pixelIndex ^ diffPcgHash(sampleIndex ^ diffPcgHash(iterationSeed)));
    rng.draws = 0u;
    return rng;
}

float diffRngNext1D(inout DiffPathRng rng) {
    rng.state = diffPcgHash(rng.state);
    rng.draws += 1u;
    return float(rng.state >> 8u) * (1.0 / 16777216.0);
}

#endif  // OHAO_DIFF_RNG_GLSL
