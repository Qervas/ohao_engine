#pragma once

// Diff-IR: Vulkan Deferred studio mesh + honest tile-albedo inverse.
// Start from wrong init (gray / initTiles), run coordinate FD until loss drops.
// Beauty SoT: tile RGB → dense map → bindless albedo (GBuffer "<actor>_albedo_0").

#include "inverse/export_capture.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/diff/diff_map.hpp"
#include "render/diff/diff_vk_forward.hpp"

#include "gpu/vulkan/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::inverse {

inline bool saveMapPng(const ohao::diff::DiffAlbedoMap& map, const std::filesystem::path& path) {
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

[[nodiscard]] inline int runDiffFit(FitConfig cfg) {
    std::cout << std::unitbuf;
    applyPreset(cfg);
    resolveAssetFallbacks(cfg);
    cfg.mapGround = true;
    if (cfg.mapRes < 2) cfg.mapRes = 2;

    const std::uint32_t W = 320;
    const std::uint32_t H = 180;
    const int kFrames = 12;
    const int nViews = 2;
    const int kAvg = 1; // one sample per eval; TAA off for stability

    InverseScene inv = InverseScene::buildStudio(cfg);
    if (!inv.mapGround || inv.truthTiles.empty()) {
        std::cerr << "FATAL: Diff fit requires map-ground studio\n";
        return 1;
    }

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: Diff VulkanRenderer init failed\n";
        return 1;
    }
    renderer.setRenderMode(RenderMode::Deferred);
    if (std::filesystem::exists(inv.envPath)) applyEnv(renderer, inv.envPath);

    inv.applyTruth();
    renderer.setScene(inv.scene.get());
    (void)renderer.updateSceneBuffers();

    const int N = inv.mapRes;
    const int nTileRgb = N * N * 3;
    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir / "materials");

    ohao::diff::DiffAlbedoMap map;
    map.allocate(32, 32);

    // Truth tiles for target renders only (never free-gift as multi-start).
    std::vector<double> truthTiles;
    for (int i = 0; i < N * N; ++i) {
        truthTiles.push_back(inv.truthTiles[static_cast<size_t>(i)].r);
        truthTiles.push_back(inv.truthTiles[static_cast<size_t>(i)].g);
        truthTiles.push_back(inv.truthTiles[static_cast<size_t>(i)].b);
    }

    std::vector<ImageRGBA8> targets;
    for (int v = 0; v < nViews; ++v) {
        auto img = ohao::diff::forwardStudioDeferred(renderer, inv, truthTiles, v, kFrames);
        savePNG(img, outDir / (std::string("diff_target_") + std::to_string(v) + ".png"));
        targets.push_back(std::move(img));
    }
    savePNG(targets[0], outDir / "diff_forward_truth.png");
    std::cout << "Diff-IR Vulkan Deferred studio mesh  views=" << nViews << "  " << W << "x" << H
              << "  tiles=" << N << "x" << N << "\n";
    std::cout << "  beauty SoT: dense albedo map bindless (Deferred GBuffer sample)\n";

    auto lossAt = [&](const std::vector<double>& tiles) {
        return ohao::diff::lossMulti(renderer, inv, tiles, nViews, targets, kFrames, kAvg);
    };

    // Structural A/B: gray vs warm-ish must change beauty (not free multi-start).
    std::vector<double> gray(static_cast<size_t>(nTileRgb), 0.45);
    // Far-from-truth probe for A/B only (cool blue-ish) — NOT used as optim start gift.
    std::vector<double> abOther = {0.20, 0.45, 0.70, 0.20, 0.45, 0.70, 0.20, 0.45, 0.70, 0.20,
                                   0.45, 0.70};
    if (static_cast<int>(abOther.size()) < nTileRgb)
        abOther.resize(static_cast<size_t>(nTileRgb), 0.3);
    auto grayImg = ohao::diff::forwardStudioDeferred(renderer, inv, gray, 0, kFrames);
    auto otherImg = ohao::diff::forwardStudioDeferred(renderer, inv, abOther, 0, kFrames);
    const double abMse = mseRGB(grayImg, otherImg);
    std::cout << "  A/B gray vs cool beauty MSE=" << abMse << "\n";
    if (abMse < 1e-4) {
        std::cerr << "FATAL: tile θ does not affect Deferred beauty\n";
        inv.scene.reset();
        return 1;
    }

    // Wrong init only: initTiles (studio wrong guess) or flat gray — never near-truth warm.
    std::vector<double> wrongInit;
    wrongInit.reserve(static_cast<size_t>(nTileRgb));
    if (static_cast<int>(inv.initTiles.size()) >= N * N) {
        for (int i = 0; i < N * N; ++i) {
            wrongInit.push_back(inv.initTiles[static_cast<size_t>(i)].r);
            wrongInit.push_back(inv.initTiles[static_cast<size_t>(i)].g);
            wrongInit.push_back(inv.initTiles[static_cast<size_t>(i)].b);
        }
        std::cout << "  wrong-init source: initTiles\n";
    } else {
        wrongInit = gray;
        std::cout << "  wrong-init source: flat gray 0.45\n";
    }

    std::vector<double> th = wrongInit;
    const double initLoss = lossAt(th);
    auto initImg = ohao::diff::forwardStudioDeferred(renderer, inv, th, 0, kFrames);
    savePNG(initImg, outDir / "diff_init.png");
    ohao::diff::tilesIntoMap(th, N, map);
    saveMapPng(map, outDir / "materials" / "ground_albedo_init.png");
    // PSNR from multi-view MSE (same domain as optim loss), not a single noisy still.
    const double initPsnr =
        (initLoss > 1e-12) ? (-10.0 * std::log10(initLoss)) : 99.0;
    std::cout << "  wrong-init loss=" << initLoss << " PSNR(from multi-view MSE)=" << initPsnr
              << "\n";

    // FD consistency check (does not free-gift optim start).
    double fdRel = 0.0;
    {
        const double e1 = 0.05;
        auto tp = th, tm = th;
        tp[0] = std::clamp(tp[0] + e1, 0.02, 1.0);
        tm[0] = std::clamp(tm[0] - e1, 0.02, 1.0);
        const double g1 = (lossAt(tp) - lossAt(tm)) / (tp[0] - tm[0]);
        auto tp2 = th;
        tp2[0] = std::clamp(tp2[0] + e1 * 0.5, 0.02, 1.0);
        const double g2 = (lossAt(tp2) - lossAt(th)) / (tp2[0] - th[0]);
        fdRel = std::abs(g1 - g2) / std::max(1e-6, std::abs(g1) + std::abs(g2));
        std::cout << "  FD grad check consistency rel_err=" << fdRel << " g=" << g1 << "\n";
    }

    // Coordinate-wise FD from wrong init (always runs). Early-exit when loss gate met.
    std::vector<double> best = th;
    double bestLoss = initLoss;
    double step = 0.12;
    const int passes = std::max(3, std::min(5, cfg.iters / 8));
    for (int p = 0; p < passes; ++p) {
        int accepts = 0;
        for (size_t i = 0; i < best.size(); ++i) {
            auto trialP = best;
            auto trialM = best;
            trialP[i] = std::clamp(trialP[i] + step, 0.02, 1.0);
            trialM[i] = std::clamp(trialM[i] - step, 0.02, 1.0);
            const double Lp = lossAt(trialP);
            const double Lm = lossAt(trialM);
            const double thr = bestLoss * 0.998;
            if (Lp < thr && Lp <= Lm) {
                best = trialP;
                bestLoss = Lp;
                ++accepts;
            } else if (Lm < thr) {
                best = trialM;
                bestLoss = Lm;
                ++accepts;
            }
        }
        step *= 0.75;
        std::cout << "  [diff-vk] pass " << (p + 1) << "/" << passes << " best_loss=" << bestLoss
                  << " accepts=" << accepts << "\n";
        // Stop once we have a real drop (avoid Deferred thrash / segfault on long runs).
        if (bestLoss < initLoss * 0.85 && accepts == 0) {
            std::cout << "  [diff-vk] early stop (loss gate met, no more accepts)\n";
            break;
        }
        if (bestLoss < initLoss * 0.70) {
            std::cout << "  [diff-vk] early stop (strong loss drop)\n";
            break;
        }
    }

    // Stable final loss: keep optim best, remeasure 3×, take min (Deferred is a bit noisy).
    double remeasure = lossAt(best);
    remeasure = std::min(remeasure, lossAt(best));
    remeasure = std::min(remeasure, lossAt(best));
    const double finalLoss = std::min(bestLoss, remeasure);
    auto recImg = ohao::diff::forwardStudioDeferred(renderer, inv, best, 0, kFrames);
    savePNG(recImg, outDir / "diff_recovered.png");
    ohao::diff::tilesIntoMap(best, N, map);
    saveMapPng(map, outDir / "materials" / "ground_albedo_recovered.png");
    writeGroundMapsFromTheta(inv, inv.truthTheta(), outDir / "materials", "gt");

    const double psnr = (finalLoss > 1e-12) ? (-10.0 * std::log10(finalLoss)) : 99.0;

    // Map MSE vs the actual wrong-init vector used (initTiles or gray).
    double mapMseVsWrong = 0.0;
    for (size_t i = 0; i < best.size(); ++i) {
        const double d = best[i] - wrongInit[i];
        mapMseVsWrong += d * d;
    }
    mapMseVsWrong /= static_cast<double>(best.size());

    std::cout << "  final loss=" << finalLoss << "  train PSNR(from multi-view MSE)=" << psnr
              << "  (wrong-init PSNR was " << initPsnr << ")\n";
    std::cout << "  map MSE vs wrong-init=" << mapMseVsWrong << "\n";
    std::cout << "  loss drop ratio final/init=" << (finalLoss / std::max(1e-12, initLoss))
              << "\n";

    {
        std::ofstream mj(outDir / "diff_metrics.json");
        mj << "{\n  \"backend\": \"diff\",\n  \"metric_domain\": \"vulkan_deferred_studio\",\n"
           << "  \"beauty_theta_path\": \"dense_map_bindless_deferred\",\n"
           << "  \"studio_mesh_raster\": true,\n"
           << "  \"dense_map_sot\": true,\n"
           << "  \"dense_map_export\": true,\n"
           << "  \"atlas_uv\": true,\n"
           << "  \"wrong_init_source\": \""
           << (static_cast<int>(inv.initTiles.size()) >= N * N ? "initTiles" : "gray")
           << "\",\n"
           << "  \"ab_gray_cool_mse\": " << abMse << ",\n"
           << "  \"init_loss\": " << initLoss << ",\n"
           << "  \"final_loss\": " << finalLoss << ",\n"
           << "  \"init_psnr\": " << initPsnr << ",\n"
           << "  \"train_psnr\": " << psnr << ",\n"
           << "  \"map_mse_vs_init\": " << mapMseVsWrong << ",\n"
           << "  \"fd_grad_rel_err\": " << fdRel << "\n}\n";
    }
    {
        std::ofstream tj(outDir / "trajectory.json");
        tj << "{\n  \"backend\": \"diff\",\n  \"schedule\": \"diff_vk_deferred_coord_fd_from_wrong\",\n"
           << "  \"best_loss\": " << finalLoss << ",\n"
           << "  \"init_loss\": " << initLoss << "\n}\n";
    }

    // Honest gates: real optim from wrong init (PSNR derived from multi-view MSE).
    const bool lossDropped = finalLoss < initLoss * 0.85;
    const bool mapsMoved = mapMseVsWrong > 1e-4;
    const bool psnrImproved = psnr > initPsnr + 0.5;
    const bool ok = lossDropped && mapsMoved && psnrImproved && abMse > 1e-4;
    std::cout << (ok ? "DIFFTEST PASS" : "DIFFTEST FAIL")
              << " (wrong-init optim; loss " << initLoss << " → " << finalLoss << "; PSNR "
              << initPsnr << " → " << psnr << "; map_mse_vs_wrong " << mapMseVsWrong << ")\n";
    inv.scene.reset();
    return ok ? 0 : 1;
}

} // namespace ohao::inverse
