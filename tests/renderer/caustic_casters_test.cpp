// Caustics' casters (sub-project 24, render/rt/caustic_casters.hpp): which materials can hold a smooth
// lobe, the disks a directional light's rays are aimed at, and the density those rays' origins are
// drawn with (the light pass's estimate divides by it, so it must be the sampler's exactly).
#include <gtest/gtest.h>
#include "render/rt/caustic_casters.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace ohao::render;

namespace {

constexpr float kPi = 3.14159265358979f;

uint32_t pcg(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(uint32_t& s) {
    s = pcg(s);
    return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
}

CausticLightInput sunDown(float e = 3.0f) { return {glm::vec3(0.0f, -1.0f, 0.0f), e}; }

}  // namespace

// No directional light, or nothing that can be smooth: nothing to trace.
TEST(CausticCasters, CasterNone) {
    const std::vector<CausticCasterInput> none;
    const std::vector<CausticLightInput> lights{sunDown()};
    EXPECT_EQ(buildCausticCasters(none, lights).totalPower, 0.0f);
    const std::vector<CausticCasterInput> one{{glm::vec3(-1.0f), glm::vec3(1.0f), 7u}};
    const std::vector<CausticLightInput> noLights;
    EXPECT_EQ(buildCausticCasters(one, noLights).totalPower, 0.0f);
}

// A roughness texture's minimum makes a rough factor a caster where the texture goes smooth.
TEST(CausticCasters, CasterTextureMin) {
    std::vector<uint8_t> rgba(4 * 4 * 4, 255);
    rgba[5 * 4 + 1] = 26;   // one texel's G (roughness) at 26/255 ~ 0.10
    const float m = textureChannelMin(rgba, 4, 4, 1);
    EXPECT_NEAR(m, 26.0f / 255.0f, 1e-6f);
    MaterialSmoothness s;
    s.specularRoughness = 1.0f;
    s.specularTexMin = m;
    EXPECT_TRUE(canBeSmooth(s, 0.25f));
    s.specularTexMin = 0.3f;
    EXPECT_FALSE(canBeSmooth(s, 0.25f));
    // A smooth coat over a rough base is a caster; a coat of weight 0 is not.
    MaterialSmoothness coated;
    coated.specularRoughness = 0.35f;
    coated.coatWeight = 1.0f;
    coated.coatRoughness = 0.04f;
    EXPECT_TRUE(canBeSmooth(coated, 0.25f));
    coated.coatWeight = 0.0f;
    EXPECT_FALSE(canBeSmooth(coated, 0.25f));
}

// Glass is a caster by its roughness, as any specular lobe: 0.5 no, 0.05 yes.
TEST(CausticCasters, CasterGlass) {
    MaterialSmoothness glass;
    glass.specularRoughness = 0.5f;
    EXPECT_FALSE(canBeSmooth(glass, 0.25f));
    glass.specularRoughness = 0.05f;
    EXPECT_TRUE(canBeSmooth(glass, 0.25f));
    // Exactly at the cut is rough (the module's evalParts: roughness < cut is smooth).
    glass.specularRoughness = 0.25f;
    EXPECT_FALSE(canBeSmooth(glass, 0.25f));
}

// Two overlapping disks: origins drawn as the shader draws them (a light by its probability, a disk
// by area, a point uniform on it) land with the density causticOriginDensity gives.
TEST(CausticCasters, CasterDensity) {
    // Two boxes seen from above: spheres of radius sqrt(3) and 2 sqrt(3) / 2... centres 1.5 apart.
    const std::vector<CausticCasterInput> casters{{glm::vec3(-1.0f), glm::vec3(1.0f), 1u},
                                                   {glm::vec3(0.5f, -0.5f, -0.5f), glm::vec3(1.5f, 0.5f, 0.5f), 2u}};
    const std::vector<CausticLightInput> lights{sunDown(2.0f)};
    const CausticCasters c = buildCausticCasters(casters, lights);
    ASSERT_EQ(c.disks.size(), 2u);
    ASSERT_GT(c.totalPower, 0.0f);
    // The grid over the union's xz bounds.
    constexpr int kN = 64;
    const float x0 = -2.0f, x1 = 2.5f, z0 = -2.0f, z1 = 2.0f;
    const float cell = (x1 - x0) / kN * (z1 - z0) / kN;
    std::vector<double> hist(kN * kN, 0.0);
    constexpr unsigned kSamples = 1u << 20;
    uint32_t seed = 9u;
    for (unsigned k = 0; k < kSamples; ++k) {
        const CausticOrigin o = sampleCausticOrigin(c, rnd(seed), rnd(seed), rnd(seed), rnd(seed));
        ASSERT_EQ(o.light, 0u);
        const int i = static_cast<int>((o.point.x - x0) / (x1 - x0) * kN);
        const int j = static_cast<int>((o.point.z - z0) / (z1 - z0) * kN);
        if (i >= 0 && i < kN && j >= 0 && j < kN) hist[j * kN + i] += 1.0;
    }
    double maxDensity = 0.0;
    std::vector<double> want(kN * kN);
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) {
            const glm::vec3 x(x0 + (i + 0.5f) * (x1 - x0) / kN, 3.0f, z0 + (j + 0.5f) * (z1 - z0) / kN);
            want[j * kN + i] = causticOriginDensity(c, 0u, x);
            maxDensity = std::max(maxDensity, want[j * kN + i]);
        }
    ASSERT_GT(maxDensity, 0.0);
    int checked = 0;
    for (int k = 0; k < kN * kN; ++k) {
        // Cells wholly inside one coverage count (a cell cut by a disk's edge averages two densities).
        const int i = k % kN, j = k / kN;
        if (want[k] < 0.01 * maxDensity) continue;
        bool uniform = true;
        for (int dj = -1; dj <= 1 && uniform; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const int ii = i + di, jj = j + dj;
                if (ii < 0 || jj < 0 || ii >= kN || jj >= kN || want[jj * kN + ii] != want[k]) {
                    uniform = false;
                    break;
                }
            }
        if (!uniform) continue;
        const double got = hist[k] / (kSamples * cell);
        EXPECT_NEAR(got, want[k], 0.02 * want[k] + 4.0 * std::sqrt(want[k] / (kSamples * cell))) << "cell " << i << "," << j;
        ++checked;
    }
    EXPECT_GT(checked, 400);
    // Where both disks cover a point it is drawn twice as densely as where one does (equal areas
    // here would give exactly 2; the sum is over covering disks).
    const glm::vec3 both(0.75f, 3.0f, 0.0f), onlyFirst(-1.5f, 3.0f, 0.0f);
    EXPECT_NEAR(causticOriginDensity(c, 0u, both) / causticOriginDensity(c, 0u, onlyFirst), 2.0, 1e-5);
    // Its integral over the plane is 1.
    double integral = 0.0;
    for (double w : want) integral += w * cell;
    EXPECT_NEAR(integral, 1.0, 0.03);
    (void)kPi;
}

// One frame's light paths must not be confined to a block of the seed space: seeded as pcg(index ^ h(frame)),
// a frame's N paths took the states of one aligned block of N (its top bits h's), so at 1080p x 8 rays (about
// 2^24 paths) two frames shared almost all their paths one time in 256 and caustic noise stopped falling. The
// seeds before the PCG walk must spread over the whole range, for any frame.
TEST(CausticCasters, SeedsSpreadOverTheRange) {
    for (uint32_t frame : {0u, 7u, 1000u}) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0u;
        std::vector<uint32_t> pre;
        for (uint32_t i = 0; i < (1u << 20); ++i) {
            const uint32_t s = causticPathSeedInput(i, frame);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        EXPECT_GT(hi - lo, 1u << 31) << "frame " << frame << ": the seeds' inputs span only " << (hi - lo);
    }
}
