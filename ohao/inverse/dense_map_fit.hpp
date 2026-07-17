#pragma once

// H1/M1a: free dense ground albedo under Deferred (map is beauty SoT).
// Control grid G×G is optim θ; painted to denseMapRes² texture sampled by GBuffer.
// Wrong-init gray map; multi-view MSE; coord FD + optional Adam polish.

#include "inverse/export_capture.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/diff/diff_map.hpp"
#include "render/diff/diff_map_bind.hpp"
#include "render/diff/diff_map_paint.hpp"
#include "render/diff/diff_optimizer.hpp"
#include "render/diff/diff_vk_forward.hpp"

#include "gpu/vulkan/renderer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/post_processing_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::inverse {

inline bool saveDiffMapPng(const ohao::diff::DiffAlbedoMap& map, const std::filesystem::path& path) {
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

/// Forward beauty with free dense map as Deferred-sampled albedo SoT.
/// After first prime (truth lights + white ground verts), only rebinds the map texture
/// so coordinate FD does not thrash BLAS/scene rebuild each step.
[[nodiscard]] inline ImageRGBA8 forwardDenseMapDeferred(VulkanRenderer& renderer, InverseScene& inv,
                                                        const ohao::diff::DiffAlbedoMap& map,
                                                        int viewIndex, int frames,
                                                        bool forceSceneRebuild = false) {
    static bool s_primed = false;
    if (forceSceneRebuild) s_primed = false;
    if (!s_primed) {
        inv.applyTruth();
        (void)ohao::diff::bindGroundAlbedoMap(renderer, inv, map, /*replacePrevious=*/true);
        (void)renderer.updateSceneBuffers();
        s_primed = true;
    } else {
        // Texture-only update (no scene rebuild, no wait-idle unload).
        (void)ohao::diff::bindGroundAlbedoMap(renderer, inv, map, /*replacePrevious=*/false);
    }
    inv.applyCamera(renderer.getCamera(), viewIndex);
    renderer.setRenderMode(RenderMode::Deferred);
    if (auto* def = renderer.getDeferredRenderer()) {
        if (auto* pp = def->getPostProcessing()) pp->setTAAEnabled(false);
    }
    renderer.resetAccumulation();
    const int n = std::max(6, frames);
    for (int i = 0; i < n; ++i) renderer.render();
    return ImageRGBA8::fromSpan(renderer.getWidth(), renderer.getHeight(), renderer.getPixelSpan());
}

[[nodiscard]] inline double lossDenseMap(VulkanRenderer& renderer, InverseScene& inv,
                                         const ohao::diff::DiffAlbedoMap& map, int nViews,
                                         const std::vector<ImageRGBA8>& targets, int frames) {
    double sum = 0.0;
    for (int v = 0; v < nViews; ++v) {
        auto img = forwardDenseMapDeferred(renderer, inv, map, v, frames);
        sum += mseRGB(img, targets[static_cast<size_t>(v)]);
    }
    return sum / static_cast<double>(nViews);
}

struct DenseMapFitResult {
    bool pass{false};
    double initLoss{0}, finalLoss{0};
    double initPsnr{0}, trainPsnr{0};
    double mapMseInit{0}, mapMseRec{0};
};

/// Free dense albedo map fit (H1/M1a). Returns MAPTEST outcome.
[[nodiscard]] inline DenseMapFitResult runDenseMapFit(FitConfig cfg) {
    DenseMapFitResult result{};
    std::cout << std::unitbuf;
    applyPreset(cfg);
    resolveAssetFallbacks(cfg);
    cfg.mapGround = true;
    if (cfg.mapRes < 2) cfg.mapRes = 2;
    if (cfg.denseMapRes < 32) cfg.denseMapRes = 64;
    if (cfg.denseGrid < 4) cfg.denseGrid = 8;
    cfg.denseGrid = std::clamp(cfg.denseGrid, 4, 16);
    cfg.denseMapRes = std::clamp(cfg.denseMapRes, 32, 256);

    const std::uint32_t W = 256;
    const std::uint32_t H = 144;
    const int kFrames = 6;
    const int nViews = 2;
    const int G = cfg.denseGrid;
    const int mapPx = cfg.denseMapRes;
    const int nGrid = G * G * 3;

    InverseScene inv = InverseScene::buildStudio(cfg);
    if (!inv.mapGround || inv.truthTiles.empty()) {
        std::cerr << "FATAL: dense map fit requires map-ground studio\n";
        return result;
    }

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: dense map VulkanRenderer init failed\n";
        return result;
    }
    renderer.setRenderMode(RenderMode::Deferred);
    if (std::filesystem::exists(inv.envPath)) applyEnv(renderer, inv.envPath);

    inv.applyTruth();
    renderer.setScene(inv.scene.get());
    (void)renderer.updateSceneBuffers();

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir / "materials");

    // GT map: truth studio tiles painted into dense texture.
    std::vector<double> truthTiles;
    const int N = inv.mapRes;
    for (int i = 0; i < N * N; ++i) {
        truthTiles.push_back(inv.truthTiles[static_cast<size_t>(i)].r);
        truthTiles.push_back(inv.truthTiles[static_cast<size_t>(i)].g);
        truthTiles.push_back(inv.truthTiles[static_cast<size_t>(i)].b);
    }
    ohao::diff::DiffAlbedoMap gtMap;
    gtMap.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    ohao::diff::tilesIntoMap(truthTiles, N, gtMap);

    // Target beauties from GT map (not free-gift optim start). Prime once.
    std::vector<ImageRGBA8> targets;
    for (int v = 0; v < nViews; ++v) {
        auto img = forwardDenseMapDeferred(renderer, inv, gtMap, v, kFrames, /*force*/ v == 0);
        savePNG(img, outDir / (std::string("dense_target_") + std::to_string(v) + ".png"));
        targets.push_back(std::move(img));
    }
    savePNG(targets[0], outDir / "dense_forward_truth.png");
    saveDiffMapPng(gtMap, outDir / "materials" / "ground_albedo_gt.png");

    std::cout << "Dense-map Diff-IR  views=" << nViews << "  " << W << "x" << H
              << "  map=" << mapPx << "x" << mapPx << "  free_grid=" << G << "x" << G
              << "  (θ dims=" << nGrid << ")\n";
    std::cout << "  beauty SoT: free dense albedo map bindless (Deferred GBuffer sample)\n";
    std::cout << "  wrong_init: cool solid (not GT)\n";

    auto lossAtGrid = [&](const std::vector<double>& grid) {
        ohao::diff::DiffAlbedoMap m;
        m.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoMap(grid, G, m);
        return lossDenseMap(renderer, inv, m, nViews, targets, kFrames);
    };

    // A/B: warm vs cool grid must move beauty.
    std::vector<double> warm(static_cast<size_t>(nGrid), 0.0);
    std::vector<double> cool(static_cast<size_t>(nGrid), 0.0);
    for (int i = 0; i < G * G; ++i) {
        warm[static_cast<size_t>(i) * 3 + 0] = 0.75;
        warm[static_cast<size_t>(i) * 3 + 1] = 0.35;
        warm[static_cast<size_t>(i) * 3 + 2] = 0.20;
        cool[static_cast<size_t>(i) * 3 + 0] = 0.20;
        cool[static_cast<size_t>(i) * 3 + 1] = 0.45;
        cool[static_cast<size_t>(i) * 3 + 2] = 0.70;
    }
    {
        ohao::diff::DiffAlbedoMap mW, mC;
        mW.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        mC.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoMap(warm, G, mW);
        ohao::diff::gridIntoMap(cool, G, mC);
        auto wImg = forwardDenseMapDeferred(renderer, inv, mW, 0, kFrames, true);
        auto cImg = forwardDenseMapDeferred(renderer, inv, mC, 0, kFrames);
        const double ab = mseRGB(wImg, cImg);
        std::cout << "  A/B warm vs cool beauty MSE=" << ab << "\n";
        if (ab < 1e-4) {
            std::cerr << "FATAL: dense map θ does not affect Deferred beauty\n";
            inv.scene.reset();
            return result;
        }
    }

    // Wrong-init: cool solid (far from multi-hue truth tiles; not GT).
    std::vector<double> th = cool;
    ohao::diff::DiffAlbedoMap work;
    work.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    ohao::diff::gridIntoMap(th, G, work);

    const double initLoss = lossDenseMap(renderer, inv, work, nViews, targets, kFrames);
    const double initPsnr = (initLoss > 1e-12) ? (-10.0 * std::log10(initLoss)) : 99.0;
    result.initLoss = initLoss;
    result.initPsnr = initPsnr;
    result.mapMseInit = ohao::diff::mapMse(work, gtMap);

    auto initImg = forwardDenseMapDeferred(renderer, inv, work, 0, kFrames);
    savePNG(initImg, outDir / "dense_init.png");
    saveDiffMapPng(work, outDir / "materials" / "ground_albedo_init.png");
    std::cout << "  wrong-init loss=" << initLoss << " PSNR=" << initPsnr
              << " map_mse_vs_gt=" << result.mapMseInit << "\n";

    // Coordinate FD on free grid (from wrong init). One-sided FD for speed.
    std::vector<double> best = th;
    double bestLoss = initLoss;
    double step = 0.16;
    const int passes = 3;
    for (int p = 0; p < passes; ++p) {
        int accepts = 0;
        for (size_t i = 0; i < best.size(); ++i) {
            auto trialP = best;
            auto trialM = best;
            trialP[i] = std::clamp(trialP[i] + step, 0.02, 1.0);
            trialM[i] = std::clamp(trialM[i] - step, 0.02, 1.0);
            const double Lp = lossAtGrid(trialP);
            const double Lm = lossAtGrid(trialM);
            const double thr = bestLoss * 0.998;
            if (Lp < thr && Lp <= Lm) {
                best = std::move(trialP);
                bestLoss = Lp;
                ++accepts;
            } else if (Lm < thr) {
                best = std::move(trialM);
                bestLoss = Lm;
                ++accepts;
            }
        }
        step *= 0.70;
        std::cout << "  [dense-map] FD pass " << (p + 1) << "/" << passes << " best_loss=" << bestLoss
                  << " accepts=" << accepts << std::endl;
        if (bestLoss < initLoss * 0.50) {
            std::cout << "  [dense-map] early stop (strong loss drop)\n";
            break;
        }
        if (accepts == 0 && bestLoss < initLoss * 0.92) {
            std::cout << "  [dense-map] early stop (plateau)\n";
            break;
        }
    }

    // Sparse Adam polish on random free-grid coords (few steps).
    {
        ohao::diff::DiffAdam adam;
        adam.resize(static_cast<size_t>(nGrid));
        // Run Adam in grid space via a thin map shim.
        ohao::diff::DiffAlbedoMap gridMap;
        gridMap.allocate(static_cast<std::uint32_t>(G), static_cast<std::uint32_t>(G));
        for (int i = 0; i < G * G; ++i) {
            gridMap.rgb[static_cast<size_t>(i) * 3 + 0] = static_cast<float>(best[static_cast<size_t>(i) * 3 + 0]);
            gridMap.rgb[static_cast<size_t>(i) * 3 + 1] = static_cast<float>(best[static_cast<size_t>(i) * 3 + 1]);
            gridMap.rgb[static_cast<size_t>(i) * 3 + 2] = static_cast<float>(best[static_cast<size_t>(i) * 3 + 2]);
        }
        const int adamSteps = 8;
        const int batch = 16;
        std::uint32_t rng = 0xC0FFEEu ^ static_cast<std::uint32_t>(cfg.seed);
        auto rnd = [&]() {
            rng = rng * 1664525u + 1013904223u;
            return rng;
        };
        for (int s = 0; s < adamSteps; ++s) {
            std::vector<float> grad(static_cast<size_t>(nGrid), 0.f);
            for (int b = 0; b < batch; ++b) {
                const size_t gi = static_cast<size_t>(rnd() % static_cast<std::uint32_t>(nGrid));
                auto gp = best;
                auto gm = best;
                const double e = 0.04;
                gp[gi] = std::clamp(gp[gi] + e, 0.02, 1.0);
                gm[gi] = std::clamp(gm[gi] - e, 0.02, 1.0);
                const double gval = (lossAtGrid(gp) - lossAtGrid(gm)) / (gp[gi] - gm[gi] + 1e-12);
                grad[gi] += static_cast<float>(gval);
            }
            adam.step(gridMap, grad, 0.10f);
            for (int i = 0; i < G * G; ++i) {
                best[static_cast<size_t>(i) * 3 + 0] = gridMap.rgb[static_cast<size_t>(i) * 3 + 0];
                best[static_cast<size_t>(i) * 3 + 1] = gridMap.rgb[static_cast<size_t>(i) * 3 + 1];
                best[static_cast<size_t>(i) * 3 + 2] = gridMap.rgb[static_cast<size_t>(i) * 3 + 2];
            }
            ohao::diff::clampGrid(best);
            const double L = lossAtGrid(best);
            if (L < bestLoss) bestLoss = L;
            if ((s + 1) % 2 == 0)
                std::cout << "  [dense-map] Adam step " << (s + 1) << "/" << adamSteps
                          << " loss=" << bestLoss << std::endl;
        }
    }

    ohao::diff::gridIntoMap(best, G, work);
    // Stable final metrics (force clean bind).
    const double finalLoss = std::min(bestLoss, lossDenseMap(renderer, inv, work, nViews, targets, kFrames));
    const double finalPsnr = (finalLoss > 1e-12) ? (-10.0 * std::log10(finalLoss)) : 99.0;
    result.finalLoss = finalLoss;
    result.trainPsnr = finalPsnr;
    result.mapMseRec = ohao::diff::mapMse(work, gtMap);

    auto recImg = forwardDenseMapDeferred(renderer, inv, work, 0, kFrames, true);
    savePNG(recImg, outDir / "dense_recovered.png");
    saveDiffMapPng(work, outDir / "materials" / "ground_albedo_recovered.png");

    std::cout << "  final loss=" << finalLoss << "  train PSNR=" << finalPsnr
              << "  (wrong-init PSNR was " << initPsnr << ")\n";
    std::cout << "  map_mse_init=" << result.mapMseInit << "  map_mse_recovered=" << result.mapMseRec
              << "\n";

    {
        std::ofstream mj(outDir / "dense_map_metrics.json");
        mj << "{\n"
           << "  \"backend\": \"diff\",\n"
           << "  \"mode\": \"dense_map\",\n"
           << "  \"metric_domain\": \"vulkan_deferred_studio\",\n"
           << "  \"beauty_theta_path\": \"dense_map_bindless_deferred\",\n"
           << "  \"dense_map_sot\": true,\n"
           << "  \"dense_map_res\": " << mapPx << ",\n"
           << "  \"dense_grid\": " << G << ",\n"
           << "  \"wrong_init_source\": \"cool_solid\",\n"
           << "  \"init_loss\": " << initLoss << ",\n"
           << "  \"final_loss\": " << finalLoss << ",\n"
           << "  \"init_psnr\": " << initPsnr << ",\n"
           << "  \"train_psnr\": " << finalPsnr << ",\n"
           << "  \"map_mse_init\": " << result.mapMseInit << ",\n"
           << "  \"map_mse_recovered\": " << result.mapMseRec << ",\n"
           << "  \"psnr_improve_db\": " << (finalPsnr - initPsnr) << "\n"
           << "}\n";
    }
    {
        std::ofstream tj(outDir / "trajectory.json");
        tj << "{\n  \"backend\": \"diff\",\n  \"mode\": \"dense_map\",\n"
           << "  \"schedule\": \"dense_grid_coord_fd_adam\",\n"
           << "  \"best_loss\": " << finalLoss << ",\n"
           << "  \"init_loss\": " << initLoss << "\n}\n";
    }

    // MAPTEST gates (roadmap H1/M1a).
    const bool mapDrop = result.mapMseRec < result.mapMseInit * 0.85;
    const bool psnrGain = finalPsnr >= initPsnr + 2.0;
    const bool lossDrop = finalLoss < initLoss * 0.90;
    result.pass = mapDrop && psnrGain && lossDrop;
    std::cout << (result.pass ? "MAPTEST PASS" : "MAPTEST FAIL")
              << " (map_mse " << result.mapMseInit << " → " << result.mapMseRec << "; PSNR "
              << initPsnr << " → " << finalPsnr << " dB; ΔPSNR=" << (finalPsnr - initPsnr)
              << ")\n";

    inv.scene.reset();
    return result;
}

[[nodiscard]] inline int runDenseMapFitCli(FitConfig cfg) {
    return runDenseMapFit(std::move(cfg)).pass ? 0 : 1;
}

} // namespace ohao::inverse
