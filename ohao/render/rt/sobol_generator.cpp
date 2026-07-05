// Sobol direction numbers for dimensions 0-3, from Joe-Kuo
// new-joe-kuo-6.21201. Each dimension has 32 direction numbers (one per
// bit). Dimension 0 (van der Corput) has directions 1<<31, 1<<30, ...,
// 1<<0. Dimension 1 uses the primitive polynomial x+1 with initial
// numbers m_1 = 1. Dimensions 2 and 3 use higher-order polynomials from
// the Joe-Kuo table.
#include "render/rt/sobol_generator.hpp"

namespace ohao {

namespace {

// Direction numbers packed as directions[dim * 32 + bit]
constexpr uint32_t kDirectionNumbers[SobolGenerator::kDimensions * 32] = {
    // Dimension 0: van der Corput (radical-inverse in base 2)
    0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u,
    0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u,
    0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u,
    0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u,
    0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u,
    0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u,
    0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u,
    0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u,

    // Dimension 1: primitive polynomial x+1, initial m_1 = 1
    0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u,
    0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u,
    0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u,
    0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u,
    0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u,
    0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u,
    0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u,
    0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu,

    // Dimension 2: primitive polynomial x^2+x+1, initial m = {1, 3}
    0x80000000u, 0x40000000u, 0xE0000000u, 0x50000000u,
    0x98000000u, 0xBC000000u, 0xAE000000u, 0x25000000u,
    0x5B800000u, 0x91400000u, 0x65600000u, 0x2E500000u,
    0x91180000u, 0x88D40000u, 0xDD360000u, 0x02950000u,
    0x4C888000u, 0x09EB4000u, 0x86CA6000u, 0x13B15000u,
    0x42ADD800u, 0x3DF8D400u, 0x2FE57E00u, 0x56EB8100u,
    0x8C41A680u, 0xE68455C0u, 0x3C9CE8E0u, 0xB44DBFF0u,
    0x7C27F6C8u, 0x5239A6ACu, 0x853B0EDEu, 0x52EB2EF9u,

    // Dimension 3: primitive polynomial x^3+x+1, initial m = {1, 1, 1}
    0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u,
    0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u,
    0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u,
    0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u,
    0x208F8000u, 0x51CBC000u, 0xFBEA2000u, 0x75AD5000u,
    0xA00AF800u, 0x90077400u, 0xD800A200u, 0x25009300u,
    0x59E08480u, 0xE6D0CEC0u, 0x78088520u, 0xB40CEBB0u,
    0x82028348u, 0xC305C274u, 0x208FB552u, 0x51CB9AE9u,
};

// Compute the Sobol value at index i by XOR-accumulating direction numbers
// for each set bit in the binary expansion of i. This is the reference
// (non-Gray-code) formulation that matches published Joe-Kuo test vectors.
static uint32_t sobolIntForIndex(uint32_t index, uint32_t dim) {
    uint32_t result = 0;
    const uint32_t* dirs = kDirectionNumbers + dim * 32;
    for (uint32_t bit = 0; index != 0; bit++, index >>= 1) {
        if (index & 1u) {
            result ^= dirs[bit];
        }
    }
    return result;
}

} // namespace

float SobolGenerator::sample1D(uint32_t index, uint32_t dim) {
    uint32_t v = sobolIntForIndex(index, dim);
    // Use only the top 24 bits so the result is in [0, 1) — a direct float
    // cast of values near 0xFFFFFFFF rounds up to exactly 1.0 in IEEE-754.
    // (v >> 8) is at most 2^24-1, so (v >> 8) * (1/2^24) < 1 exactly.
    return static_cast<float>(v >> 8) * (1.0f / static_cast<float>(1u << 24));
}

const uint32_t* SobolGenerator::directionNumbers() {
    return kDirectionNumbers;
}

} // namespace ohao
