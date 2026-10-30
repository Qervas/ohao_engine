#include <gtest/gtest.h>
#include "render/rt/env_cdf.hpp"
#include <cmath>
#include <vector>

using ohao::EnvCdf;

namespace {
std::vector<float> map(int w, int h, float v) { return std::vector<float>(size_t(w) * h * 4, v); }
}  // namespace

// Every CDF rises to exactly 1, and a texel's probability is its luminance x sin(theta) over the sum.
TEST(EnvCdf, ProbabilitiesAreLuminanceTimesSinTheta) {
    const int W = 64, H = 32;
    std::vector<float> px = map(W, H, 0.0f);
    for (int i = 0; i < W * H; ++i) {
        const float v = 0.01f + float((i * 2654435761u) % 1000u) / 100.0f;   // a ragged sky
        px[i * 4 + 0] = v; px[i * 4 + 1] = v * 0.5f; px[i * 4 + 2] = v * 2.0f;
    }
    EnvCdf cdf;
    cdf.build(px.data(), W, H);
    ASSERT_TRUE(cdf.valid());
    ASSERT_EQ(cdf.data().size(), size_t(H + W * H));
    EXPECT_EQ(cdf.marginal().back(), 1.0f);
    for (int y = 0; y < H; ++y) {
        EXPECT_EQ(cdf.conditional(y).back(), 1.0f);
        for (int x = 1; x < W; ++x) EXPECT_GE(cdf.conditional(y)[x], cdf.conditional(y)[x - 1]);
    }
    double total = 0.0;
    std::vector<double> w(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* c = &px[(y * W + x) * 4];
            w[size_t(y) * W + x] = (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) * std::sin(3.14159265358979 * (y + 0.5) / H);
            total += w[size_t(y) * W + x];
        }
    double sum = 0.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double p = cdf.probability(x, y);
            EXPECT_NEAR(p, w[size_t(y) * W + x] / total, 2e-6) << x << "," << y;
            sum += p;
        }
    EXPECT_NEAR(sum, 1.0, 1e-5);
}

// A hot spot takes its share: a large step in its row's CDF and in the marginal at its row.
TEST(EnvCdf, HotSpotConcentratesTheCdf) {
    const int W = 16, H = 8;
    std::vector<float> px = map(W, H, 0.01f);
    const int hotY = 4, hotX = 10;
    const int i = (hotY * W + hotX) * 4;
    px[i + 0] = px[i + 1] = px[i + 2] = 100.0f;
    EnvCdf cdf;
    cdf.build(px.data(), W, H);
    EXPECT_GT(cdf.probability(hotX, hotY), 0.9f);
    EXPECT_GT(cdf.conditional(hotY)[hotX] - cdf.conditional(hotY)[hotX - 1], 0.5f);
    EXPECT_GT(cdf.marginal()[hotY] - cdf.marginal()[hotY - 1], 0.5f);
}

// A black map (nothing to importance-sample) draws every texel alike.
TEST(EnvCdf, BlackMapIsUniform) {
    EnvCdf cdf;
    std::vector<float> px = map(8, 4, 0.0f);
    cdf.build(px.data(), 8, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) EXPECT_NEAR(cdf.probability(x, y), 1.0f / 32.0f, 1e-7f);
}
