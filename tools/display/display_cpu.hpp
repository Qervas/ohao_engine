#pragma once

// shaders/display/display.slang on the CPU (tools/display/display_kernel.slang, compiled to C++ by
// slangc): what tests/renderer/display_test holds render/display/display.hpp's twin to.

#include <array>
#include <cstdint>

namespace ohao::display {

// The module's displayPixel: scene-linear radiance at a linear exposure through `curve` (0 AgX,
// 1 PBR Neutral) to sRGB-encoded [0, 1].
std::array<float, 3> displayPixelGpu(std::array<float, 3> radiance, float exposure, uint32_t curve);

} // namespace ohao::display
