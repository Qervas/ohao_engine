#pragma once
// A capture preview's size: the source scaled down, aspect kept, so its long side is at most
// maxSide; a source that fits keeps its size. One rule for the GPU preview (capture.preview)
// and the CPU one (runtime::fitWithin), so both give the same dimensions.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ohao::render {

[[nodiscard]] inline std::pair<uint32_t, uint32_t> previewExtent(uint32_t width, uint32_t height,
                                                                 uint32_t maxSide) noexcept {
    const uint32_t longSide = std::max(width, height);
    if (maxSide == 0 || longSide <= maxSide) return {width, height};
    const double s = static_cast<double>(maxSide) / longSide;
    return {std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(width * s))),
            std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(height * s)))};
}

} // namespace ohao::render
