// The environment's sun, found and painted out (render/rt/env_sun.hpp): what a directional
// light stands for when a scene has one, so RT GI's cosine rays never meet it.
#include <gtest/gtest.h>
#include "render/rt/env_sun.hpp"

#include <cmath>
#include <vector>

using ohao::render::EnvSun;
using ohao::render::separateEnvSun;

namespace {

constexpr float kPi = 3.14159265358979f;

std::vector<float> sky(int w, int h, float r, float g, float b) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 1.0f;
    }
    return px;
}

void put(std::vector<float>& px, int w, int x, int y, float v) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
    px[i] = px[i + 1] = px[i + 2] = v;
}

float texelSolidAngle(int w, int h, int y) {
    const float elevation = ((static_cast<float>(y) + 0.5f) / static_cast<float>(h) - 0.5f) * kPi;
    return (2.0f * kPi / static_cast<float>(w)) * (kPi / static_cast<float>(h)) * std::cos(elevation);
}

}  // namespace

// A three-texel sun on a flat sky: found, every sun texel painted with the sky around it, its
// irradiance the radiance it carried above that sky, its direction the engine's lookup direction.
TEST(EnvSun, SunIsFoundAndPaintedOut) {
    const int W = 256, H = 128;   // a texel about 6e-4 sr: three are a sun-sized 1.7e-3
    auto px = sky(W, H, 0.4f, 0.5f, 0.6f);
    const int sy = 80;   // below the middle row: elevation > 0 under v = asin(y)/pi + 0.5
    put(px, W, 10, sy, 5.0e4f);
    put(px, W, 11, sy, 5.0e4f);
    put(px, W, 10, sy + 1, 2.0e4f);

    const EnvSun sun = separateEnvSun(px, W, H);
    ASSERT_TRUE(sun.found);
    EXPECT_EQ(sun.texels, 3u);
    for (size_t i = 0; i < px.size(); i += 4) {
        EXPECT_FLOAT_EQ(px[i], 0.4f);
        EXPECT_FLOAT_EQ(px[i + 1], 0.5f);
        EXPECT_FLOAT_EQ(px[i + 2], 0.6f);
    }
    const float expectedR = (5.0e4f - 0.4f) * (2.0f * texelSolidAngle(W, H, sy)) + (2.0e4f - 0.4f) * texelSolidAngle(W, H, sy + 1);
    EXPECT_NEAR(sun.irradiance.r, expectedR, expectedR * 1e-4f);
    EXPECT_GT(sun.powerShare, 0.9f);
    // The engine's lookup: u = atan2(z, x) / 2pi + 0.5, v = asin(y) / pi + 0.5 (row 0 at v = 0).
    const float u = 11.0f / W, v = (sy + 0.5f) / H;   // the three texels' weighted centre, nearly
    const glm::vec3 d = sun.direction;
    EXPECT_NEAR(std::atan2(d.z, d.x) / (2.0f * kPi) + 0.5f, u, 0.6f / W);
    EXPECT_NEAR(std::asin(d.y) / kPi + 0.5f, v, 0.6f / H);
    EXPECT_NEAR(glm::length(d), 1.0f, 1e-5f);
}

// The flood fill wraps around the map's seam (u = 0 and u = 1 are the same azimuth).
TEST(EnvSun, SunAcrossTheSeamIsOneSun) {
    const int W = 256, H = 128;
    auto px = sky(W, H, 0.3f, 0.3f, 0.3f);
    put(px, W, 0, 12, 4.0e4f);
    put(px, W, W - 1, 12, 4.0e4f);
    const EnvSun sun = separateEnvSun(px, W, H);
    ASSERT_TRUE(sun.found);
    EXPECT_EQ(sun.texels, 2u);
    EXPECT_FLOAT_EQ(px[(12 * W + 0) * 4], 0.3f);
    EXPECT_FLOAT_EQ(px[(12 * W + W - 1) * 4], 0.3f);
}

// Studio softboxes a few hundred times the sky are lights the environment keeps: no sun, and
// the map is untouched.
TEST(EnvSun, StudioSoftboxesAreNotASun) {
    const int W = 64, H = 32;
    auto px = sky(W, H, 0.3f, 0.3f, 0.3f);
    for (int x = 20; x < 26; ++x)
        for (int y = 8; y < 12; ++y) put(px, W, x, y, 500.0f);
    const auto before = px;
    const EnvSun sun = separateEnvSun(px, W, H);
    EXPECT_FALSE(sun.found);
    EXPECT_EQ(px, before);
}

// A bright region too wide to be a sun (an overexposed sky patch) is not painted out.
TEST(EnvSun, WideBrightRegionIsNotASun) {
    const int W = 128, H = 64;
    auto px = sky(W, H, 0.3f, 0.3f, 0.3f);
    for (int x = 30; x < 70; ++x)
        for (int y = 20; y < 40; ++y) put(px, W, x, y, 1.0e4f);
    const auto before = px;
    const EnvSun sun = separateEnvSun(px, W, H);
    EXPECT_FALSE(sun.found);
    EXPECT_EQ(px, before);
}

// A dim halo connected to the sun goes with it; a bright cloud elsewhere, not connected, stays.
TEST(EnvSun, OnlyTheSunsConnectedRegionGoes) {
    const int W = 256, H = 128;
    auto px = sky(W, H, 0.2f, 0.2f, 0.2f);
    put(px, W, 30, 10, 6.0e4f);
    put(px, W, 31, 10, 50.0f);   // halo: over 64 x the median sky, touching the sun
    put(px, W, 50, 20, 50.0f);   // as bright, elsewhere
    const EnvSun sun = separateEnvSun(px, W, H);
    ASSERT_TRUE(sun.found);
    EXPECT_EQ(sun.texels, 2u);
    EXPECT_FLOAT_EQ(px[(10 * W + 31) * 4], 0.2f);
    EXPECT_FLOAT_EQ(px[(20 * W + 50) * 4], 50.0f);
}

// No map, or a black one: nothing found, nothing written.
TEST(EnvSun, EmptyAndBlackMaps) {
    std::vector<float> none;
    EXPECT_FALSE(separateEnvSun(none, 0, 0).found);
    auto black = sky(16, 8, 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(separateEnvSun(black, 16, 8).found);
}
