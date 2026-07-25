#pragma once

// Shared helpers for dense albedo / ORM / metal inverse fits (H1–H2).

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/scene_builder.hpp"

#include "render/diff/diff_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace ohao::inverse::dense_common {

/// Synthetic relight for the dense paths: the SAME key light scaled by this factor.
/// Not novel illumination — no env swap, no light move, no new light. See
/// docs/inverse_lab.md ("Synthetic key-light relight").
inline constexpr float kRelightKeyScale = 2.5f;

/// RAII scope that actually holds the key light at kRelightKeyScale × training
/// intensity across the forward renders inside it.
///
/// Why this is not just `keyLight->setIntensity(x * 2.5f)`: the dense forward
/// helpers call `inv.applyTruth()` on every primed/forced render, and
/// applyTheta() re-drives `keyLight->setIntensity(truthKeyI)` from the θ
/// source-of-truth (scene_builder.hpp:330-331 / :193). A live setIntensity is
/// therefore silently reverted by the very next forced forward, which is how the
/// published "relight" numbers ended up being a second *training-light* render.
/// So we scale the source of truth as well; the live set covers the
/// `fitKeyLight == false` case, where applyTheta never touches the light at all.
class RelightScope {
public:
    explicit RelightScope(InverseScene& inv, float scale = kRelightKeyScale)
        : inv_(inv), savedTruth_(inv.truthKeyI), scale_(scale) {
        inv_.truthKeyI = savedTruth_ * scale_;
        if (inv_.keyLight) {
            savedLive_ = inv_.keyLight->getIntensity();
            inv_.keyLight->setIntensity(savedLive_ * scale_);
        }
    }
    RelightScope(const RelightScope&) = delete;
    RelightScope& operator=(const RelightScope&) = delete;

    /// Call AFTER the relit forwards. Makes the "the boost got reverted" failure
    /// mode unrepresentable instead of silently publishing a duplicate metric.
    void verify(const char* what) const {
        if (!inv_.keyLight) return;
        const float want = savedLive_ * scale_;
        const float got = inv_.keyLight->getIntensity();
        if (std::abs(got - want) > 0.02f * std::max(1.0f, want)) {
            std::cerr << "FATAL: " << what << " relight did not hold — key intensity " << got
                      << " but expected " << want << " (" << scale_ << "x of " << savedLive_
                      << "). The relight metric would be a duplicate training-light render.\n";
            std::abort();
        }
    }

    ~RelightScope() {
        inv_.truthKeyI = savedTruth_;
        if (inv_.keyLight) inv_.keyLight->setIntensity(savedLive_);
    }

private:
    InverseScene& inv_;
    float savedTruth_{0.f};
    float savedLive_{1.f};
    float scale_{kRelightKeyScale};
};

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

/// Resolve FIT/SHOW viewport for dense optim (honors --fit-* / --show-* / --hd / --quality-plate).
struct DenseViewport {
    std::uint32_t fitW{256}, fitH{144};
    std::uint32_t showW{256}, showH{144};
    int frames{6};      // optim accumulation
    int showFrames{8};  // plate stills accumulation (persuasion bar)
    bool qualityPlate{false};
    [[nodiscard]] bool wantShowStills() const noexcept {
        return showW != fitW || showH != fitH || qualityPlate;
    }
};

[[nodiscard]] inline DenseViewport resolveViewport(const FitConfig& cfg) {
    DenseViewport v;
    v.qualityPlate = cfg.denseQualityPlate;
    v.fitW = std::max(256u, cfg.fit.width);
    v.fitH = std::max(144u, cfg.fit.height);
    v.showW = std::max(v.fitW, cfg.show.width);
    v.showH = std::max(v.fitH, cfg.show.height);
    // Quality plate: never starve frames at high res (old path cut frames as px grew).
    if (v.qualityPlate) {
        v.frames = std::max(8, cfg.denseFitFrames > 0 ? cfg.denseFitFrames : 8);
        v.showFrames = std::max(16, cfg.denseShowFrames > 0 ? cfg.denseShowFrames : 20);
    } else {
        const std::uint64_t px = static_cast<std::uint64_t>(v.fitW) * v.fitH;
        if (px >= 1280ull * 720ull) v.frames = 5;
        else if (px >= 640ull * 360ull) v.frames = 6;
        else v.frames = 6;
        v.showFrames = std::max(v.frames, 10);
        if (cfg.denseFitFrames > 0) v.frames = cfg.denseFitFrames;
        if (cfg.denseShowFrames > 0) v.showFrames = cfg.denseShowFrames;
    }
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

/// Product-floor scalar: soft diagonal gloss bands (less toy than hard checker).
/// Free grid still recovers well when G divides band count.
inline void fillProductScalar(ohao::diff::DiffAlbedoMap& map, int bands, float lo, float hi) {
    if (map.empty() || bands < 1) return;
    const float mid = 0.5f * (lo + hi);
    const float amp = 0.5f * (hi - lo);
    for (std::uint32_t y = 0; y < map.desc.height; ++y) {
        for (std::uint32_t x = 0; x < map.desc.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(map.desc.width);
            const float vv = (static_cast<float>(y) + 0.5f) / static_cast<float>(map.desc.height);
            // Soft band + mild radial falloff (center slightly glossier).
            const float phase = (u + vv) * static_cast<float>(bands) * 3.14159265f;
            float s = mid + amp * std::sin(phase);
            const float r = std::hypot(u - 0.5f, vv - 0.5f);
            s = std::clamp(s - 0.08f * r, lo, hi);
            const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
            map.rgb[o + 0] = s;
            map.rgb[o + 1] = s;
            map.rgb[o + 2] = s;
        }
    }
}

} // namespace ohao::inverse::dense_common
