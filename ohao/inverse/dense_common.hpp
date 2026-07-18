#pragma once

// Shared helpers for dense albedo / ORM / metal inverse fits (H1–H2).

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"

#include "render/diff/diff_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace ohao::inverse::dense_common {

inline bool saveMapPng(const ohao::diff::DiffAlbedoMap& map,
                       const std::filesystem::path& path) {
    ImageRGBA8 m;
    m.width = map.desc.width;
    m.height = map.desc.height;
    m.rgba.resize(map.pixelCount() * 4u);
    for (size_t i = 0; i < map.pixelCount(); ++i) {
        m.rgba[i * 4 + 0] =
            static_cast<uint8_t>(std::clamp(map.rgb[i * 3 + 0], 0.f, 1.f) * 255.f + 0.5f);
        m.rgba[i * 4 + 1] =
            static_cast<uint8_t>(std::clamp(map.rgb[i * 3 + 1], 0.f, 1.f) * 255.f + 0.5f);
        m.rgba[i * 4 + 2] =
            static_cast<uint8_t>(std::clamp(map.rgb[i * 3 + 2], 0.f, 1.f) * 255.f + 0.5f);
        m.rgba[i * 4 + 3] = 255;
    }
    return savePNG(m, path);
}

[[nodiscard]] inline double psnrFromMse(double mse) {
    return (mse > 1e-12) ? (-10.0 * std::log10(mse)) : 99.0;
}

/// Resolve FIT/SHOW viewport for dense optim (honors --fit-* / --show-* / --hd).
struct DenseViewport {
    std::uint32_t fitW{256}, fitH{144};
    std::uint32_t showW{256}, showH{144};
    int frames{6};
    [[nodiscard]] bool wantShowStills() const noexcept {
        return showW != fitW || showH != fitH;
    }
};

[[nodiscard]] inline DenseViewport resolveViewport(const FitConfig& cfg) {
    DenseViewport v;
    v.fitW = std::max(256u, cfg.fit.width);
    v.fitH = std::max(144u, cfg.fit.height);
    v.showW = std::max(v.fitW, cfg.show.width);
    v.showH = std::max(v.fitH, cfg.show.height);
    const std::uint64_t px = static_cast<std::uint64_t>(v.fitW) * v.fitH;
    if (px >= 1280ull * 720ull) v.frames = 4;
    else if (px >= 640ull * 360ull) v.frames = 5;
    else v.frames = 6;
    return v;
}

/// Checker pattern into all RGB channels (rough or metal scalar maps).
inline void fillCheckerScalar(ohao::diff::DiffAlbedoMap& map, int tiles, float lo, float hi) {
    if (map.empty() || tiles < 1) return;
    for (std::uint32_t y = 0; y < map.desc.height; ++y) {
        for (std::uint32_t x = 0; x < map.desc.width; ++x) {
            const int tx = static_cast<int>(x * tiles / map.desc.width);
            const int ty = static_cast<int>(y * tiles / map.desc.height);
            const float v = ((tx + ty) & 1) ? hi : lo;
            const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
            map.rgb[o + 0] = v;
            map.rgb[o + 1] = v;
            map.rgb[o + 2] = v;
        }
    }
}

} // namespace ohao::inverse::dense_common
