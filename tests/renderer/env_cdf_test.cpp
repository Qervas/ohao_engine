#include <gtest/gtest.h>
#include "render/rt/env_cdf.hpp"
#include <vector>
#include <cmath>

using ohao::EnvCDF;

// A fully uniform HDR → sampling should approximate uniform sphere distribution.
TEST(EnvCDF, UniformEnvProducesValidCDF) {
    const int W = 32, H = 16;
    std::vector<float> pixels(W * H * 4, 1.0f);  // all white, alpha=1

    EnvCDF cdf;
    cdf.build(pixels.data(), W, H);

    EXPECT_EQ(cdf.width(), W);
    EXPECT_EQ(cdf.height(), H);
    EXPECT_GT(cdf.integral(), 0.0f);

    // Marginal CDF is monotonic in [0, 1]
    const auto& marg = cdf.marginalCDF();
    ASSERT_EQ(marg.size(), static_cast<size_t>(H));
    EXPECT_NEAR(marg.back(), 1.0f, 1e-4f);
    for (size_t i = 1; i < marg.size(); i++) {
        EXPECT_GE(marg[i], marg[i-1]);
    }

    // Each row's conditional CDF is monotonic in [0, 1]
    const auto& cond = cdf.conditionalCDF();
    ASSERT_EQ(cond.size(), static_cast<size_t>(W * H));
    for (int y = 0; y < H; y++) {
        EXPECT_NEAR(cond[y * W + (W - 1)], 1.0f, 1e-4f);
        for (int x = 1; x < W; x++) {
            EXPECT_GE(cond[y * W + x], cond[y * W + x - 1]);
        }
    }
}

// A hot spot in one row should produce a conditional CDF that jumps at that column,
// and a marginal CDF that jumps at that row.
TEST(EnvCDF, HotSpotConcentratesCDF) {
    const int W = 16, H = 8;
    std::vector<float> pixels(W * H * 4, 0.01f);  // dim background
    const int hotY = 4, hotX = 10;
    int idx = (hotY * W + hotX) * 4;
    pixels[idx + 0] = 100.0f;
    pixels[idx + 1] = 100.0f;
    pixels[idx + 2] = 100.0f;

    EnvCDF cdf;
    cdf.build(pixels.data(), W, H);

    // Row CDF: large step at hotX
    float stepCol = cdf.conditionalCDF()[hotY * W + hotX]
                  - cdf.conditionalCDF()[hotY * W + hotX - 1];
    EXPECT_GT(stepCol, 0.5f);

    // Marginal CDF: large step at hotY
    float stepRow = cdf.marginalCDF()[hotY] - cdf.marginalCDF()[hotY - 1];
    EXPECT_GT(stepRow, 0.5f);
}
