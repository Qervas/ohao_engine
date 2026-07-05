#ifndef OHAO_SAMPLER_SOBOL_GLSL
#define OHAO_SAMPLER_SOBOL_GLSL

#include "includes/rt/sampler_sobol_tables.glsl"

// Per-pixel state. GLSL ray-tracing shaders run one invocation per ray,
// so these file-scope uints live in registers — no sharing issues.
uint _sobol_index;
uint _sobol_pixelSeed;

// Burley 2020 hash-based Owen scramble.
// Mirrors ohao/render/rt/owen_scramble.cpp byte-for-byte.
uint _sobol_owenScramble(uint v, uint seed) {
    // Reverse bits
    v = (v << 16) | (v >> 16);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);

    v ^= v * 0x3d20adeau;
    v += seed;
    v *= (seed >> 16) | 1u;
    v ^= v * 0x05526c56u;
    v ^= v * 0x53a22864u;

    // Reverse bits back
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    v = (v << 16) | (v >> 16);
    return v;
}

// Direct-bit-expansion Sobol for (index, dim in 0..3).
// Mirrors ohao/render/rt/sobol_generator.cpp::sobolIntForIndex.
uint _sobol_int(uint index, uint dim) {
    uint result = 0u;
    uint baseOffset = dim * 32u;
    for (uint bit = 0u; index != 0u; bit++, index >>= 1u) {
        if ((index & 1u) != 0u) {
            result ^= OHAO_SOBOL_DIRS[baseOffset + bit];
        }
    }
    return result;
}

// Pixel-hash seed for per-pixel decorrelation.
uint _sobol_hashPixel(uvec2 pixel) {
    uint h = pixel.x * 0x1b873593u ^ pixel.y * 0xcc9e2d51u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

void samplerInit_sobol(uvec2 pixel, uint sampleIdx) {
    _sobol_index = sampleIdx;
    _sobol_pixelSeed = _sobol_hashPixel(pixel);
}

// Padded-4D Sobol: dim >> 2 selects the pad, dim & 3 selects the
// Sobol dimension within the pad. Each pad has an independent Owen
// scramble seed so consecutive pads decorrelate.
float getSample1D_sobol(uint dim) {
    uint pad = dim >> 2;
    uint local = dim & 3u;
    uint seed = _sobol_pixelSeed ^ (pad * 0x9e3779b9u);
    uint sobolVal = _sobol_int(_sobol_index, local);
    uint scrambled = _sobol_owenScramble(sobolVal, seed);
    // Match the CPU 24-bit right-shift approach — guarantees [0, 1).
    return float(scrambled >> 8) * (1.0 / 16777216.0);
}

vec2 getSample2D_sobol(uint dim) {
    float x = getSample1D_sobol(dim);
    float y = getSample1D_sobol(dim + 1u);
    return vec2(x, y);
}

#endif
