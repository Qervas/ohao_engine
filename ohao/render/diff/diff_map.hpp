#pragma once

// Dense albedo map (linear RGB floats) for Diff-IR inverse.

#include "render/diff/diff_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ohao::diff {

struct DiffAlbedoMap {
    DiffMapDesc desc{};
    std::vector<float> rgb; // w*h*3 linear

    void allocate(std::uint32_t w, std::uint32_t h) {
        desc.width = w;
        desc.height = h;
        desc.channels = 3;
        rgb.assign(static_cast<size_t>(w) * h * 3u, 0.45f);
    }

    [[nodiscard]] bool empty() const noexcept { return rgb.empty(); }

    [[nodiscard]] size_t pixelCount() const noexcept {
        return static_cast<size_t>(desc.width) * desc.height;
    }

    void fill(float r, float g, float b) {
        for (size_t i = 0; i < pixelCount(); ++i) {
            rgb[i * 3 + 0] = r;
            rgb[i * 3 + 1] = g;
            rgb[i * 3 + 2] = b;
        }
    }

    /// Sample bilinear albedo at UV in [0,1]^2 (v grows with +z / tex v).
    void sample(float u, float v, float& r, float& g, float& b) const {
        if (empty()) {
            r = g = b = 0.45f;
            return;
        }
        u = std::clamp(u, 0.f, 1.f);
        v = std::clamp(v, 0.f, 1.f);
        const float x = u * static_cast<float>(desc.width - 1);
        const float y = v * static_cast<float>(desc.height - 1);
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, static_cast<int>(desc.width) - 1);
        const int y1 = std::min(y0 + 1, static_cast<int>(desc.height) - 1);
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);
        auto at = [&](int ix, int iy, int c) {
            return rgb[(static_cast<size_t>(iy) * desc.width + static_cast<size_t>(ix)) * 3u +
                       static_cast<size_t>(c)];
        };
        for (int c = 0; c < 3; ++c) {
            const float a = at(x0, y0, c) * (1 - tx) + at(x1, y0, c) * tx;
            const float b0 = at(x0, y1, c) * (1 - tx) + at(x1, y1, c) * tx;
            const float val = a * (1 - ty) + b0 * ty;
            if (c == 0) r = val;
            else if (c == 1) g = val;
            else b = val;
        }
    }

    /// Scatter ∂L/∂rgb into map (nearest for v0 stability).
    void scatterGrad(float u, float v, float dr, float dg, float db, std::vector<float>& grad) const {
        if (empty() || grad.size() != rgb.size()) return;
        u = std::clamp(u, 0.f, 1.f);
        v = std::clamp(v, 0.f, 1.f);
        const int ix = std::clamp(static_cast<int>(u * static_cast<float>(desc.width)), 0,
                                  static_cast<int>(desc.width) - 1);
        const int iy = std::clamp(static_cast<int>(v * static_cast<float>(desc.height)), 0,
                                  static_cast<int>(desc.height) - 1);
        const size_t o = (static_cast<size_t>(iy) * desc.width + static_cast<size_t>(ix)) * 3u;
        grad[o + 0] += dr;
        grad[o + 1] += dg;
        grad[o + 2] += db;
    }

    void clamp01() {
        for (float& c : rgb) c = std::clamp(c, 0.02f, 1.0f);
    }
};

} // namespace ohao::diff
