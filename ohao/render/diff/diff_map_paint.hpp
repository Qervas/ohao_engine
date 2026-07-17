#pragma once

// Paint free control grids / tiles into a dense albedo map; map MSE helpers.

#include "render/diff/diff_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ohao::diff {

/// Nearest paint G×G RGB grid (row-major, 3 floats/cell) into map.
inline void gridIntoMap(const std::vector<double>& gridRgb, int G, DiffAlbedoMap& map) {
    if (G < 1 || map.empty()) return;
    for (std::uint32_t y = 0; y < map.desc.height; ++y) {
        for (std::uint32_t x = 0; x < map.desc.width; ++x) {
            const int gx = std::min(G - 1, static_cast<int>(x * G / map.desc.width));
            const int gy = std::min(G - 1, static_cast<int>(y * G / map.desc.height));
            const size_t gi = static_cast<size_t>(gy * G + gx) * 3u;
            const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
            if (gi + 2 < gridRgb.size()) {
                map.rgb[o + 0] = static_cast<float>(gridRgb[gi + 0]);
                map.rgb[o + 1] = static_cast<float>(gridRgb[gi + 1]);
                map.rgb[o + 2] = static_cast<float>(gridRgb[gi + 2]);
            }
        }
    }
}

/// Mean squared error between two maps (same size). Returns 0 if incompatible.
[[nodiscard]] inline double mapMse(const DiffAlbedoMap& a, const DiffAlbedoMap& b) {
    if (a.empty() || b.empty() || a.rgb.size() != b.rgb.size()) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const double d = static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]);
        s += d * d;
    }
    return s / static_cast<double>(a.rgb.size());
}

inline void clampGrid(std::vector<double>& grid, double lo = 0.02, double hi = 1.0) {
    for (double& c : grid) c = std::clamp(c, lo, hi);
}

} // namespace ohao::diff
