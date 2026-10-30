// Sobol direction numbers for dimensions 0-3, from Joe-Kuo new-joe-kuo-6.21201 by the
// Bratley-Fox recurrence (tests/renderer/sobol_test.cpp holds them to it). Dimension 0 is the van
// der Corput sequence; dimensions 1-3 are Joe-Kuo's d = 2, 3, 4 (their s, a and m below).
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

    // Dimension 1: Joe-Kuo d=2, s=1, a=0, m={1}
    0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u,
    0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u,
    0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u,
    0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u,
    0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u,
    0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u,
    0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u,
    0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu,

    // Dimension 2: Joe-Kuo d=3, s=2, a=1, m={1, 3}
    0x80000000u, 0xC0000000u, 0x60000000u, 0x90000000u,
    0xE8000000u, 0x5C000000u, 0x8E000000u, 0xC5000000u,
    0x68800000u, 0x9CC00000u, 0xEE600000u, 0x55900000u,
    0x80680000u, 0xC09C0000u, 0x60EE0000u, 0x90550000u,
    0xE8808000u, 0x5CC0C000u, 0x8E606000u, 0xC5909000u,
    0x6868E800u, 0x9C9C5C00u, 0xEEEE8E00u, 0x5555C500u,
    0x8000E880u, 0xC0005CC0u, 0x60008E60u, 0x9000C590u,
    0xE8006868u, 0x5C009C9Cu, 0x8E00EEEEu, 0xC5005555u,

    // Dimension 3: Joe-Kuo d=4, s=3, a=1, m={1, 3, 1}
    0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u,
    0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u,
    0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u,
    0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u,
    0x208F8000u, 0x51474000u, 0xFBEA2000u, 0x75D93000u,
    0xA0858800u, 0x914E5400u, 0xDBE79E00u, 0x25DB6D00u,
    0x58800080u, 0xE54000C0u, 0x79E00020u, 0xB6D00050u,
    0x800800F8u, 0xC00C0074u, 0x200200A2u, 0x50050093u,
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
