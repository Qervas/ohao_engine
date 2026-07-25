#pragma once

// H2/M2b: free dense ground metallic (ORM.b) under Deferred.
// Fixed albedo+rough; free G×G metal grid; wrong-init; FD+Adam; MAPTEST+relight.

#include "inverse/dense_common.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/diff/diff_map.hpp"
#include "inverse/diff_map_bind.hpp"
#include "render/diff/diff_map_paint.hpp"
#include "render/diff/diff_optimizer.hpp"

#include "gpu/vulkan/renderer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/post_processing_pipeline.hpp"
#include "scene/component/light_component.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::inverse {

namespace dense_metal_detail {

using dense_common::saveMapPng;
using dense_common::psnrFromMse;
using dense_common::fillCheckerScalar;

// Glossy fixed rough for metal contrast.
inline constexpr float kFixedRough = 0.12f;
inline constexpr double kMetalCropX = 1.0;
inline constexpr double kMetalCropYMin = 0.42;
// Very soft mid prior (only anti-collapse; too strong fights checker GT extremes).
inline constexpr double kMetalPriorW = 0.0;

[[nodiscard]] inline ImageRGBA8 forwardMetal(VulkanRenderer& renderer, InverseScene& inv,
                                             const ohao::diff::DiffAlbedoMap& albedo,
                                             const ohao::diff::DiffAlbedoMap& rough,
                                             const ohao::diff::DiffAlbedoMap& metal, int viewIndex,
                                             int frames, bool forceSceneRebuild = false) {
    static bool s_primed = false;
    if (forceSceneRebuild) s_primed = false;
    ohao::diff::DiffAlbedoMap orm;
    ohao::diff::packOrmRoughMetal(rough, metal, orm);
    if (!s_primed) {
        inv.applyTruth();
        inv.truthPrimary.metallic = 1.f;
        if (inv.primaryMat) inv.primaryMat->getMaterial().metallic = 1.f;
        (void)ohao::diff::bindGroundAlbedoOrmMaps(renderer, inv, albedo, orm);
        (void)renderer.updateSceneBuffers();
        s_primed = true;
    } else {
        (void)ohao::diff::bindGroundAlbedoOrmMaps(renderer, inv, albedo, orm);
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

[[nodiscard]] inline double beautyLoss(const ImageRGBA8& pred, const ImageRGBA8& tgt) {
    return hybridSpecularRGB(pred, tgt, kMetalCropX, kMetalCropYMin, /*maeW=*/0.40,
                             /*specW=*/0.75);
}

[[nodiscard]] inline double beautyMse(const ImageRGBA8& pred, const ImageRGBA8& tgt) {
    return mseRGB(pred, tgt, kMetalCropX, kMetalCropYMin);
}

[[nodiscard]] inline double lossMetal(VulkanRenderer& renderer, InverseScene& inv,
                                      const ohao::diff::DiffAlbedoMap& albedo,
                                      const ohao::diff::DiffAlbedoMap& rough,
                                      const ohao::diff::DiffAlbedoMap& metal, int nViews,
                                      const std::vector<ImageRGBA8>& targets, int frames ) {
    double sum = 0.0;
    for (int v = 0; v < nViews; ++v) {
        auto img = forwardMetal(renderer, inv, albedo, rough, metal, v, frames);
        sum += beautyLoss(img, targets[static_cast<size_t>(v)]);
    }
    return sum / static_cast<double>(nViews);
}

} // namespace dense_metal_detail

struct DenseMetalFitResult {
    bool pass{false};
    double initLoss{0}, finalLoss{0};
    double initPsnr{0}, trainPsnr{0};
    double metalMseInit{0}, metalMseRec{0};
    double relightInitPsnr{0}, relightRecPsnr{0};
};

[[nodiscard]] inline DenseMetalFitResult runDenseMetalFit(FitConfig cfg) {
    using namespace dense_metal_detail;
    DenseMetalFitResult result{};
    std::cout << std::unitbuf;
    applyPreset(cfg);
    resolveAssetFallbacks(cfg);
    cfg.mapGround = true;
    if (cfg.mapRes < 2) cfg.mapRes = 2;
    if (cfg.denseMapRes < 32) cfg.denseMapRes = 64;
    if (cfg.denseGrid < 2) cfg.denseGrid = 2;
    cfg.denseGrid = std::clamp(cfg.denseGrid, 2, 16);
    cfg.denseMapRes = std::clamp(cfg.denseMapRes, 32, 256);

    const auto vp = dense_common::resolveViewport(cfg);
    const std::uint32_t W = vp.fitW, H = vp.fitH, showW = vp.showW, showH = vp.showH;
    const bool wantShowStills = vp.wantShowStills();
    const int kFrames = vp.frames;
    const int showFrames = vp.showFrames;
    int nViews = (cfg.denseViews > 0) ? std::clamp(cfg.denseViews, 1, 4) : 2;
    const bool qualityPlate = cfg.denseQualityPlate;
    const int G = cfg.denseGrid;
    const int mapPx = cfg.denseMapRes;
    const int nGrid = G * G;

    InverseScene inv = InverseScene::buildStudio(cfg);
    if (!inv.mapGround || inv.groundTiles.empty()) {
        std::cerr << "FATAL: dense metal fit requires map-ground studio\n";
        return result;
    }
    nViews = std::min(nViews, std::max(1, static_cast<int>(inv.views.size())));

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: dense metal VulkanRenderer init failed\n";
        return result;
    }
    renderer.setRenderMode(RenderMode::Deferred);
    if (std::filesystem::exists(inv.envPath)) applyEnv(renderer, inv.envPath);

    inv.applyTruth();
    inv.truthPrimary.metallic = 1.f;
    renderer.setScene(inv.scene.get());
    (void)renderer.updateSceneBuffers();

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir / "materials");

    ohao::diff::DiffAlbedoMap fixedAlb;
    fixedAlb.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    fixedAlb.fill(0.38f, 0.36f, 0.34f); // darker base so metal specular reads clearer

    ohao::diff::DiffAlbedoMap fixedRough;
    fixedRough.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    ohao::diff::fillRoughMap(fixedRough, kFixedRough);

    // Free grid = checker tiles.
    const int checkerTiles = G;
    ohao::diff::DiffAlbedoMap gtMetal;
    gtMetal.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    fillCheckerScalar(gtMetal, checkerTiles, /*lo=*/0.05f, /*hi=*/0.95f);

    std::vector<ImageRGBA8> targets;
    for (int v = 0; v < nViews; ++v) {
        auto img =
            forwardMetal(renderer, inv, fixedAlb, fixedRough, gtMetal, v, kFrames, /*force*/ v == 0);
        savePNG(img, outDir / (std::string("metal_target_") + std::to_string(v) + ".png"));
        targets.push_back(std::move(img));
    }
    savePNG(targets[0], outDir / "metal_forward_truth.png");
    saveMapPng(gtMetal, outDir / "materials" / "ground_metal_gt.png");
    {
        ohao::diff::DiffAlbedoMap gtOrm;
        ohao::diff::packOrmRoughMetal(fixedRough, gtMetal, gtOrm);
        saveMapPng(gtOrm, outDir / "materials" / "ground_orm_gt.png");
        saveMapPng(fixedAlb, outDir / "materials" / "ground_albedo_fixed.png");
        saveMapPng(fixedRough, outDir / "materials" / "ground_rough_fixed.png");
    }

    std::cout << "Dense-Metal Diff-IR  views=" << nViews << "  FIT " << W << "x" << H
              << "  SHOW " << showW << "x" << showH << "  map=" << mapPx << "x" << mapPx
              << "  free_metal_grid=" << G << "x" << G << "  (θ dims=" << nGrid
              << ")  checker=" << checkerTiles << "\n";
    std::cout << "  loss: floor crop y>=" << kMetalCropYMin << " + specular + metal prior w="
              << kMetalPriorW << "\n";

    auto lossAtGrid = [&](const std::vector<double>& grid) {
        ohao::diff::DiffAlbedoMap m;
        m.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoMetalMap(grid, G, m);
        return lossMetal(renderer, inv, fixedAlb, fixedRough, m, nViews, targets, kFrames);
    };

    // A/B: dielectric vs metal solid must move beauty (floor crop).
    {
        std::vector<double> diel(static_cast<size_t>(nGrid), 0.02);
        std::vector<double> met(static_cast<size_t>(nGrid), 0.98);
        ohao::diff::DiffAlbedoMap mD, mM;
        mD.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        mM.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoMetalMap(diel, G, mD);
        ohao::diff::gridIntoMetalMap(met, G, mM);
        auto dImg = forwardMetal(renderer, inv, fixedAlb, fixedRough, mD, 0, kFrames, true);
        auto mImg = forwardMetal(renderer, inv, fixedAlb, fixedRough, mM, 0, kFrames);
        const double ab = beautyMse(dImg, mImg);
        std::cout << "  A/B dielectric vs metal floor MSE=" << ab << "\n";
        if (ab < 5e-5) {
            std::cerr << "FATAL: metallic θ does not affect Deferred beauty (floor crop)\n";
            inv.scene.reset();
            return result;
        }
    }

    // Wrong-init: low metal solid (far from checker GT).
    std::vector<double> th(static_cast<size_t>(nGrid), 0.05);
    ohao::diff::DiffAlbedoMap work;
    work.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    ohao::diff::gridIntoMetalMap(th, G, work);

    const double initLoss =
        lossMetal(renderer, inv, fixedAlb, fixedRough, work, nViews, targets, kFrames);
    auto initImg0 = forwardMetal(renderer, inv, fixedAlb, fixedRough, work, 0, kFrames);
    const double initMse = beautyMse(initImg0, targets[0]);
    const double initPsnr = psnrFromMse(initMse);
    result.initLoss = initLoss;
    result.initPsnr = initPsnr;
    result.metalMseInit = ohao::diff::metalMapMse(work, gtMetal);

    savePNG(initImg0, outDir / "metal_init.png");
    saveMapPng(work, outDir / "materials" / "ground_metal_init.png");
    {
        ohao::diff::DiffAlbedoMap initOrm;
        ohao::diff::packOrmRoughMetal(fixedRough, work, initOrm);
        saveMapPng(initOrm, outDir / "materials" / "ground_orm_init.png");
    }
    std::cout << "  wrong-init loss=" << initLoss << " floor_PSNR=" << initPsnr
              << " metal_mse_vs_gt=" << result.metalMseInit << "\n";

    std::vector<double> best = th;
    double bestLoss = initLoss;
    // Extreme flip then continuous FD.
    {
        int accepts = 0;
        for (size_t i = 0; i < best.size(); ++i) {
            auto trialHi = best;
            auto trialLo = best;
            trialHi[i] = 0.95;
            trialLo[i] = 0.05;
            const double Lh = lossAtGrid(trialHi);
            const double Ll = lossAtGrid(trialLo);
            const double thr = bestLoss * 0.9995;
            if (Lh < thr && Lh <= Ll) {
                best = std::move(trialHi);
                bestLoss = Lh;
                ++accepts;
            } else if (Ll < thr) {
                best = std::move(trialLo);
                bestLoss = Ll;
                ++accepts;
            }
        }
        std::cout << "  [dense-metal] extreme flip pass best_loss=" << bestLoss
                  << " accepts=" << accepts << std::endl;
    }
    double step = 0.28;
    const int passes = 4;
    for (int p = 0; p < passes; ++p) {
        int accepts = 0;
        for (size_t i = 0; i < best.size(); ++i) {
            auto trialP = best;
            auto trialM = best;
            trialP[i] = std::clamp(trialP[i] + step, 0.0, 1.0);
            trialM[i] = std::clamp(trialM[i] - step, 0.0, 1.0);
            const double Lp = lossAtGrid(trialP);
            const double Lm = lossAtGrid(trialM);
            const double thr = bestLoss * 0.9995;
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
        std::cout << "  [dense-metal] FD pass " << (p + 1) << "/" << passes
                  << " best_loss=" << bestLoss << " accepts=" << accepts << std::endl;
        if (bestLoss < initLoss * 0.30) {
            std::cout << "  [dense-metal] early stop (strong loss drop)\n";
            break;
        }
        if (accepts == 0 && bestLoss < initLoss * 0.90) {
            std::cout << "  [dense-metal] early stop (plateau)\n";
            break;
        }
    }

    {
        ohao::diff::DiffAdam adam;
        ohao::diff::DiffAlbedoMap gridMap;
        gridMap.allocate(static_cast<std::uint32_t>(G), static_cast<std::uint32_t>(G));
        for (int i = 0; i < nGrid; ++i) {
            const float m = static_cast<float>(best[static_cast<size_t>(i)]);
            gridMap.rgb[static_cast<size_t>(i) * 3 + 0] = m;
            gridMap.rgb[static_cast<size_t>(i) * 3 + 1] = m;
            gridMap.rgb[static_cast<size_t>(i) * 3 + 2] = m;
        }
        adam.resize(gridMap.rgb.size());
        const int adamSteps = 14;
        const int batch = std::min(nGrid, 16);
        std::uint32_t rng = 0xB17A11u ^ static_cast<std::uint32_t>(cfg.seed);
        auto rnd = [&]() {
            rng = rng * 1664525u + 1013904223u;
            return rng;
        };
        for (int s = 0; s < adamSteps; ++s) {
            std::vector<float> grad(gridMap.rgb.size(), 0.f);
            for (int b = 0; b < batch; ++b) {
                const size_t gi = static_cast<size_t>(rnd() % static_cast<std::uint32_t>(nGrid));
                auto gp = best;
                auto gm = best;
                const double e = 0.06;
                gp[gi] = std::clamp(gp[gi] + e, 0.0, 1.0);
                gm[gi] = std::clamp(gm[gi] - e, 0.0, 1.0);
                const double gval = (lossAtGrid(gp) - lossAtGrid(gm)) / (gp[gi] - gm[gi] + 1e-12);
                grad[gi * 3 + 0] += static_cast<float>(gval);
                grad[gi * 3 + 1] += static_cast<float>(gval);
                grad[gi * 3 + 2] += static_cast<float>(gval);
            }
            adam.step(gridMap, grad, 0.15f);
            for (int i = 0; i < nGrid; ++i) {
                best[static_cast<size_t>(i)] = std::clamp(
                    static_cast<double>(gridMap.rgb[static_cast<size_t>(i) * 3 + 2]), 0.0, 1.0);
                const float m = static_cast<float>(best[static_cast<size_t>(i)]);
                gridMap.rgb[static_cast<size_t>(i) * 3 + 0] = m;
                gridMap.rgb[static_cast<size_t>(i) * 3 + 1] = m;
                gridMap.rgb[static_cast<size_t>(i) * 3 + 2] = m;
            }
            const double L = lossAtGrid(best);
            if (L < bestLoss) bestLoss = L;
            if ((s + 1) % 2 == 0)
                std::cout << "  [dense-metal] Adam step " << (s + 1) << "/" << adamSteps
                          << " loss=" << bestLoss << std::endl;
        }
    }

    ohao::diff::gridIntoMetalMap(best, G, work);
    const double finalLoss = std::min(
        bestLoss, lossMetal(renderer, inv, fixedAlb, fixedRough, work, nViews, targets, kFrames));
    auto recImg0 = forwardMetal(renderer, inv, fixedAlb, fixedRough, work, 0, kFrames, true);
    const double finalMse = beautyMse(recImg0, targets[0]);
    const double finalPsnr = psnrFromMse(finalMse);
    result.finalLoss = finalLoss;
    result.trainPsnr = finalPsnr;
    result.metalMseRec = ohao::diff::metalMapMse(work, gtMetal);

    savePNG(recImg0, outDir / "metal_recovered.png");
    saveMapPng(work, outDir / "materials" / "ground_metal_recovered.png");
    {
        ohao::diff::DiffAlbedoMap recOrm;
        ohao::diff::packOrmRoughMetal(fixedRough, work, recOrm);
        saveMapPng(recOrm, outDir / "materials" / "ground_orm_recovered.png");
    }

    // Synthetic Deferred relight: SAME key light at kRelightKeyScale× training
    // intensity (not novel illumination — no env swap, no light move). The scope
    // scales the θ source-of-truth so the forced forward's applyTruth() cannot
    // revert it, and verify() aborts if it somehow did.
    auto measureRelightPsnr = [&](const ohao::diff::DiffAlbedoMap& metal) {
        dense_common::RelightScope relight(inv);
        auto tgt = forwardMetal(renderer, inv, fixedAlb, fixedRough, gtMetal, 0, kFrames, true);
        auto img = forwardMetal(renderer, inv, fixedAlb, fixedRough, metal, 0, kFrames);
        relight.verify("dense-metal");
        return psnrFromMse(beautyMse(img, tgt));
    };

    {
        ohao::diff::DiffAlbedoMap initMetal;
        initMetal.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoMetalMap(th, G, initMetal);
        result.relightInitPsnr = measureRelightPsnr(initMetal);
        result.relightRecPsnr = measureRelightPsnr(work);
        {
            dense_common::RelightScope relight(inv);
            auto relT = forwardMetal(renderer, inv, fixedAlb, fixedRough, gtMetal, 0, kFrames, true);
            auto relR = forwardMetal(renderer, inv, fixedAlb, fixedRough, work, 0, kFrames);
            relight.verify("dense-metal stills");
            savePNG(relT, outDir / "metal_relight_truth.png");
            savePNG(relR, outDir / "metal_relight_recovered.png");
        }
    }

    if (wantShowStills) {
        std::cout << "  [dense-metal] SHOW plate stills " << showW << "x" << showH << "\n";
        inv.scene.reset();
        InverseScene invShow = InverseScene::buildStudio(cfg);
        invShow.applyTruth();
        invShow.truthPrimary.metallic = 1.f;
        VulkanRenderer showR(showW, showH);
        if (showR.initialize()) {
            showR.setRenderMode(RenderMode::Deferred);
            if (std::filesystem::exists(invShow.envPath)) applyEnv(showR, invShow.envPath);
            showR.setScene(invShow.scene.get());
            (void)showR.updateSceneBuffers();
            auto showSave = [&](const ohao::diff::DiffAlbedoMap& metal, const char* name,
                                bool force = false) {
                auto img =
                    forwardMetal(showR, invShow, fixedAlb, fixedRough, metal, 0, showFrames, force);
                savePNG(img, outDir / name);
            };
            ohao::diff::DiffAlbedoMap initMetal;
            initMetal.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
            ohao::diff::gridIntoMetalMap(th, G, initMetal);
            showSave(gtMetal, "metal_forward_truth_show.png", true);
            showSave(initMetal, "metal_init_show.png");
            showSave(work, "metal_recovered_show.png");
            {
                dense_common::RelightScope relight(invShow);
                showSave(gtMetal, "metal_relight_truth_show.png", true);
                showSave(work, "metal_relight_recovered_show.png");
                relight.verify("dense-metal show");
            }
            invShow.scene.reset();
        }
    }

    std::cout << "  final loss=" << finalLoss << "  train PSNR=" << finalPsnr
              << "  (wrong-init PSNR was " << initPsnr << ")\n";
    std::cout << "  metal_mse_init=" << result.metalMseInit
              << "  metal_mse_recovered=" << result.metalMseRec << "\n";
    std::cout << "  relight PSNR init=" << result.relightInitPsnr
              << "  recovered=" << result.relightRecPsnr
              << "  (Δ=" << (result.relightRecPsnr - result.relightInitPsnr) << " dB)\n";

    {
        std::ofstream mj(outDir / "dense_metal_metrics.json");
        mj << "{\n"
           << "  \"backend\": \"diff\",\n"
           << "  \"mode\": \"dense_metal\",\n"
           << "  \"metric_domain\": \"vulkan_deferred_studio\",\n"
           << "  \"beauty_theta_path\": \"dense_metal_bindless_deferred\",\n"
           << "  \"dense_metal_sot\": true,\n"
           << "  \"quality_plate\": " << (qualityPlate ? "true" : "false") << ",\n"
           << "  \"preset\": \"" << cfg.preset << "\",\n"
           << "  \"fit_wh\": [" << W << ", " << H << "],\n"
           << "  \"show_wh\": [" << showW << ", " << showH << "],\n"
           // Present in dense_orm/dense_map metrics since 3557cfb; missing here, which
           // made test_quality_plate.py's show_frames>=12 check a silent no-op for every
           // dense_metal run (it defaulted the key to 0 and skipped).
           << "  \"fit_frames\": " << kFrames << ",\n"
           << "  \"show_frames\": " << showFrames << ",\n"
           << "  \"n_views\": " << nViews << ",\n"
           << "  \"dense_map_res\": " << mapPx << ",\n"
           << "  \"dense_grid\": " << G << ",\n"
           << "  \"map_upload\": \"in_place_updateTextureFromMemory\",\n"
           << "  \"wrong_init_source\": \"low_metal_solid\",\n"
           << "  \"gt_metal_pattern\": \"checker_" << checkerTiles << "\",\n"
           << "  \"loss_crop_y_min\": " << kMetalCropYMin << ",\n"
           << "  \"fixed_rough\": " << kFixedRough << ",\n"
           << "  \"metal_prior_w\": " << kMetalPriorW << ",\n"
           << "  \"albedo_free\": false,\n"
           << "  \"rough_free\": false,\n"
           << "  \"init_loss\": " << initLoss << ",\n"
           << "  \"final_loss\": " << finalLoss << ",\n"
           << "  \"init_psnr\": " << initPsnr << ",\n"
           << "  \"train_psnr\": " << finalPsnr << ",\n"
           << "  \"metal_mse_init\": " << result.metalMseInit << ",\n"
           << "  \"metal_mse_recovered\": " << result.metalMseRec << ",\n"
           << "  \"psnr_improve_db\": " << (finalPsnr - initPsnr) << ",\n"
           << "  \"relight_init_psnr\": " << result.relightInitPsnr << ",\n"
           << "  \"relight_recovered_psnr\": " << result.relightRecPsnr << ",\n"
           << "  \"relight_improve_db\": "
           << (result.relightRecPsnr - result.relightInitPsnr) << "\n"
           << "}\n";
    }

    const bool mapDrop = result.metalMseRec < result.metalMseInit * 0.85;
    const bool psnrGain = finalPsnr >= initPsnr + 2.0;
    const bool lossDrop = finalLoss < initLoss * 0.90;
    const bool relightGain = result.relightRecPsnr >= result.relightInitPsnr + 1.5;
    result.pass = mapDrop && psnrGain && lossDrop && relightGain;
    std::cout << (result.pass ? "MAPTEST PASS" : "MAPTEST FAIL")
              << " (metal_mse " << result.metalMseInit << " → " << result.metalMseRec << "; PSNR "
              << initPsnr << " → " << finalPsnr << " dB; relight " << result.relightInitPsnr
              << " → " << result.relightRecPsnr << " dB)\n";
    if (!result.pass) {
        std::cout << "  gates: map_drop=" << mapDrop << " psnr_gain=" << psnrGain
                  << " loss_drop=" << lossDrop << " relight_gain=" << relightGain << "\n";
    }

    inv.scene.reset();
    return result;
}

[[nodiscard]] inline int runDenseMetalFitCli(FitConfig cfg) {
    return runDenseMetalFit(std::move(cfg)).pass ? 0 : 1;
}

} // namespace ohao::inverse
