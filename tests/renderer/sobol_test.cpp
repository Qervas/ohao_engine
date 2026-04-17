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
