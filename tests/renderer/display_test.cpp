// The display transform (frontier F2 T1): shaders/display/display.slang run on the CPU through its
// slangc C++ build (tools/display), against render/display/display.hpp's twin (OIDN's frames) and
// against what the curves must do.
#include <gtest/gtest.h>
#include "display_cpu.hpp"
#include "render/display/display.hpp"

#include <array>
#include <cmath>

using ohao::display::displayPixelGpu;
using ohao::render::DisplayCurve;

namespace {
std::array<float, 3> twin(std::array<float, 3> c, float exposure, DisplayCurve curve) {
    std::array<float, 3> out{};
    ohao::render::displayPixel(c.data(), exposure, curve, out.data());
    return out;
}
}  // namespace

// The twin is the module, to the 8-bit code and within 2e-6, over five decades of radiance,
// coloured and grey, both curves, three exposures.
TEST(Display, TwinIsTheModule) {
    for (const uint32_t curve : {0u, 1u})
        for (const float ev : {-2.0f, 0.5f, 6.0f})
            for (int i = 0; i <= 200; ++i) {
                const float l = std::pow(10.0f, -3.0f + 5.0f * float(i) / 200.0f);
                for (const std::array<float, 3> c : {std::array<float, 3>{l, l, l}, {l, 0.4f * l, 0.1f * l},
                                                     {0.05f * l, 0.9f * l, 0.3f * l}}) {
                    const float e = ohao::render::displayExposure(ev);
                    const auto g = displayPixelGpu(c, e, curve);
                    const auto t = twin(c, e, DisplayCurve(curve));
                    for (int k = 0; k < 3; ++k) {
                        EXPECT_NEAR(g[k], t[k], 2e-6f) << "curve " << curve << " ev " << ev << " L " << l;
                        EXPECT_EQ(ohao::render::displayByte(g[k]), ohao::render::displayByte(t[k]));
                    }
                }
            }
}

// EV100 0 is an exposure of 1/1.2 (Lagarde and de Rousiers), and one EV halves it.
TEST(Display, ExposureIsEv100) {
    EXPECT_FLOAT_EQ(ohao::render::displayExposure(0.0f), 1.0f / 1.2f);
    EXPECT_FLOAT_EQ(ohao::render::displayExposure(1.0f), 0.5f / 1.2f);
}

// The sRGB transfer function (PBR Neutral passes these greys through, less its 0.04 offset above
// its toe at 0.08).
TEST(Display, SrgbEncoding) {
    const auto at = [](float v) { return displayPixelGpu({v + 0.04f, v + 0.04f, v + 0.04f}, 1.0f, 1u)[0]; };
    EXPECT_NEAR(at(0.04f), 0.22091636f, 1e-5f);
    EXPECT_NEAR(at(0.18f), 0.46135612f, 1e-5f);
    EXPECT_NEAR(at(0.5f), 0.73535698f, 1e-5f);
}

// PBR Neutral shows a base colour under unit light as itself (less its 0.04 toe offset) below its
// compression start, and compresses above it without passing 1.
TEST(Display, NeutralKeepsBaseColours) {
    const std::array<float, 3> c{0.5f, 0.3f, 0.2f};
    const auto out = displayPixelGpu(c, 1.0f, 1u);
    for (int k = 0; k < 3; ++k) {
        const float lin = c[k] - 0.04f;
        const float enc = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
        EXPECT_NEAR(out[k], enc, 1e-5f);
    }
    const auto hot = displayPixelGpu({50.0f, 20.0f, 5.0f}, 1.0f, 1u);
    for (float v : hot) EXPECT_LE(v, 1.0f);
}

// AgX keeps greys grey, rises with radiance, starts at black and reaches white without passing it.
TEST(Display, AgxIsAMonotonicNeutralCurve) {
    float last = -1.0f;
    for (int i = 0; i <= 120; ++i) {
        const float l = std::pow(2.0f, -12.0f + 18.0f * float(i) / 120.0f);
        const auto o = displayPixelGpu({l, l, l}, 1.0f, 0u);
        EXPECT_NEAR(o[0], o[1], 2e-3f);
        EXPECT_NEAR(o[1], o[2], 2e-3f);
        EXPECT_GE(o[1], last - 1e-6f) << l;
        last = o[1];
    }
    EXPECT_LT(displayPixelGpu({0.0f, 0.0f, 0.0f}, 1.0f, 0u)[1], 0.02f);
    EXPECT_GT(displayPixelGpu({1e4f, 1e4f, 1e4f}, 1.0f, 0u)[1], 0.98f);
    EXPECT_LE(displayPixelGpu({1e4f, 1e4f, 1e4f}, 1.0f, 0u)[1], 1.0f);
}
