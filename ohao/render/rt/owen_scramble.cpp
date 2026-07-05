// Burley 2020 hash-based Owen scramble. Reverses the input bits so the
// scramble acts on the most-significant bits (as Owen intended), applies
// a well-mixed hash, then reverses back.
//
// Must remain in sync with the GLSL port in
// shaders/includes/rt/sampler_sobol.glsl.

#include "render/rt/owen_scramble.hpp"

namespace ohao {

uint32_t owenScramble(uint32_t v, uint32_t seed) {
    // Reverse bits (__builtin_bswap32 + nibble/pair swap)
    v = __builtin_bswap32(v);
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
    v = __builtin_bswap32(v);
    return v;
}

} // namespace ohao
