#include <gtest/gtest.h>
#include "render/rt/sobol_generator.hpp"
#include <cmath>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

using ohao::SobolGenerator;

// The direction numbers are Joe-Kuo's (new-joe-kuo-6.21201): dimension d > 0 is their d + 1, whose
// (s, a, m) the file publishes, expanded by the Bratley-Fox recurrence
// v_k = v_{k-s} ^ (v_{k-s} >> s) ^ sum_i a_i v_{k-i}. (The table once held other numbers for
// dimensions 2 and 3, which tests computed from the table itself could not see.)
TEST(Sobol, DirectionNumbersAreJoeKuos) {
    struct Poly { uint32_t s, a; uint32_t m[3]; };
    const Poly polys[4] = {{0, 0, {}}, {1, 0, {1}}, {2, 1, {1, 3}}, {3, 1, {1, 3, 1}}};
    const uint32_t* dirs = SobolGenerator::directionNumbers();
    for (uint32_t d = 0; d < 4; ++d) {
        uint32_t v[32];
        const Poly& p = polys[d];
        for (uint32_t k = 0; k < 32; ++k) {
            if (d == 0) { v[k] = 1u << (31 - k); continue; }
            if (k < p.s) { v[k] = p.m[k] << (31 - k); continue; }
            uint32_t x = v[k - p.s] ^ (v[k - p.s] >> p.s);
            for (uint32_t i = 1; i < p.s; ++i)
                if ((p.a >> (p.s - 1 - i)) & 1u) x ^= v[k - i];
            v[k] = x;
        }
        for (uint32_t k = 0; k < 32; ++k) EXPECT_EQ(dirs[d * 32 + k], v[k]) << "dim " << d << " bit " << k;
    }
}

// The unscrambled Sobol sequence in dimensions 0 and 1 starts with these points
// (from Joe-Kuo new-joe-kuo-6.21201, verified against a standalone reference).
// Each point is in [0, 1)^2. Format: (x, y).
TEST(Sobol, First8PointsMatchJoeKuoReference) {
    // Joe-Kuo (dim 0, dim 1) for indices 0..7
    const float kRef[8][2] = {
        {0.0f,   0.0f},
        {0.5f,   0.5f},
        {0.25f,  0.75f},
        {0.75f,  0.25f},
        {0.125f, 0.625f},
        {0.625f, 0.125f},
        {0.375f, 0.375f},
        {0.875f, 0.875f},
    };
    for (uint32_t i = 0; i < 8; i++) {
        float x = SobolGenerator::sample1D(i, 0);
        float y = SobolGenerator::sample1D(i, 1);
        EXPECT_NEAR(x, kRef[i][0], 1e-5f) << "dim 0 at idx " << i;
        EXPECT_NEAR(y, kRef[i][1], 1e-5f) << "dim 1 at idx " << i;
    }
}

TEST(Sobol, DimensionsStayInUnitInterval) {
    for (uint32_t d = 0; d < 4; d++) {
        for (uint32_t i = 0; i < 128; i++) {
            float v = SobolGenerator::sample1D(i, d);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
    // Boundary: index 2^31 on dim 1 produces dirs[31] = 0xFFFFFFFF,
    // which would round to 1.0f under naive uint-to-float cast.
    float v = SobolGenerator::sample1D(2147483648u, 1);
    EXPECT_GE(v, 0.0f);
    EXPECT_LT(v, 1.0f);
}

#include "render/rt/owen_scramble.hpp"

using ohao::owenScramble;

// Owen scramble of an unscrambled Sobol value is deterministic:
// same (value, seed) -> same output.
TEST(Owen, Deterministic) {
    const uint32_t v = 0xABCD1234u;
    const uint32_t s = 0xDEADBEEFu;
    uint32_t r1 = owenScramble(v, s);
    uint32_t r2 = owenScramble(v, s);
    EXPECT_EQ(r1, r2);
}

// Different seeds produce different outputs for the same input (decorrelation).
TEST(Owen, DifferentSeedsDecorrelate) {
    const uint32_t v = 0x01234567u;
    uint32_t r1 = owenScramble(v, 0x00000001u);
    uint32_t r2 = owenScramble(v, 0x00000002u);
    EXPECT_NE(r1, r2);
}

// Input 0 with any nonzero seed must still produce a value in [0, 2^32).
TEST(Owen, ZeroInputProducesValidOutput) {
    uint32_t r = owenScramble(0u, 0xCAFEBABEu);
    EXPECT_GE(r, 0u);
    EXPECT_LE(r, 0xFFFFFFFFu);
}

// Mass decorrelation — for 1000 random inputs, two different scrambles
// should disagree in most bits (statistical proxy for Owen's uniform-scramble property).
TEST(Owen, MassDecorrelation) {
    uint32_t bitDiffTotal = 0;
    for (uint32_t i = 1; i <= 1000u; i++) {
        uint32_t v = i * 0x9E3779B9u;  // any deterministic spread
        uint32_t a = owenScramble(v, 0x1u);
        uint32_t b = owenScramble(v, 0x2u);
#if defined(_MSC_VER)
        bitDiffTotal += static_cast<uint32_t>(__popcnt(a ^ b));
#else
        bitDiffTotal += __builtin_popcount(a ^ b);
#endif
    }
    // For a good hash-based scramble, expected bit differences ≈ 16 per sample,
    // i.e. 16000 total for 1000 samples. Allow wide band.
    EXPECT_GT(bitDiffTotal, 12000u);
    EXPECT_LT(bitDiffTotal, 20000u);
}

// ---- The path tracer's sampler (shaders/sampling/sobol.slang, frontier F1 T2) ----------------
// The module runs here through slangc's C++ (tools/sampling): its draws must be Burley's shuffled,
// Owen-scrambled 4-D Sobol built from the generator and the scramble above, bit for bit.

#include "sobol_cpu.hpp"

#include <set>
#include <vector>

namespace {

uint32_t hashCombine(uint32_t seed, uint32_t v) { return seed ^ (v + (seed << 6) + (seed >> 2)); }

uint32_t sobolBits(uint32_t index, uint32_t dim) {
    uint32_t x = 0;
    const uint32_t* dirs = SobolGenerator::directionNumbers() + dim * 32;
    for (uint32_t bit = 0; index != 0; ++bit, index >>= 1)
        if (index & 1u) x ^= dirs[bit];
    return x;
}

// Burley 2020's shuffled_scrambled_sobol4d: the index shuffled, each coordinate scrambled.
std::array<uint32_t, 4> reference(uint32_t index, uint32_t seed) {
    const uint32_t i = owenScramble(index, seed);
    std::array<uint32_t, 4> p{};
    for (uint32_t d = 0; d < 4; ++d) p[d] = owenScramble(sobolBits(i, d), hashCombine(seed, d)) >> 8;
    return p;
}

uint32_t bits24(float u) { return static_cast<uint32_t>(u * 16777216.0f); }

} // namespace

TEST(Sampler, ModuleIsBurleysShuffledScrambledSobol) {
    for (uint32_t pixelSeed : {0x1u, 0x9E3779B9u, 0xDEADBEEFu})
        for (uint32_t group : {0u, 1u, 7u, 97u})
            for (uint32_t index = 0; index < 64; ++index) {
                const auto got = ohao::sampling::sobolGroup(index, pixelSeed, group);
                const auto want = reference(index, hashCombine(pixelSeed, group));
                for (int d = 0; d < 4; ++d) {
                    ASSERT_EQ(bits24(got[d]), want[d]) << "seed " << pixelSeed << " group " << group
                                                       << " index " << index << " dim " << d;
                    ASSERT_GE(got[d], 0.0f);
                    ASSERT_LT(got[d], 1.0f);
                }
            }
}

// Owen scrambling keeps the net's stratification: for every pair of dimensions and every split of
// the first 2^m samples into elementary intervals of area 2^-m, the scrambled points occupy as
// many intervals as the unscrambled sequence's do (all 2^m for dimensions 0 and 1, the (0, 2)
// pair). This is the stratification a pixel's frames get, whatever its seed.
TEST(Sampler, ScramblingKeepsEveryPairsStratification) {
    constexpr uint32_t m = 8, n = 1u << m;
    const auto cellsOf = [&](const std::vector<std::array<uint32_t, 4>>& pts, int a, int b, uint32_t k) {
        std::set<uint32_t> cells;
        for (const auto& p : pts) cells.insert((p[a] >> (24 - k)) << (m - k) | (p[b] >> (24 - (m - k))));
        return cells.size();
    };
    std::vector<std::array<uint32_t, 4>> plain;
    for (uint32_t i = 0; i < n; ++i)
        plain.push_back({sobolBits(i, 0) >> 8, sobolBits(i, 1) >> 8, sobolBits(i, 2) >> 8, sobolBits(i, 3) >> 8});
    for (uint32_t pixelSeed : {0x1u, 0x12345678u})
        for (uint32_t group : {0u, 5u}) {
            std::vector<std::array<uint32_t, 4>> pts;
            for (uint32_t i = 0; i < n; ++i) {
                const auto p = ohao::sampling::sobolGroup(i, pixelSeed, group);
                pts.push_back({bits24(p[0]), bits24(p[1]), bits24(p[2]), bits24(p[3])});
            }
            for (int a = 0; a < 4; ++a)
                for (int b = a + 1; b < 4; ++b)
                    for (uint32_t k = 0; k <= m; ++k)
                        EXPECT_EQ(cellsOf(pts, a, b, k), cellsOf(plain, a, b, k)) << "dims " << a << "," << b << " split " << k;
            for (uint32_t k = 0; k <= m; ++k) EXPECT_EQ(cellsOf(pts, 0, 1, k), n) << "dims 0,1 split " << k;
        }
}

// Groups are decorrelated: the same sample's points of two groups differ.
TEST(Sampler, GroupsAreIndependent) {
    int same = 0;
    for (uint32_t i = 0; i < 256; ++i) {
        const auto p = ohao::sampling::sobolGroup(i, 0x2468ACEu, 1);
        const auto q = ohao::sampling::sobolGroup(i, 0x2468ACEu, 2);
        same += bits24(p[0]) == bits24(q[0]);
    }
    EXPECT_LT(same, 4);
}
