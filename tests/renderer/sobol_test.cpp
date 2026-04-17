#include <gtest/gtest.h>
#include "render/rt/sobol_generator.hpp"
#include <cmath>

using ohao::SobolGenerator;

// Dim 2 reference values, computed from the dim-2 direction numbers
// (0x80000000, 0x40000000, 0xE0000000, 0x50000000, ...) using direct
// bit expansion.
TEST(Sobol, Dim2First8Points) {
    const float kRef[8] = {
        0.0f, 0.5f, 0.25f, 0.75f, 0.875f, 0.375f, 0.625f, 0.125f,
    };
    for (uint32_t i = 0; i < 8; i++) {
        EXPECT_NEAR(SobolGenerator::sample1D(i, 2), kRef[i], 1e-5f)
            << "dim 2 at idx " << i;
    }
}

// Dim 3 reference values.
TEST(Sobol, Dim3First8Points) {
    const float kRef[8] = {
        0.0f, 0.5f, 0.75f, 0.25f, 0.125f, 0.625f, 0.875f, 0.375f,
    };
    for (uint32_t i = 0; i < 8; i++) {
        EXPECT_NEAR(SobolGenerator::sample1D(i, 3), kRef[i], 1e-5f)
            << "dim 3 at idx " << i;
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
        bitDiffTotal += __builtin_popcount(a ^ b);
    }
    // For a good hash-based scramble, expected bit differences ≈ 16 per sample,
    // i.e. 16000 total for 1000 samples. Allow wide band.
    EXPECT_GT(bitDiffTotal, 12000u);
    EXPECT_LT(bitDiffTotal, 20000u);
}
