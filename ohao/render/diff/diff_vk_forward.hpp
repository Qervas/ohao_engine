#pragma once

// Diff-IR Vulkan forward: Deferred raster of full studio mesh.
// Beauty SoT: tile RGB → dense map → bindless albedo sampled by GBuffer.

#include "render/diff/diff_map.hpp"
#include "render/diff/diff_map_bind.hpp"

#include "gpu/vulkan/renderer.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/scene_builder.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/post_processing_pipeline.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ohao::diff {

inline void tilesIntoMap(const std::vector<double>& tileRgb, int N, DiffAlbedoMap& map) {
    if (N < 1) return;
    if (map.empty()) map.allocate(32, 32);
    for (std::uint32_t y = 0; y < map.desc.height; ++y) {
        for (std::uint32_t x = 0; x < map.desc.width; ++x) {
            const int tx = std::min(N - 1, static_cast<int>(x * N / map.desc.width));
            const int ty = std::min(N - 1, static_cast<int>(y * N / map.desc.height));
            const size_t ti = static_cast<size_t>(ty * N + tx) * 3u;
            const size_t o = (static_cast<size_t>(y) * map.desc.width + x) * 3u;
            if (ti + 2 < tileRgb.size()) {
                map.rgb[o + 0] = static_cast<float>(tileRgb[ti + 0]);
                map.rgb[o + 1] = static_cast<float>(tileRgb[ti + 1]);
                map.rgb[o + 2] = static_cast<float>(tileRgb[ti + 2]);
            }
        }
    }
}

inline void applyTilesToScene(ohao::inverse::InverseScene& inv, const std::vector<double>& tileRgb,
                              int mapGrid) {
    if (!inv.mapGround || inv.groundMats.empty()) return;
    const int N = mapGrid > 0 ? mapGrid : inv.mapRes;
    auto th = inv.truthTheta();
    for (int i = 0; i < N * N; ++i) {
        const size_t o = static_cast<size_t>(i) * 3u;
        if (o + 2 >= tileRgb.size()) break;
        th[o + 0] = tileRgb[o + 0];
        th[o + 1] = tileRgb[o + 1];
        th[o + 2] = tileRgb[o + 2];
    }
    inv.applyTheta(th);
}

[[nodiscard]] inline ohao::inverse::ImageRGBA8 forwardStudioDeferred(
    ohao::VulkanRenderer& renderer, ohao::inverse::InverseScene& inv,
    const std::vector<double>& tileRgb, int viewIndex, int frames = 16) {
    static DiffAlbedoMap s_map;
    if (s_map.empty()) s_map.allocate(32, 32);

    tilesIntoMap(tileRgb, inv.mapRes, s_map);
    applyTilesToScene(inv, tileRgb, inv.mapRes);
    // Wire bindless map (white verts + materialColors + "<actor>_albedo_0").
    (void)bindGroundAlbedoMap(renderer, inv, s_map);
    (void)renderer.updateSceneBuffers();

    inv.applyCamera(renderer.getCamera(), viewIndex);
    renderer.setRenderMode(ohao::RenderMode::Deferred);
    if (auto* def = renderer.getDeferredRenderer()) {
        if (auto* pp = def->getPostProcessing()) pp->setTAAEnabled(false);
    }
    renderer.resetAccumulation();
    const int n = std::max(14, frames);
    for (int i = 0; i < n; ++i) renderer.render();
    const auto span = renderer.getPixelSpan();
    return ohao::inverse::ImageRGBA8::fromSpan(renderer.getWidth(), renderer.getHeight(), span);
}

[[nodiscard]] inline double lossMulti(ohao::VulkanRenderer& renderer,
                                      ohao::inverse::InverseScene& inv,
                                      const std::vector<double>& tileRgb, int nViews,
                                      const std::vector<ohao::inverse::ImageRGBA8>& targets,
                                      int frames, int kAvg = 2) {
    double sum = 0.0;
    for (int a = 0; a < kAvg; ++a) {
        for (int v = 0; v < nViews; ++v) {
            auto img = forwardStudioDeferred(renderer, inv, tileRgb, v, frames);
            sum += ohao::inverse::mseRGB(img, targets[static_cast<size_t>(v)]);
        }
    }
    return sum / static_cast<double>(kAvg * nViews);
}

} // namespace ohao::diff
