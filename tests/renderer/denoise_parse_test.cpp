#include <gtest/gtest.h>
#include "render/rt/denoise/denoise_types.hpp"

using ohao::DenoiseMode;
using ohao::parseDenoiseMode;
using ohao::denoiseModeName;

TEST(DenoiseTypes, ParsesKnownModes) {
    EXPECT_EQ(parseDenoiseMode("none"), DenoiseMode::None);
    EXPECT_EQ(parseDenoiseMode("oidn"), DenoiseMode::OIDN);
}

TEST(DenoiseTypes, CaseInsensitive) {
    EXPECT_EQ(parseDenoiseMode("OIDN"), DenoiseMode::OIDN);
    EXPECT_EQ(parseDenoiseMode("None"), DenoiseMode::None);
    EXPECT_EQ(parseDenoiseMode("Oidn"), DenoiseMode::OIDN);
}

TEST(DenoiseTypes, UnknownFallsBackToNone) {
    EXPECT_EQ(parseDenoiseMode("gibberish"), DenoiseMode::None);
    EXPECT_EQ(parseDenoiseMode(""), DenoiseMode::None);
}

TEST(DenoiseTypes, NameRoundTrip) {
    EXPECT_STREQ(denoiseModeName(DenoiseMode::None), "none");
    EXPECT_STREQ(denoiseModeName(DenoiseMode::OIDN), "oidn");
}
