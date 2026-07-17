#pragma once

// Inverse-rendering image buffers + losses (Phase 1 / B6).
// Works on LDR RGBA8 (path-tracer getPixels) and optional float linear buffers later.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

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

namespace detail {

inline void cropLimits(uint32_t w, uint32_t h, double xMaxFrac, double yMinFrac,
                       uint32_t& xLim, uint32_t& y0) {
    xLim = (xMaxFrac >= 1.0) ? w
                             : static_cast<uint32_t>(std::ceil(xMaxFrac * static_cast<double>(w)));
    y0 = (yMinFrac <= 0.0)
             ? 0u
             : static_cast<uint32_t>(std::floor(yMinFrac * static_cast<double>(h)));
    xLim = std::min(xLim, w);
    y0 = std::min(y0, h);
}

[[nodiscard]] inline double luma01(uint8_t r, uint8_t g, uint8_t b) {
    return (0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) +
            0.0722 * static_cast<double>(b)) /
           255.0;
}

} // namespace detail

/// Mean squared error over RGB (ignore alpha). Returns +inf on size mismatch.
/// Optional axis-aligned crop in normalized coords [0,1]:
///   x in [0, xMaxFrac), y in [yMinFrac, 1)  (image y grows downward).
[[nodiscard]] inline double mseRGB(const ImageRGBA8& a, const ImageRGBA8& b,
                                   double xMaxFrac = 1.0, double yMinFrac = 0.0) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return std::numeric_limits<double>::infinity();
    }
    if (a.pixelCount() == 0) return 0.0;
    uint32_t xLim = 0, y0 = 0;
    detail::cropLimits(a.width, a.height, xMaxFrac, yMinFrac, xLim, y0);
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
    uint32_t xLim = 0, y0 = 0;
    detail::cropLimits(a.width, a.height, xMaxFrac, yMinFrac, xLim, y0);
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

/// Specular-biased hybrid loss for metal/rough recovery.
/// Bright / high-contrast pixels (highlights, mirror glints) get higher weight so
/// roughness+metallic are not under-determined by flat diffuse MSE.
///
/// `specularWeight` in [0,1]: 0 = plain hybrid; 1 = fully highlight-weighted.
[[nodiscard]] inline double hybridSpecularRGB(const ImageRGBA8& pred, const ImageRGBA8& target,
                                              double xMaxFrac = 1.0, double yMinFrac = 0.0,
                                              double maeWeight = 0.35,
                                              double specularWeight = 0.55) {
    if (pred.width != target.width || pred.height != target.height ||
        pred.rgba.size() != target.rgba.size()) {
        return std::numeric_limits<double>::infinity();
    }
    if (pred.pixelCount() == 0) return 0.0;
    const double sw = std::clamp(specularWeight, 0.0, 1.0);
    if (sw <= 1e-9) return hybridRGB(pred, target, xMaxFrac, yMinFrac, maeWeight);

    uint32_t xLim = 0, y0 = 0;
    detail::cropLimits(pred.width, pred.height, xMaxFrac, yMinFrac, xLim, y0);

    double sumMse = 0.0, sumMae = 0.0, sumW = 0.0;
    size_t count = 0;
    for (uint32_t y = y0; y < pred.height; ++y) {
        for (uint32_t x = 0; x < xLim; ++x) {
            const size_t o = (static_cast<size_t>(y) * pred.width + x) * 4;
            const double tr = target.rgba[o + 0] / 255.0;
            const double tg = target.rgba[o + 1] / 255.0;
            const double tb = target.rgba[o + 2] / 255.0;
            const double pr = pred.rgba[o + 0] / 255.0;
            const double pg = pred.rgba[o + 1] / 255.0;
            const double pb = pred.rgba[o + 2] / 255.0;
            const double dr = pr - tr, dg = pg - tg, db = pb - tb;
            const double mse = dr * dr + dg * dg + db * db;
            const double mae = std::abs(dr) + std::abs(dg) + std::abs(db);

            // Target luma + local highlight proxy (max channel).
            const double lumaT = 0.2126 * tr + 0.7152 * tg + 0.0722 * tb;
            const double maxT = std::max({tr, tg, tb});
            // Also weight mismatch in bright pred (missed glint still matters).
            const double maxP = std::max({pr, pg, pb});
            const double highlight = std::max(maxT, 0.65 * maxP);
            // Soft floor keeps diffuse regions alive; power boosts glints.
            const double wSpec = 0.18 + std::pow(std::max(highlight, lumaT), 1.35);
            const double w = (1.0 - sw) + sw * wSpec;

            sumMse += w * mse;
            sumMae += w * mae;
            sumW += w;
            ++count;
        }
    }
    if (count == 0 || sumW <= 0.0) return 0.0;
    const double inv = 1.0 / (sumW * 3.0);
    return sumMse * inv + maeWeight * sumMae * inv;
}

/// Relative L2 on RGB in [0,1].
[[nodiscard]] inline double rmseRGB(const ImageRGBA8& a, const ImageRGBA8& b) {
    return std::sqrt(mseRGB(a, b));
}

/// PSNR (dB) with peak = 1.0 on RGB in [0,1] using mseRGB. Higher is better.
[[nodiscard]] inline double psnrRGB(const ImageRGBA8& a, const ImageRGBA8& b) {
    const double mse = mseRGB(a, b);
    if (!std::isfinite(mse)) return 0.0;
    if (mse <= 1e-12) return 99.0;
    return -10.0 * std::log10(mse);
}

/// Mean SSIM over RGB channels (simplified, Gaussian-free window = full image stats).
/// Suitable for lab reporting; not identical to multi-scale SSIM.
[[nodiscard]] inline double ssimRGB(const ImageRGBA8& a, const ImageRGBA8& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size() ||
        a.empty()) {
        return 0.0;
    }
    const size_t n = a.pixelCount();
    if (n == 0) return 0.0;
    // Per-channel global SSIM (stable for reporting).
    double ssimSum = 0.0;
    constexpr double C1 = 0.01 * 0.01;
    constexpr double C2 = 0.03 * 0.03;
    for (int c = 0; c < 3; ++c) {
        double meanA = 0.0, meanB = 0.0;
        for (size_t i = 0; i < n; ++i) {
            meanA += a.rgba[i * 4 + static_cast<size_t>(c)] / 255.0;
            meanB += b.rgba[i * 4 + static_cast<size_t>(c)] / 255.0;
        }
        meanA /= static_cast<double>(n);
        meanB /= static_cast<double>(n);
        double varA = 0.0, varB = 0.0, cov = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double va = a.rgba[i * 4 + static_cast<size_t>(c)] / 255.0 - meanA;
            const double vb = b.rgba[i * 4 + static_cast<size_t>(c)] / 255.0 - meanB;
            varA += va * va;
            varB += vb * vb;
            cov += va * vb;
        }
        varA /= static_cast<double>(n);
        varB /= static_cast<double>(n);
        cov /= static_cast<double>(n);
        const double num = (2.0 * meanA * meanB + C1) * (2.0 * cov + C2);
        const double den = (meanA * meanA + meanB * meanB + C1) * (varA + varB + C2);
        ssimSum += (den > 0.0) ? (num / den) : 0.0;
    }
    return ssimSum / 3.0;
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

/// Nearest-neighbor resize (for external photo targets → FIT/SHOW budgets).
[[nodiscard]] inline ImageRGBA8 resizeNearest(const ImageRGBA8& src, uint32_t dw, uint32_t dh) {
    ImageRGBA8 out;
    out.width = dw;
    out.height = dh;
    if (src.empty() || dw == 0 || dh == 0) return out;
    out.rgba.resize(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t sy = std::min(src.height - 1,
                                     static_cast<uint32_t>((static_cast<uint64_t>(y) * src.height) / dh));
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t sx = std::min(src.width - 1,
                                         static_cast<uint32_t>((static_cast<uint64_t>(x) * src.width) / dw));
            const size_t si = (static_cast<size_t>(sy) * src.width + sx) * 4;
            const size_t di = (static_cast<size_t>(y) * dw + x) * 4;
            out.rgba[di + 0] = src.rgba[si + 0];
            out.rgba[di + 1] = src.rgba[si + 1];
            out.rgba[di + 2] = src.rgba[si + 2];
            out.rgba[di + 3] = src.rgba[si + 3];
        }
    }
    return out;
}

/// Apply linear exposure gain in LDR space (clamp to 8-bit).
[[nodiscard]] inline ImageRGBA8 applyExposure(const ImageRGBA8& src, float exposure) {
    if (src.empty() || std::abs(exposure - 1.0f) < 1e-5f) return src;
    ImageRGBA8 out = src;
    const double e = static_cast<double>(exposure);
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const double v = std::clamp(out.rgba[i + static_cast<size_t>(c)] * e, 0.0, 255.0);
            out.rgba[i + static_cast<size_t>(c)] = static_cast<uint8_t>(v + 0.5);
        }
    }
    return out;
}

} // namespace ohao::inverse
