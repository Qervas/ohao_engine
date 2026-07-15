#pragma once

// Inverse-rendering image buffers + losses (Phase 1).
// Works on LDR RGBA8 (path-tracer getPixels) and optional float linear buffers later.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
// std::ceil

namespace ohao::inverse {

struct ImageRGBA8 {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba;  // w*h*4

    [[nodiscard]] bool empty() const noexcept { return rgba.empty(); }
    [[nodiscard]] size_t pixelCount() const noexcept {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    static ImageRGBA8 fromSpan(uint32_t w, uint32_t h, std::span<const uint8_t> px) {
        ImageRGBA8 img;
        img.width = w;
        img.height = h;
        img.rgba.assign(px.begin(), px.end());
        return img;
    }
};

/// Mean squared error over RGB (ignore alpha). Returns +inf on size mismatch.
/// Optional axis-aligned crop in normalized coords [0,1]:
///   x in [0, xMaxFrac), y in [yMinFrac, 1)  (image y grows downward).
[[nodiscard]] inline double mseRGB(const ImageRGBA8& a, const ImageRGBA8& b,
                                   double xMaxFrac = 1.0, double yMinFrac = 0.0) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return std::numeric_limits<double>::infinity();
    }
    if (a.pixelCount() == 0) return 0.0;
    const uint32_t xLim =
        (xMaxFrac >= 1.0) ? a.width
                          : static_cast<uint32_t>(std::ceil(xMaxFrac * static_cast<double>(a.width)));
    const uint32_t y0 =
        (yMinFrac <= 0.0)
            ? 0u
            : static_cast<uint32_t>(std::floor(yMinFrac * static_cast<double>(a.height)));
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = y0; y < a.height; ++y) {
        for (uint32_t x = 0; x < xLim; ++x) {
            const size_t o = (static_cast<size_t>(y) * a.width + x) * 4;
            const double dr = (static_cast<double>(a.rgba[o + 0]) - b.rgba[o + 0]) / 255.0;
            const double dg = (static_cast<double>(a.rgba[o + 1]) - b.rgba[o + 1]) / 255.0;
            const double db = (static_cast<double>(a.rgba[o + 2]) - b.rgba[o + 2]) / 255.0;
            sum += dr * dr + dg * dg + db * db;
            ++count;
        }
    }
    if (count == 0) return 0.0;
    return sum / static_cast<double>(count * 3);
}

/// Mean absolute error over RGB in [0,1] (same crop as mseRGB).
[[nodiscard]] inline double maeRGB(const ImageRGBA8& a, const ImageRGBA8& b,
                                   double xMaxFrac = 1.0, double yMinFrac = 0.0) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return std::numeric_limits<double>::infinity();
    }
    if (a.pixelCount() == 0) return 0.0;
    const uint32_t xLim =
        (xMaxFrac >= 1.0) ? a.width
                          : static_cast<uint32_t>(std::ceil(xMaxFrac * static_cast<double>(a.width)));
    const uint32_t y0 =
        (yMinFrac <= 0.0)
            ? 0u
            : static_cast<uint32_t>(std::floor(yMinFrac * static_cast<double>(a.height)));
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = y0; y < a.height; ++y) {
        for (uint32_t x = 0; x < xLim; ++x) {
            const size_t o = (static_cast<size_t>(y) * a.width + x) * 4;
            sum += std::abs(static_cast<double>(a.rgba[o + 0]) - b.rgba[o + 0]) / 255.0;
            sum += std::abs(static_cast<double>(a.rgba[o + 1]) - b.rgba[o + 1]) / 255.0;
            sum += std::abs(static_cast<double>(a.rgba[o + 2]) - b.rgba[o + 2]) / 255.0;
            ++count;
        }
    }
    if (count == 0) return 0.0;
    return sum / static_cast<double>(count * 3);
}

/// Hybrid loss: MSE + w * MAE (more robust FD under MC noise).
[[nodiscard]] inline double hybridRGB(const ImageRGBA8& a, const ImageRGBA8& b,
                                      double xMaxFrac = 1.0, double yMinFrac = 0.0,
                                      double maeWeight = 0.35) {
    const double mse = mseRGB(a, b, xMaxFrac, yMinFrac);
    const double mae = maeRGB(a, b, xMaxFrac, yMinFrac);
    if (!std::isfinite(mse)) return mse;
    if (!std::isfinite(mae)) return mse;
    return mse + maeWeight * mae;
}

/// Relative L2 on RGB in [0,1].
[[nodiscard]] inline double rmseRGB(const ImageRGBA8& a, const ImageRGBA8& b) {
    return std::sqrt(mseRGB(a, b));
}

/// Max absolute channel error in [0,255] (for recovery diagnostics).
[[nodiscard]] inline double maxAbsRGB(const ImageRGBA8& a, const ImageRGBA8& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double m = 0.0;
    const size_t n = a.pixelCount();
    for (size_t i = 0; i < n; ++i) {
        const size_t o = i * 4;
        for (int c = 0; c < 3; ++c) {
            m = std::max(m, std::abs(static_cast<double>(a.rgba[o + c]) - b.rgba[o + c]));
        }
    }
    return m;
}

} // namespace ohao::inverse
