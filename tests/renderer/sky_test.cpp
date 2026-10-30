// The physical sky (frontier F2 T3, render/sky/atmosphere.*): Hillaire 2020 on the CPU, against a
// closed form and against what a sky must look like.
#include <gtest/gtest.h>
#include "render/sky/atmosphere.hpp"

#include <cmath>

using ohao::render::Atmosphere;
using ohao::render::AtmosphereParams;

namespace {
const glm::vec3 kUnitSun(1.0f);
glm::vec3 sunAt(float elevationDeg) {
    const float e = glm::radians(elevationDeg);
    return {std::cos(e), std::sin(e), 0.0f};
}
glm::vec3 dirAt(float elevationDeg, float azimuthDeg) {
    const float e = glm::radians(elevationDeg), a = glm::radians(azimuthDeg);
    return {std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)};
}
}  // namespace

// A pure Rayleigh atmosphere (no Mie, no ozone): the transmittance from the ground to the zenith is
// exp(-beta H (1 - e^(-(top - bottom) / H))), the exponential profile's optical depth.
TEST(Sky, ZenithTransmittanceIsTheClosedForm) {
    AtmosphereParams p;
    p.mieScattering = 0.0f;
    p.mieExtinction = 0.0f;
    p.ozoneAbsorption = glm::vec3(0.0f);
    const Atmosphere sky(p);
    const glm::vec3 t = sky.transmittance(p.bottomRadius, 1.0f);
    const float depthScale = p.rayleighScaleHeight * (1.0f - std::exp(-(p.topRadius - p.bottomRadius) / p.rayleighScaleHeight));
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(t[c], std::exp(-p.rayleighScattering[c] * depthScale), 2e-3f) << c;
}

// The sun below the horizon reaches nothing from the ground; a ray into the ground carries none.
TEST(Sky, TheGroundBlocksTheSun) {
    const Atmosphere sky;
    EXPECT_EQ(sky.transmittance(sky.params().bottomRadius + 10.0f, -0.2f), glm::vec3(0.0f));
}

// At noon the zenith is blue and bluer than the horizon, which is brighter and whiter.
TEST(Sky, NoonIsBlueAboveAndPaleAtTheHorizon) {
    const Atmosphere sky;
    const glm::vec3 zenith = sky.radiance(dirAt(90.0f, 0.0f), sunAt(80.0f), kUnitSun);
    const glm::vec3 horizon = sky.radiance(dirAt(2.0f, 180.0f), sunAt(80.0f), kUnitSun);
    EXPECT_GT(zenith.b, zenith.g);
    EXPECT_GT(zenith.g, zenith.r);
    EXPECT_GT(zenith.b / zenith.r, horizon.b / horizon.r);
    EXPECT_GT(horizon.r + horizon.g + horizon.b, zenith.r + zenith.g + zenith.b);
}

// At sunset the sky beside the sun is red and the sky opposite it blue.
TEST(Sky, SunsetIsRedTowardTheSun) {
    const Atmosphere sky;
    const glm::vec3 sun = sunAt(1.0f);
    const glm::vec3 toward = sky.radiance(dirAt(3.0f, 10.0f), sun, kUnitSun);
    EXPECT_GT(toward.r, toward.b);
    const glm::vec3 zenith = sky.radiance(dirAt(90.0f, 0.0f), sun, kUnitSun);
    EXPECT_GT(zenith.b, zenith.r);
}

// The sky is linear in the sun, and goes dark when the sun is well below the horizon.
TEST(Sky, LinearInTheSunAndDarkAtNight) {
    const Atmosphere sky;
    const glm::vec3 a = sky.radiance(dirAt(40.0f, 30.0f), sunAt(35.0f), kUnitSun);
    const glm::vec3 b = sky.radiance(dirAt(40.0f, 30.0f), sunAt(35.0f), glm::vec3(3.0f));
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(b[c], 3.0f * a[c], 1e-6f * std::max(1.0f, b[c]));
    const glm::vec3 night = sky.radiance(dirAt(90.0f, 0.0f), sunAt(-20.0f), kUnitSun);
    const glm::vec3 noon = sky.radiance(dirAt(90.0f, 0.0f), sunAt(80.0f), kUnitSun);
    EXPECT_LT(night.b, 1e-3f * noon.b);
}

// The map uses the environment's convention: row 0 the zenith, u = atan2(z, x) / 2pi + 0.5.
TEST(Sky, EquirectFollowsTheEnvironmentConvention) {
    const Atmosphere sky;
    const glm::vec3 sun = sunAt(30.0f);
    const uint32_t W = 64, H = 32;
    const std::vector<float> px = sky.equirect(W, H, sun, kUnitSun);
    ASSERT_EQ(px.size(), std::size_t{W} * H * 4u);
    // Texel (W/2, 0)'s centre: phi 0.5 texel past 0 (toward +z), elevation just under 90 degrees.
    const float phi = (0.5f / float(W)) * 2.0f * 3.14159265f;
    const float elev = (0.5f - 0.5f / float(H)) * 3.14159265f;
    const glm::vec3 d(std::cos(elev) * std::cos(phi), std::sin(elev), std::cos(elev) * std::sin(phi));
    const glm::vec3 want = sky.radiance(d, glm::normalize(sun), kUnitSun);
    const float* got = &px[(0 * W + W / 2) * 4];
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(got[c], want[c], 1e-6f * std::max(1.0f, want[c]));
    EXPECT_EQ(got[3], 1.0f);
}
