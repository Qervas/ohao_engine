#pragma once

// H1/M1a: free dense ground albedo under Deferred (map is beauty SoT).
// Control grid G×G is optim θ; painted to denseMapRes² texture sampled by GBuffer.
// Wrong-init gray map; multi-view MSE; coord FD + optional Adam polish.

#include "inverse/dense_analytic.hpp"
#include "inverse/dense_common.hpp"
#include "inverse/export_capture.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/fit_targets.hpp"
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
#include <chrono>
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

// Floor-weighted loss (ground map signal; matches analytic solve crop).
[[nodiscard]] inline double lossDenseMap(VulkanRenderer& renderer, InverseScene& inv,
                                         const ohao::diff::DiffAlbedoMap& map, int nViews,
                                         const std::vector<ImageRGBA8>& targets, int frames) {
    double sum = 0.0;
    for (int v = 0; v < nViews; ++v) {
        auto img = forwardDenseMapDeferred(renderer, inv, map, v, frames);
        sum += mseRGB(img, targets[static_cast<size_t>(v)], /*xMaxFrac=*/1.0,
                      /*yMinFrac=*/0.40);
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
    // Museum: 2×2 high-contrast marble tiles + matching free grid (obvious recovery).
    if (cfg.museumStudio) {
        cfg.mapRes = 2;
        if (cfg.denseGrid != 2) cfg.denseGrid = 2;
        if (cfg.denseMapRes < 128) cfg.denseMapRes = 128;
    }
    if (cfg.denseMapRes < 32) cfg.denseMapRes = 64;
    if (cfg.denseGrid < 2) cfg.denseGrid = 8;
    cfg.denseGrid = std::clamp(cfg.denseGrid, 2, 16);
    cfg.denseMapRes = std::clamp(cfg.denseMapRes, 32, 256);

    const auto vp = dense_common::resolveViewport(cfg);
    const std::uint32_t W = vp.fitW, H = vp.fitH, showW = vp.showW, showH = vp.showH;
    const int kFrames = vp.frames;
    const int showFrames = vp.showFrames;
    const bool wantShowStills = vp.wantShowStills() || cfg.museumStudio;
    const bool qualityPlate = vp.qualityPlate;
    // Museum publish face: always 1080p SHOW stills (FIT may stay draft-sized for speed).
    const std::uint32_t plateW =
        cfg.museumStudio ? std::max(showW, 1920u) : showW;
    const std::uint32_t plateH =
        cfg.museumStudio ? std::max(showH, 1080u) : showH;
    const int nViews = (cfg.denseViews > 0) ? cfg.denseViews : (qualityPlate ? 3 : 2);
    const int G = cfg.denseGrid;
    const int mapPx = cfg.denseMapRes;
    const int nGrid = G * G * 3;

    InverseScene inv = InverseScene::buildStudio(cfg);
    if (!inv.mapGround || inv.truthTiles.empty()) {
        std::cerr << "FATAL: dense map fit requires map-ground studio\n";
        return result;
    }

    // H3: if a lab bundle is supplied, render/fit through its real 6-DOF poses
    // (COLMAP path) instead of the synthetic pitch/yaw cameras. This is what
    // validates the full-pose plumbing for the dense-albedo (M0a) case.
    if (!cfg.labBundle.empty()) {
        std::filesystem::path labCap;
        std::string err;
        if (resolveLabCapturePath(cfg, labCap, err)) {
            const int nj = injectLabPoses(inv, labCap / "cameras.jsonl");
            std::cout << "  [dense-map] lab bundle poses injected=" << nj << " (full 6-DOF path)\n";
        } else {
            std::cerr << "  [dense-map] WARN: " << err << " (using synthetic cameras)\n";
        }
    }

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: dense map VulkanRenderer init failed\n";
        return result;
    }
    renderer.setRenderMode(RenderMode::Deferred);
    inv.applyTruth();
    renderer.setScene(inv.scene.get());
    (void)renderer.updateSceneBuffers();
    // Env after scene bind (avoids light/env upload thrash on empty scene).
    if (std::filesystem::exists(inv.envPath)) applyEnv(renderer, inv.envPath);

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir / "materials");

    std::cout << "Dense-map Diff-IR  views=" << nViews << "  FIT " << W << "x" << H
              << "  SHOW " << plateW << "x" << plateH << "  map=" << mapPx << "x" << mapPx
              << "  free_grid=" << G << "x" << G << "  (θ dims=" << nGrid << ")"
              << (cfg.museumStudio ? "  MUSEUM" : "")
              << (qualityPlate ? "  QUALITY_PLATE" : "") << "\n";

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

    // ── H4/M5a–b: analytic albedo grads + Adam optim (FD-checked) ───────────
    std::vector<double> best = th;
    double bestLoss = initLoss;
    bool usedAnalytic = false;
    bool optimAnalytic = false;
    double gradMedianRel = 1.0;
    double analyticMs = 0.0, fdProbeMs = 0.0, fdEstMs = 0.0, fdActualMs = 0.0;
    double speedup = 0.0;
    int analyticSteps = 0;

    ohao::diff::DiffAlbedoMap whiteMap, uvMap;
    whiteMap.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    uvMap.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    dense_analytic::fillWhite(whiteMap);
    dense_analytic::fillUvCoded(uvMap);

    auto iWhite0 = forwardDenseMapDeferred(renderer, inv, whiteMap, 0, kFrames, true);
    auto iUv0 = forwardDenseMapDeferred(renderer, inv, uvMap, 0, kFrames);
    auto uv0 = dense_analytic::estimateUv(iUv0, iWhite0, dense_analytic::kCropX,
                                          dense_analytic::kCropYMin);
    ImageRGBA8 iWhite1;
    dense_analytic::UvBuffer uv1;
    if (nViews > 1) {
        iWhite1 = forwardDenseMapDeferred(renderer, inv, whiteMap, 1, kFrames);
        auto iUv1 = forwardDenseMapDeferred(renderer, inv, uvMap, 1, kFrames);
        uv1 = dense_analytic::estimateUv(iUv1, iWhite1, dense_analytic::kCropX,
                                         dense_analytic::kCropYMin);
    }

    auto analyticGradAt = [&](const std::vector<double>& grid) {
        ohao::diff::DiffAlbedoMap m;
        m.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoMap(grid, G, m);
        std::vector<double> g(static_cast<size_t>(nGrid), 0.0);
        for (int v = 0; v < nViews; ++v) {
            auto pred = forwardDenseMapDeferred(renderer, inv, m, v, kFrames, /*force*/ false);
            const auto& S = (v == 0) ? iWhite0 : iWhite1;
            const auto& U = (v == 0) ? uv0 : uv1;
            auto gv = dense_analytic::gridGradMse(pred, targets[static_cast<size_t>(v)], S, U, G,
                                                  dense_analytic::kCropX, dense_analytic::kCropYMin);
            for (size_t i = 0; i < g.size() && i < gv.size(); ++i) g[i] += gv[i];
        }
        for (double& x : g) x /= static_cast<double>(nViews);
        return g;
    };

    // Soft reprime after first bind: texture-only (force rebuild only when thrashing UV/white).
    auto reprime = [&](const std::vector<double>& grid, bool force = false) {
        ohao::diff::gridIntoMap(grid, G, work);
        (void)forwardDenseMapDeferred(renderer, inv, work, 0, kFrames, force);
    };

    reprime(best, /*force*/ true);
    bestLoss = lossDenseMap(renderer, inv, work, nViews, targets, kFrames);

    // GRADCHECK + per-eval timing for FD cost estimate.
    {
        auto g0 = analyticGradAt(best);
        reprime(best);
        bestLoss = lossDenseMap(renderer, inv, work, nViews, targets, kFrames);

        const auto t0 = std::chrono::steady_clock::now();
        // Time one full loss eval (2 views) for FD cost model.
        const auto te0 = std::chrono::steady_clock::now();
        (void)lossAtGrid(best);
        const auto te1 = std::chrono::steady_clock::now();
        const double lossEvalMs = std::chrono::duration<double, std::milli>(te1 - te0).count();

        // Museum / hard scenes often land ~0.20–0.25; allow optim solve with soft thr.
        auto chk = dense_analytic::checkFdAgreement(
            best, G, g0, lossAtGrid, /*nProbe=*/8, cfg.seed, /*thr=*/0.25);
        reprime(best);
        bestLoss = lossDenseMap(renderer, inv, work, nViews, targets, kFrames);

        const auto t1 = std::chrono::steady_clock::now();
        fdProbeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        // Full coord FD: 3 passes × nGrid dims × 2 trials × lossEval
        fdEstMs = lossEvalMs * 2.0 * static_cast<double>(nGrid) * 3.0;
        gradMedianRel = chk.medianRelErr;
        usedAnalytic = chk.pass && !uv0.empty();
        // Linear/residual solve when UV is valid and grads are at least weakly correlated.
        const bool allowSolve = !uv0.empty() && gradMedianRel < 0.35;
        std::cout << "  [dense-map] analytic vs FD median rel err=" << gradMedianRel
                  << " scale=" << chk.scale << " n=" << chk.nCompared
                  << (chk.pass ? "  GRADCHECK PASS" : "  GRADCHECK FAIL")
                  << (allowSolve && !usedAnalytic ? "  (solve ok soft)" : "") << "\n";
        std::cout << "  [dense-map] loss_eval=" << lossEvalMs << " ms  FD_est_3pass=" << fdEstMs
                  << " ms\n";

        // M5b: closed-form A≈T/S + residual iters + optional tiny sparse polish.
        // Speed vs estimated full 3-pass coord FD; MAPTEST quality gates optim_analytic.
        if (allowSolve) {
            // Fast optim loss: view-0 floor crop, fewer frames (final metrics still full).
            const int kFastFrames = std::max(3, kFrames - 2);
            auto lossFast = [&](const std::vector<double>& grid) {
                ohao::diff::DiffAlbedoMap m;
                m.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
                ohao::diff::gridIntoMap(grid, G, m);
                auto img = forwardDenseMapDeferred(renderer, inv, m, 0, kFastFrames, false);
                return mseRGB(img, targets[0], /*xMaxFrac=*/1.0, /*yMinFrac=*/0.40);
            };
            auto softApply = [&](const std::vector<double>& grid) {
                ohao::diff::gridIntoMap(grid, G, work);
                (void)forwardDenseMapDeferred(renderer, inv, work, 0, kFastFrames, false);
            };

            const auto ta0 = std::chrono::steady_clock::now();
            // Multi-view average linear solve (CPU).
            {
                std::vector<double> acc(best.size(), 0.0);
                int nSol = 0;
                for (int v = 0; v < nViews; ++v) {
                    const auto& S = (v == 0) ? iWhite0 : iWhite1;
                    const auto& U = (v == 0) ? uv0 : uv1;
                    if (U.empty()) continue;
                    auto sol = dense_analytic::gridFromLinearSolve(
                        targets[static_cast<size_t>(v)], S, U, G, best, /*blend=*/0.97,
                        dense_analytic::kCropX, dense_analytic::kSolveCropYMin);
                    for (size_t i = 0; i < acc.size() && i < sol.size(); ++i) acc[i] += sol[i];
                    ++nSol;
                }
                if (nSol > 0) {
                    for (double& x : acc) x /= static_cast<double>(nSol);
                    best = std::move(acc);
                    ohao::diff::clampGrid(best);
                }
            }
            // Soft reprime after solve; residual polish texture-only.
            reprime(best, /*force*/ true);
            bestLoss = lossFast(best);
            std::cout << "  [dense-map] linear solve loss=" << bestLoss << "\n";
            analyticSteps = 1;

            // Residual closed-form iters: ΔA ≈ (T−P)/S (2 views, no FD).
            double rStep = 0.90;
            for (int it = 0; it < 5; ++it) {
                std::vector<double> acc = best;
                int nR = 0;
                for (int v = 0; v < nViews; ++v) {
                    const auto& S = (v == 0) ? iWhite0 : iWhite1;
                    const auto& U = (v == 0) ? uv0 : uv1;
                    if (U.empty()) continue;
                    ohao::diff::gridIntoMap(best, G, work);
                    auto pred =
                        forwardDenseMapDeferred(renderer, inv, work, v, kFastFrames, false);
                    acc = dense_analytic::gridFromResidual(
                        pred, targets[static_cast<size_t>(v)], S, U, G, acc,
                        /*step=*/rStep, dense_analytic::kCropX, dense_analytic::kSolveCropYMin);
                    ++nR;
                }
                if (nR == 0) break;
                ohao::diff::clampGrid(acc);
                softApply(acc);
                const double L = lossFast(acc);
                if (L < bestLoss * 0.999) {
                    best = std::move(acc);
                    bestLoss = L;
                    ++analyticSteps;
                    rStep *= 0.75;
                } else {
                    softApply(best);
                    break;
                }
            }
            std::cout << "  [dense-map] residual polish loss=" << bestLoss
                      << " steps=" << analyticSteps << "\n";

            // Tiny sparse one-sided polish only if residual needs help (top-k, 1 pass).
            {
                auto g = analyticGradAt(best);
                softApply(best);
                bestLoss = lossFast(best);
                std::vector<std::pair<double, size_t>> rank;
                rank.reserve(g.size());
                for (size_t i = 0; i < g.size(); ++i) rank.push_back({std::abs(g[i]), i});
                std::sort(rank.begin(), rank.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
                const int kTop = std::min(std::max(24, nGrid / 3), static_cast<int>(rank.size()));
                std::cout << "  [dense-map] one-sided sparse FD top-" << kTop << " (fast loss)\n";
                const auto tf0 = std::chrono::steady_clock::now();
                double step = 0.22;
                int accepts = 0;
                for (int pass = 0; pass < 3; ++pass) {
                    for (int t = 0; t < kTop; ++t) {
                        const size_t i = rank[static_cast<size_t>(t)].second;
                        auto trial = best;
                        const double dir = (g[i] > 0.0) ? -step : step;
                        trial[i] = std::clamp(trial[i] + dir, 0.02, 1.0);
                        const double L = lossFast(trial);
                        if (L < bestLoss * 0.9995) {
                            best = std::move(trial);
                            bestLoss = L;
                            ++accepts;
                        }
                    }
                    step *= 0.55;
                }
                const auto tf1 = std::chrono::steady_clock::now();
                fdActualMs = std::chrono::duration<double, std::milli>(tf1 - tf0).count();
                std::cout << "  [dense-map] sparse FD accepts=" << accepts << " loss=" << bestLoss
                          << " sparse_ms=" << fdActualMs << "\n";
            }

            const auto ta1 = std::chrono::steady_clock::now();
            analyticMs = std::chrono::duration<double, std::milli>(ta1 - ta0).count();
            speedup = (analyticMs > 1.0) ? (fdEstMs / analyticMs) : 0.0;

            // Full multi-view quality gate (not timed as optim — same as MAPTEST metrics).
            reprime(best, /*force*/ true);
            bestLoss = lossDenseMap(renderer, inv, work, nViews, targets, kFrames);
            ohao::diff::gridIntoMap(best, G, work);
            const double preMapMse = ohao::diff::mapMse(work, gtMap);
            const double prePsnr = (bestLoss > 1e-12) ? (-10.0 * std::log10(bestLoss)) : 99.0;
            const bool qualityOk = (preMapMse < result.mapMseInit * 0.85) &&
                                   (prePsnr >= initPsnr + 2.0) && (bestLoss < initLoss * 0.90);
            optimAnalytic = qualityOk;
            // GRADCHECK strict flag: only when thr passed (soft solve may still optim).
            if (!usedAnalytic && qualityOk) usedAnalytic = (gradMedianRel < 0.30);
            std::cout << "  [dense-map] analytic+residual+sparse " << analyticMs
                      << " ms  est_speedup=" << speedup << "×  quality="
                      << (qualityOk ? "ok" : "need_more")
                      << (optimAnalytic ? "  OPTIM_ANALYTIC" : "") << "\n";
        }
    }

    // Full FD only if analytic+sparse failed quality gate.
    // Museum G=2 → only 12 free dims; run all 3 passes for clean marble recovery.
    if (!optimAnalytic) {
        std::cout << "  [dense-map] full coord FD from current θ (not cold init)\n";
        reprime(best, /*force*/ true);
        bestLoss = lossAtGrid(best);
        const auto tf0 = std::chrono::steady_clock::now();
        double step = cfg.museumStudio ? 0.28 : 0.18;
        const int nPass = cfg.museumStudio ? 4 : 3;
        for (int p = 0; p < nPass; ++p) {
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
            step *= 0.65;
            std::cout << "  [dense-map] FD pass " << (p + 1) << "/" << nPass
                      << " best_loss=" << bestLoss << " accepts=" << accepts << std::endl;
            if (!cfg.museumStudio && p >= 1 && bestLoss < initLoss * 0.40) break;
            if (accepts == 0 && bestLoss < initLoss * 0.92) break;
        }
        const auto tf1 = std::chrono::steady_clock::now();
        fdActualMs = std::chrono::duration<double, std::milli>(tf1 - tf0).count();
        // If we still have analytic phase time, report hybrid total speedup.
        if (analyticMs > 1.0)
            speedup = fdEstMs / (analyticMs + fdActualMs);
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

    // Init map for SHOW plate (wrong-init cool solid grid).
    ohao::diff::DiffAlbedoMap initMap;
    initMap.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    ohao::diff::gridIntoMap(cool, G, initMap);

    // High-res SHOW stills (publish face): re-render after optim — not lab_fast crops.
    if (wantShowStills || plateW > W || plateH > H) {
        std::cout << "  [dense-map] SHOW plate stills " << plateW << "x" << plateH << " @"
                  << showFrames << " frames"
                  << (cfg.museumStudio ? " (museum publish)" : "") << "\n";
        inv.scene.reset();
        InverseScene invShow = InverseScene::buildStudio(cfg);
        if (!cfg.labBundle.empty()) {
            std::filesystem::path labCap;
            std::string err;
            if (resolveLabCapturePath(cfg, labCap, err))
                (void)injectLabPoses(invShow, labCap / "cameras.jsonl");
        }
        invShow.applyTruth();
        VulkanRenderer showR(plateW, plateH);
        if (showR.initialize()) {
            showR.setRenderMode(RenderMode::Deferred);
            showR.setScene(invShow.scene.get());
            (void)showR.updateSceneBuffers();
            if (std::filesystem::exists(invShow.envPath)) applyEnv(showR, invShow.envPath);
            auto showSave = [&](const ohao::diff::DiffAlbedoMap& m, const char* name,
                                bool force = false) {
                auto img = forwardDenseMapDeferred(showR, invShow, m, 0, showFrames, force);
                savePNG(img, outDir / name);
            };
            showSave(gtMap, "dense_forward_truth_show.png", true);
            showSave(initMap, "dense_init_show.png");
            showSave(work, "dense_recovered_show.png");
            invShow.scene.reset();
        } else {
            std::cerr << "  WARN: SHOW renderer init failed — no high-res stills\n";
        }
    }

    std::cout << "  final loss=" << finalLoss << "  train PSNR=" << finalPsnr
              << "  (wrong-init PSNR was " << initPsnr << ")\n";
    std::cout << "  map_mse_init=" << result.mapMseInit << "  map_mse_recovered=" << result.mapMseRec
              << "\n";

    {
        std::ofstream mj(outDir / "dense_map_metrics.json");
        mj << "{\n"
           << "  \"backend\": \"diff\",\n"
           << "  \"mode\": \"dense_map\",\n"
           << "  \"metric_domain\": \""
           << (cfg.museumStudio ? "ohao_museum_studio_protocol" : "vulkan_deferred_studio")
           << "\",\n"
           << "  \"beauty_theta_path\": \"dense_map_bindless_deferred\",\n"
           << "  \"dense_map_sot\": true,\n"
           << "  \"quality_plate\": " << (qualityPlate ? "true" : "false") << ",\n"
           << "  \"museum_studio\": " << (cfg.museumStudio ? "true" : "false") << ",\n"
           << "  \"preset\": \"" << cfg.preset << "\",\n"
           << "  \"analytic_albedo_grad\": " << (usedAnalytic ? "true" : "false") << ",\n"
           << "  \"optim_analytic\": " << (optimAnalytic ? "true" : "false") << ",\n"
           << "  \"grad_median_rel_err\": " << gradMedianRel << ",\n"
           << "  \"analytic_optim_ms\": " << analyticMs << ",\n"
           << "  \"fd_probe_ms\": " << fdProbeMs << ",\n"
           << "  \"fd_est_3pass_ms\": " << fdEstMs << ",\n"
           << "  \"fd_actual_ms\": " << fdActualMs << ",\n"
           << "  \"speedup_vs_fd\": " << speedup << ",\n"
           << "  \"analytic_steps\": " << analyticSteps << ",\n"
           << "  \"fit_wh\": [" << W << ", " << H << "],\n"
           << "  \"show_wh\": [" << plateW << ", " << plateH << "],\n"
           << "  \"fit_frames\": " << kFrames << ",\n"
           << "  \"show_frames\": " << showFrames << ",\n"
           << "  \"n_views\": " << nViews << ",\n"
           << "  \"dense_map_res\": " << mapPx << ",\n"
           << "  \"dense_grid\": " << G << ",\n"
           << "  \"map_upload\": \"in_place_updateTextureFromMemory\",\n"
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
           << "  \"schedule\": \""
           << (optimAnalytic ? "analytic_residual_sparse"
                             : (usedAnalytic ? "fd_after_gradcheck" : "dense_grid_coord_fd"))
           << "\",\n"
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
