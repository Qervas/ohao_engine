#pragma once

// Diff-IR session: cameras + albedo map + forward / fit / export.

#include "render/diff/diff_camera.hpp"
#include "render/diff/diff_forward.hpp"
#include "render/diff/diff_map.hpp"
#include "render/diff/diff_optimizer.hpp"
#include "render/diff/diff_pipeline.hpp"
#include "render/diff/diff_types.hpp"

#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/scene_builder.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::diff {

class DiffSession {
public:
    DiffPipeline pipeline;
    DiffAlbedoMap map;
    DiffLight light{};
    std::vector<DiffCamera> cameras;
    std::uint32_t width{320};
    std::uint32_t height{180};

    void setupFromInverse(const ohao::inverse::InverseScene& inv, std::uint32_t w,
                          std::uint32_t h, std::uint32_t mapRes = 32) {
        width = w;
        height = h;
        map.allocate(mapRes, mapRes);
        cameras.clear();
        const float aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.777f;
        for (const auto& v : inv.views) {
            DiffCamera c;
            c.position = v.position;
            c.pitchDeg = v.pitchDeg;
            c.yawDeg = v.yawDeg;
            c.aspect = aspect;
            cameras.push_back(c);
        }
        if (cameras.empty()) {
            DiffCamera c;
            c.aspect = aspect;
            cameras.push_back(c);
        }
    }

    void mapFromTiles(const std::vector<glm::vec3>& tiles, int mapGrid) {
        if (tiles.empty() || mapGrid < 1) return;
        const int N = mapGrid;
        // Paint constant color per tile cell into dense map.
        for (std::uint32_t y = 0; y < map.desc.height; ++y) {
            for (std::uint32_t x = 0; x < map.desc.width; ++x) {
                const int tx = std::min(N - 1, static_cast<int>(x * N / map.desc.width));
                const int ty = std::min(N - 1, static_cast<int>(y * N / map.desc.height));
                const int ti = ty * N + tx;
                const auto& c = tiles[static_cast<size_t>(std::min(
                    ti, static_cast<int>(tiles.size()) - 1))];
                const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
                map.rgb[o + 0] = c.r;
                map.rgb[o + 1] = c.g;
                map.rgb[o + 2] = c.b;
            }
        }
    }

    [[nodiscard]] ohao::inverse::ImageRGBA8 forwardView(int viewIndex) const {
        const DiffCamera& cam =
            cameras[static_cast<size_t>(std::clamp(viewIndex, 0, static_cast<int>(cameras.size()) - 1))];
        auto lin = forwardLinear(map, cam, light, width, height);
        return linearToRGBA8(lin, width, height);
    }

    /// Multi-view train fit; returns final loss. writes trajectory optional.
    double fitAlbedo(const std::vector<ohao::inverse::ImageRGBA8>& trainTargets, int iters,
                     float lr = 0.08f) {
        if (trainTargets.empty()) return 1e6;
        DiffAdam adam;
        double last = 1e6;
        for (int it = 0; it < iters; ++it) {
            std::vector<float> grad(map.rgb.size(), 0.f);
            double lossSum = 0.0;
            int n = 0;
            for (size_t v = 0; v < trainTargets.size() && v < cameras.size(); ++v) {
                std::vector<float> g;
                const double L =
                    lossAndGrad(map, cameras[v], light, width, height, trainTargets[v], g);
                lossSum += L;
                ++n;
                for (size_t i = 0; i < grad.size() && i < g.size(); ++i) grad[i] += g[i];
            }
            if (n > 0) {
                for (float& g : grad) g /= static_cast<float>(n);
                lossSum /= static_cast<double>(n);
            }
            adam.step(map, grad, lr);
            last = lossSum;
            if (it % std::max(1, iters / 5) == 0 || it + 1 == iters) {
                std::cout << "  [diff] " << (it + 1) << "/" << iters << " loss=" << last << "\n";
            }
        }
        return last;
    }

    /// Average map into N×N tile colors (for PT θ apply).
    [[nodiscard]] std::vector<glm::vec3> tilesFromMap(int N) const {
        std::vector<glm::vec3> tiles(static_cast<size_t>(N * N), glm::vec3(0.45f));
        if (map.empty() || N < 1) return tiles;
        std::vector<double> acc(static_cast<size_t>(N * N) * 3u, 0.0);
        std::vector<int> cnt(static_cast<size_t>(N * N), 0);
        for (std::uint32_t y = 0; y < map.desc.height; ++y) {
            for (std::uint32_t x = 0; x < map.desc.width; ++x) {
                const int tx = std::min(N - 1, static_cast<int>(x * N / map.desc.width));
                const int ty = std::min(N - 1, static_cast<int>(y * N / map.desc.height));
                const int ti = ty * N + tx;
                const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
                acc[static_cast<size_t>(ti) * 3 + 0] += map.rgb[o + 0];
                acc[static_cast<size_t>(ti) * 3 + 1] += map.rgb[o + 1];
                acc[static_cast<size_t>(ti) * 3 + 2] += map.rgb[o + 2];
                cnt[static_cast<size_t>(ti)]++;
            }
        }
        for (int i = 0; i < N * N; ++i) {
            if (cnt[static_cast<size_t>(i)] == 0) continue;
            const double inv = 1.0 / cnt[static_cast<size_t>(i)];
            tiles[static_cast<size_t>(i)] = {
                static_cast<float>(acc[static_cast<size_t>(i) * 3 + 0] * inv),
                static_cast<float>(acc[static_cast<size_t>(i) * 3 + 1] * inv),
                static_cast<float>(acc[static_cast<size_t>(i) * 3 + 2] * inv)};
        }
        return tiles;
    }

    bool saveMapPng(const std::filesystem::path& path) const {
        ohao::inverse::ImageRGBA8 img;
        img.width = map.desc.width;
        img.height = map.desc.height;
        img.rgba.resize(map.pixelCount() * 4u);
        for (size_t i = 0; i < map.pixelCount(); ++i) {
            img.rgba[i * 4 + 0] =
                static_cast<std::uint8_t>(std::clamp(map.rgb[i * 3 + 0], 0.f, 1.f) * 255.f + 0.5f);
            img.rgba[i * 4 + 1] =
                static_cast<std::uint8_t>(std::clamp(map.rgb[i * 3 + 1], 0.f, 1.f) * 255.f + 0.5f);
            img.rgba[i * 4 + 2] =
                static_cast<std::uint8_t>(std::clamp(map.rgb[i * 3 + 2], 0.f, 1.f) * 255.f + 0.5f);
            img.rgba[i * 4 + 3] = 255;
        }
        return ohao::inverse::savePNG(img, path);
    }

    /// FD check: analytic vs finite-diff on one texel channel. Returns relative error.
    [[nodiscard]] double fdGradCheck(const ohao::inverse::ImageRGBA8& target, size_t idx,
                                     float eps = 1e-3f) const {
        if (map.rgb.empty() || idx >= map.rgb.size()) return 1e9;
        std::vector<float> g;
        (void)lossAndGrad(map, cameras[0], light, width, height, target, g);
        const float analytic = g[idx];
        DiffAlbedoMap m2 = map;
        m2.rgb[idx] += eps;
        std::vector<float> dummy;
        const double Lp = lossAndGrad(m2, cameras[0], light, width, height, target, dummy);
        m2.rgb[idx] = map.rgb[idx] - eps;
        const double Lm = lossAndGrad(m2, cameras[0], light, width, height, target, dummy);
        const double fd = (Lp - Lm) / (2.0 * static_cast<double>(eps));
        const double denom = std::max(1e-6, std::abs(fd) + std::abs(static_cast<double>(analytic)));
        return std::abs(fd - static_cast<double>(analytic)) / denom;
    }
};

} // namespace ohao::diff
