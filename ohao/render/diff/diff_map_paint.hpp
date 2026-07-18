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

/// Nearest paint G×G roughness (1 float/cell) into map (all channels = rough).
inline void gridIntoRoughMap(const std::vector<double>& gridRough, int G, DiffAlbedoMap& map) {
    if (G < 1 || map.empty()) return;
    for (std::uint32_t y = 0; y < map.desc.height; ++y) {
        for (std::uint32_t x = 0; x < map.desc.width; ++x) {
            const int gx = std::min(G - 1, static_cast<int>(x * G / map.desc.width));
            const int gy = std::min(G - 1, static_cast<int>(y * G / map.desc.height));
            const size_t gi = static_cast<size_t>(gy * G + gx);
            const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
            if (gi < gridRough.size()) {
                const float r =
                    static_cast<float>(std::clamp(gridRough[gi], 0.04, 1.0));
                map.rgb[o + 0] = r;
                map.rgb[o + 1] = r;
                map.rgb[o + 2] = r;
            }
        }
    }
}

/// Same as gridIntoRoughMap but metal clamped to [0,1] (no 0.04 floor).
inline void gridIntoMetalMap(const std::vector<double>& gridMetal, int G, DiffAlbedoMap& map) {
    if (G < 1 || map.empty()) return;
    for (std::uint32_t y = 0; y < map.desc.height; ++y) {
        for (std::uint32_t x = 0; x < map.desc.width; ++x) {
            const int gx = std::min(G - 1, static_cast<int>(x * G / map.desc.width));
            const int gy = std::min(G - 1, static_cast<int>(y * G / map.desc.height));
            const size_t gi = static_cast<size_t>(gy * G + gx);
            const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
            if (gi < gridMetal.size()) {
                const float m =
                    static_cast<float>(std::clamp(gridMetal[gi], 0.0, 1.0));
                map.rgb[o + 0] = m;
                map.rgb[o + 1] = m;
                map.rgb[o + 2] = m;
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

/// Single-channel rough MSE (uses R of each map).
[[nodiscard]] inline double roughMapMse(const DiffAlbedoMap& a, const DiffAlbedoMap& b) {
    if (a.empty() || b.empty() || a.pixelCount() != b.pixelCount()) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < a.pixelCount(); ++i) {
        const double d = static_cast<double>(a.rgb[i * 3]) - static_cast<double>(b.rgb[i * 3]);
        s += d * d;
    }
    return s / static_cast<double>(a.pixelCount());
}

/// Metallic map MSE (same layout as roughMapMse).
[[nodiscard]] inline double metalMapMse(const DiffAlbedoMap& a, const DiffAlbedoMap& b) {
    return roughMapMse(a, b);
}

inline void clampGrid(std::vector<double>& grid, double lo = 0.02, double hi = 1.0) {
    for (double& c : grid) c = std::clamp(c, lo, hi);
}

inline void clampRoughGrid(std::vector<double>& grid) {
    clampGrid(grid, 0.04, 1.0);
}

inline void clampMetalGrid(std::vector<double>& grid) { clampGrid(grid, 0.0, 1.0); }

} // namespace ohao::diff
