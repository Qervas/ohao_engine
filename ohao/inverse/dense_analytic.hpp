#pragma once

// H4/M5a: analytic ∂L/∂albedo for free dense ground grid under Deferred.
// Model: beauty ≈ albedo ⊙ lighting. Lighting from white-albedo render.
// Floor UV from UV-coded albedo / lighting (nearest free-grid scatter).
// Always FD-check a few cells; never claim autodiff without agreement.

#include "inverse/dense_common.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"

#include "render/diff/diff_map.hpp"
#include "render/diff/diff_map_paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace ohao::inverse::dense_analytic {

inline constexpr double kCropX = 1.0;
inline constexpr double kCropYMin = 0.22; // ground + pedestal band (studio)

inline void fillWhite(ohao::diff::DiffAlbedoMap& m) {
    if (!m.empty()) m.fill(1.f, 1.f, 1.f);
}

/// UV-coded albedo: R=u, G=v, B=0.5 (decoded after / lighting).
inline void fillUvCoded(ohao::diff::DiffAlbedoMap& m) {
    if (m.empty()) return;
    for (std::uint32_t y = 0; y < m.desc.height; ++y) {
        for (std::uint32_t x = 0; x < m.desc.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(m.desc.width);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(m.desc.height);
            const size_t o = (static_cast<size_t>(y) * m.desc.width + x) * 3u;
            m.rgb[o + 0] = u;
            m.rgb[o + 1] = v;
            m.rgb[o + 2] = 0.5f;
        }
    }
}

struct UvBuffer {
    std::uint32_t w{0}, h{0};
    std::vector<float> uv; // w*h*2, -1 if invalid
    [[nodiscard]] bool empty() const noexcept { return uv.empty(); }
};

/// Estimate screen UV from UV-coded beauty / white lighting (per-pixel).
[[nodiscard]] inline UvBuffer estimateUv(const ImageRGBA8& iUv, const ImageRGBA8& iWhite,
                                         double cropX, double cropYMin) {
    UvBuffer out;
    if (iUv.width != iWhite.width || iUv.height != iWhite.height || iUv.empty()) return out;
    out.w = iUv.width;
    out.h = iUv.height;
    out.uv.assign(static_cast<size_t>(out.w) * out.h * 2u, -1.f);
    std::uint32_t xLim = 0, y0 = 0;
    detail::cropLimits(out.w, out.h, cropX, cropYMin, xLim, y0);
    for (std::uint32_t y = y0; y < out.h; ++y) {
        for (std::uint32_t x = 0; x < xLim; ++x) {
            const size_t o = (static_cast<size_t>(y) * out.w + x) * 4u;
            const float sr = std::max(1e-3f, iWhite.rgba[o + 0] / 255.f);
            const float sg = std::max(1e-3f, iWhite.rgba[o + 1] / 255.f);
            // Skip dark / non-ground (hero, sky): weak lighting response
            if (sr + sg < 0.04f) continue;
            float u = (iUv.rgba[o + 0] / 255.f) / sr;
            float v = (iUv.rgba[o + 1] / 255.f) / sg;
            u = std::clamp(u, 0.f, 1.f);
            v = std::clamp(v, 0.f, 1.f);
            const size_t uo = (static_cast<size_t>(y) * out.w + x) * 2u;
            out.uv[uo + 0] = u;
            out.uv[uo + 1] = v;
        }
    }
    return out;
}

/// Analytic grid gradient for MSE: ∂/∂A (||A⊙S − T||²) ≈ 2(I−T)⊙S scattered to G×G.
/// `gridRgb` is G*G*3 free params. Returns same layout gradient.
[[nodiscard]] inline std::vector<double> gridGradMse(const ImageRGBA8& pred, const ImageRGBA8& tgt,
                                                     const ImageRGBA8& lighting, const UvBuffer& uv,
                                                     int G, double cropX, double cropYMin) {
    std::vector<double> g(static_cast<size_t>(G) * G * 3u, 0.0);
    std::vector<double> w(static_cast<size_t>(G) * G, 0.0);
    if (pred.empty() || tgt.empty() || lighting.empty() || uv.empty() || G < 1) return g;
    if (pred.width != tgt.width || pred.height != tgt.height) return g;
    std::uint32_t xLim = 0, y0 = 0;
    detail::cropLimits(pred.width, pred.height, cropX, cropYMin, xLim, y0);
    for (std::uint32_t y = y0; y < pred.height; ++y) {
        for (std::uint32_t x = 0; x < xLim; ++x) {
            const size_t uo = (static_cast<size_t>(y) * uv.w + x) * 2u;
            if (uo + 1 >= uv.uv.size()) continue;
            const float uu = uv.uv[uo + 0];
            const float vv = uv.uv[uo + 1];
            if (uu < 0.f || vv < 0.f) continue;
            const int gx = std::min(G - 1, static_cast<int>(uu * static_cast<float>(G)));
            const int gy = std::min(G - 1, static_cast<int>(vv * static_cast<float>(G)));
            const size_t gi = static_cast<size_t>(gy * G + gx);
            const size_t o = (static_cast<size_t>(y) * pred.width + x) * 4u;
            for (int c = 0; c < 3; ++c) {
                const double ip = pred.rgba[o + c] / 255.0;
                const double it = tgt.rgba[o + c] / 255.0;
                const double s = lighting.rgba[o + c] / 255.0;
                // dL/dA = 2 (I-T) * dI/dA, dI/dA ≈ S (linear albedo)
                g[gi * 3u + static_cast<size_t>(c)] += 2.0 * (ip - it) * s;
            }
            w[gi] += 1.0;
        }
    }
    for (int i = 0; i < G * G; ++i) {
        if (w[static_cast<size_t>(i)] > 1.0) {
            const double inv = 1.0 / w[static_cast<size_t>(i)];
            g[static_cast<size_t>(i) * 3 + 0] *= inv;
            g[static_cast<size_t>(i) * 3 + 1] *= inv;
            g[static_cast<size_t>(i) * 3 + 2] *= inv;
        }
    }
    return g;
}

struct GradCheckResult {
    bool pass{false};
    double medianRelErr{1.0};
    double scale{1.0}; // median |fd|/|analytic| for step calibration
    int nCompared{0};
};

/// Central FD vs analytic on random free-grid coords. Median |a-f|/(|f|+eps) < thr.
template <typename LossFn>
[[nodiscard]] inline GradCheckResult checkFdAgreement(const std::vector<double>& grid, int G,
                                                      const std::vector<double>& analytic,
                                                      LossFn&& lossAt, int nProbe, std::uint32_t seed,
                                                      double thr = 0.15) {
    GradCheckResult r{};
    if (grid.size() != analytic.size() || G < 1) return r;
    const int nGrid = G * G * 3;
    std::uint32_t rng = 0xA11CEu ^ seed;
    auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    std::vector<double> rels;
    std::vector<double> scales;
    rels.reserve(static_cast<size_t>(nProbe));
    const double eps = 0.03;
    for (int k = 0; k < nProbe; ++k) {
        const size_t gi = static_cast<size_t>(rnd() % static_cast<std::uint32_t>(nGrid));
        auto gp = grid;
        auto gm = grid;
        gp[gi] = std::clamp(gp[gi] + eps, 0.02, 1.0);
        gm[gi] = std::clamp(gm[gi] - eps, 0.02, 1.0);
        const double fd = (lossAt(gp) - lossAt(gm)) / (gp[gi] - gm[gi] + 1e-12);
        const double an = analytic[gi];
        const double denom = std::abs(fd) + 1e-4;
        rels.push_back(std::abs(an - fd) / denom);
        if (std::abs(an) > 1e-8) scales.push_back(std::abs(fd) / (std::abs(an) + 1e-12));
    }
    if (rels.empty()) return r;
    std::sort(rels.begin(), rels.end());
    r.medianRelErr = rels[rels.size() / 2];
    r.nCompared = static_cast<int>(rels.size());
    r.pass = r.medianRelErr < thr;
    if (!scales.empty()) {
        std::sort(scales.begin(), scales.end());
        r.scale = std::clamp(scales[scales.size() / 2], 0.1, 50.0);
    }
    return r;
}

/// One Adam-like step on free grid using analytic gradient (positive grad → increase loss).
inline void adamLikeStep(std::vector<double>& grid, const std::vector<double>& grad, double lr) {
    for (size_t i = 0; i < grid.size() && i < grad.size(); ++i) {
        // Descent: subtract gradient
        grid[i] = std::clamp(grid[i] - lr * grad[i], 0.02, 1.0);
    }
}

} // namespace ohao::inverse::dense_analytic
