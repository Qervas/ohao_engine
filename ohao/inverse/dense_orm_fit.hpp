#pragma once

// H2/M2a: free dense ground roughness (ORM.g) under Deferred.
// Fixed GT albedo; free G×G rough grid painted to dense map → bindless ORM.
// Wrong-init high-rough solid; coord FD + Adam; MAPTEST + synthetic relight gate.

#include "inverse/dense_common.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/diff/diff_map.hpp"
#include "render/diff/diff_map_bind.hpp"
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

namespace dense_orm_detail {

using dense_common::saveMapPng;
using dense_common::psnrFromMse;
using dense_common::fillCheckerScalar;
using dense_common::fillProductScalar;

// Absolute metal in ORM.b (base metallic = 1 in bind); high for rough signal.
inline constexpr float kOrmGroundMetal = 0.72f;

/// Forward beauty with fixed albedo + free roughness ORM under Deferred.
[[nodiscard]] inline ImageRGBA8 forwardOrm(VulkanRenderer& renderer, InverseScene& inv,
                                           const ohao::diff::DiffAlbedoMap& albedo,
                                           const ohao::diff::DiffAlbedoMap& rough, int viewIndex,
                                           int frames, bool forceSceneRebuild = false) {
    static bool s_primed = false;
    if (forceSceneRebuild) s_primed = false;
    ohao::diff::DiffAlbedoMap orm;
    ohao::diff::packOrmMap(rough, kOrmGroundMetal, orm);
    if (!s_primed) {
        inv.applyTruth();
        inv.truthPrimary.metallic = kOrmGroundMetal;
        if (inv.primaryMat) inv.primaryMat->getMaterial().metallic = kOrmGroundMetal;
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

// Floor band crop: ground is bottom of studio frame; hero/pedestal dominate full-frame MSE.
inline constexpr double kOrmCropX = 1.0;
inline constexpr double kOrmCropYMin = 0.42;

[[nodiscard]] inline double beautyLoss(const ImageRGBA8& pred, const ImageRGBA8& tgt) {
    // Specular-biased so roughness moves the loss more than flat diffuse.
    return hybridSpecularRGB(pred, tgt, kOrmCropX, kOrmCropYMin, /*maeW=*/0.40,
                             /*specW=*/0.70);
}

[[nodiscard]] inline double beautyMse(const ImageRGBA8& pred, const ImageRGBA8& tgt) {
    return mseRGB(pred, tgt, kOrmCropX, kOrmCropYMin);
}

[[nodiscard]] inline double lossOrm(VulkanRenderer& renderer, InverseScene& inv,
                                    const ohao::diff::DiffAlbedoMap& albedo,
                                    const ohao::diff::DiffAlbedoMap& rough, int nViews,
                                    const std::vector<ImageRGBA8>& targets, int frames) {
    double sum = 0.0;
    for (int v = 0; v < nViews; ++v) {
        auto img = forwardOrm(renderer, inv, albedo, rough, v, frames);
        sum += beautyLoss(img, targets[static_cast<size_t>(v)]);
    }
    return sum / static_cast<double>(nViews);
}

} // namespace dense_orm_detail

struct DenseOrmFitResult {
    bool pass{false};
    double initLoss{0}, finalLoss{0};
    double initPsnr{0}, trainPsnr{0};
    double roughMseInit{0}, roughMseRec{0};
    double relightInitPsnr{0}, relightRecPsnr{0};
};

/// Free dense roughness map fit (H2/M2a). Returns MAPTEST + relight outcome.
[[nodiscard]] inline DenseOrmFitResult runDenseOrmFit(FitConfig cfg) {
    using namespace dense_orm_detail;
    DenseOrmFitResult result{};
    std::cout << std::unitbuf;
    applyPreset(cfg);
    resolveAssetFallbacks(cfg);
    cfg.mapGround = true;
    if (cfg.mapRes < 2) cfg.mapRes = 2;
    if (cfg.denseMapRes < 32) cfg.denseMapRes = 64;
    if (cfg.denseGrid < 4) cfg.denseGrid = 8;
    cfg.denseGrid = std::clamp(cfg.denseGrid, 4, 16);
    cfg.denseMapRes = std::clamp(cfg.denseMapRes, 32, 256);

    const auto vp = dense_common::resolveViewport(cfg);
    const std::uint32_t W = vp.fitW, H = vp.fitH, showW = vp.showW, showH = vp.showH;
    const bool wantShowStills = vp.wantShowStills();
    const int kFrames = vp.frames;
    const int showFrames = vp.showFrames;
    int nViews = (cfg.denseViews > 0) ? std::clamp(cfg.denseViews, 1, 4) : 2;
    const int G = cfg.denseGrid;
    const int mapPx = cfg.denseMapRes;
    const int nGrid = G * G; // single-channel rough θ
    const bool qualityPlate = cfg.denseQualityPlate;

    InverseScene inv = InverseScene::buildStudio(cfg);
    if (!inv.mapGround || inv.groundTiles.empty()) {
        std::cerr << "FATAL: dense ORM fit requires map-ground studio\n";
        return result;
    }
    nViews = std::min(nViews, std::max(1, static_cast<int>(inv.views.size())));

    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: dense ORM VulkanRenderer init failed\n";
        return result;
    }
    renderer.setRenderMode(RenderMode::Deferred);
    if (std::filesystem::exists(inv.envPath)) applyEnv(renderer, inv.envPath);

    inv.applyTruth();
    // Specular headroom for roughness signal under Deferred lights (ORM.b absolute).
    inv.truthPrimary.metallic = kOrmGroundMetal;
    renderer.setScene(inv.scene.get());
    (void)renderer.updateSceneBuffers();

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir / "materials");

    // Fixed albedo SoT (not free) — mid gray; metal + rough drive highlights.
    ohao::diff::DiffAlbedoMap fixedAlb;
    fixedAlb.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    fixedAlb.fill(0.48f, 0.48f, 0.50f);

    // GT roughness: checker (lab) or soft product bands (quality plate — less toy, still free-grid recoverable).
    const int checkerTiles = std::max(2, G / 2);
    const int productBands = std::max(2, G);
    ohao::diff::DiffAlbedoMap gtRough;
    gtRough.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    // Quality: denser G-aligned checker (product-looking tiles, still free-grid recoverable).
    // Soft product_bands exist in dense_common for future continuous θ.
    if (qualityPlate) {
        fillCheckerScalar(gtRough, G, /*lo=*/0.10f, /*hi=*/0.92f);
    } else {
        fillCheckerScalar(gtRough, checkerTiles, /*lo=*/0.08f, /*hi=*/0.95f);
    }
    const std::string gtPattern =
        qualityPlate ? (std::string("checker_dense_") + std::to_string(G))
                     : (std::string("checker_") + std::to_string(checkerTiles));

    std::vector<ImageRGBA8> targets;
    for (int v = 0; v < nViews; ++v) {
        auto img = forwardOrm(renderer, inv, fixedAlb, gtRough, v, kFrames, /*force*/ v == 0);
        savePNG(img, outDir / (std::string("orm_target_") + std::to_string(v) + ".png"));
        targets.push_back(std::move(img));
    }
    savePNG(targets[0], outDir / "orm_forward_truth.png");
    saveMapPng(gtRough, outDir / "materials" / "ground_rough_gt.png");
    {
        ohao::diff::DiffAlbedoMap gtOrm;
        ohao::diff::packOrmMap(gtRough, kOrmGroundMetal, gtOrm);
        saveMapPng(gtOrm, outDir / "materials" / "ground_orm_gt.png");
        saveMapPng(fixedAlb, outDir / "materials" / "ground_albedo_fixed.png");
    }

    std::cout << "Dense-ORM Diff-IR  views=" << nViews << "  FIT " << W << "x" << H
              << "  SHOW " << showW << "x" << showH << "  map=" << mapPx << "x" << mapPx
              << "  free_rough_grid=" << G << "x" << G << "  (θ dims=" << nGrid
              << ")  gt=" << gtPattern << (qualityPlate ? "  QUALITY_PLATE" : "") << "\n";
    std::cout << "  frames fit=" << kFrames << " show=" << showFrames
              << "  beauty SoT: fixed albedo + free ORM.g (Deferred)\n";
    std::cout << "  loss: floor crop y>=" << kOrmCropYMin << " + specular; metal="
              << kOrmGroundMetal << "  wrong_init=high_rough_solid\n";

    auto lossAtGrid = [&](const std::vector<double>& grid) {
        ohao::diff::DiffAlbedoMap m;
        m.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoRoughMap(grid, G, m);
        return lossOrm(renderer, inv, fixedAlb, m, nViews, targets, kFrames);
    };

    // A/B: glossy vs matte solid must move beauty (floor crop).
    {
        std::vector<double> glossy(static_cast<size_t>(nGrid), 0.06);
        std::vector<double> matte(static_cast<size_t>(nGrid), 0.98);
        ohao::diff::DiffAlbedoMap mG, mM;
        mG.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        mM.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoRoughMap(glossy, G, mG);
        ohao::diff::gridIntoRoughMap(matte, G, mM);
        auto gImg = forwardOrm(renderer, inv, fixedAlb, mG, 0, kFrames, true);
        auto mImg = forwardOrm(renderer, inv, fixedAlb, mM, 0, kFrames);
        const double ab = beautyMse(gImg, mImg);
        std::cout << "  A/B glossy vs matte floor MSE=" << ab << "\n";
        if (ab < 5e-5) {
            std::cerr << "FATAL: roughness θ does not affect Deferred beauty (floor crop)\n";
            inv.scene.reset();
            return result;
        }
    }

    // Wrong-init: high rough solid (far from checker GT).
    std::vector<double> th(static_cast<size_t>(nGrid), 0.95);
    ohao::diff::DiffAlbedoMap work;
    work.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
    ohao::diff::gridIntoRoughMap(th, G, work);

    const double initLoss = lossOrm(renderer, inv, fixedAlb, work, nViews, targets, kFrames);
    auto initImg0 = forwardOrm(renderer, inv, fixedAlb, work, 0, kFrames);
    const double initMse = beautyMse(initImg0, targets[0]);
    const double initPsnr = psnrFromMse(initMse);
    result.initLoss = initLoss;
    result.initPsnr = initPsnr;
    result.roughMseInit = ohao::diff::roughMapMse(work, gtRough);

    savePNG(initImg0, outDir / "orm_init.png");
    saveMapPng(work, outDir / "materials" / "ground_rough_init.png");
    {
        ohao::diff::DiffAlbedoMap initOrm;
        ohao::diff::packOrmMap(work, kOrmGroundMetal, initOrm);
        saveMapPng(initOrm, outDir / "materials" / "ground_orm_init.png");
    }
    std::cout << "  wrong-init loss=" << initLoss << " floor_PSNR=" << initPsnr
              << " rough_mse_vs_gt=" << result.roughMseInit << "\n";

    // Coordinate FD on free rough grid (aggressive steps; G-aligned checker is well-conditioned).
    std::vector<double> best = th;
    double bestLoss = initLoss;
    double step = 0.28;
    const int passes = 4;
    for (int p = 0; p < passes; ++p) {
        int accepts = 0;
        for (size_t i = 0; i < best.size(); ++i) {
            auto trialP = best;
            auto trialM = best;
            trialP[i] = std::clamp(trialP[i] + step, 0.04, 1.0);
            trialM[i] = std::clamp(trialM[i] - step, 0.04, 1.0);
            const double Lp = lossAtGrid(trialP);
            const double Lm = lossAtGrid(trialM);
            const double thr = bestLoss * 0.999;
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
        std::cout << "  [dense-orm] FD pass " << (p + 1) << "/" << passes
                  << " best_loss=" << bestLoss << " accepts=" << accepts << std::endl;
        if (bestLoss < initLoss * 0.40) {
            std::cout << "  [dense-orm] early stop (strong loss drop)\n";
            break;
        }
        if (accepts == 0 && bestLoss < initLoss * 0.90) {
            std::cout << "  [dense-orm] early stop (plateau)\n";
            break;
        }
    }

    // Sparse Adam polish on random rough-grid coords.
    {
        ohao::diff::DiffAdam adam;
        ohao::diff::DiffAlbedoMap gridMap;
        gridMap.allocate(static_cast<std::uint32_t>(G), static_cast<std::uint32_t>(G));
        for (int i = 0; i < nGrid; ++i) {
            const float r = static_cast<float>(best[static_cast<size_t>(i)]);
            gridMap.rgb[static_cast<size_t>(i) * 3 + 0] = r;
            gridMap.rgb[static_cast<size_t>(i) * 3 + 1] = r;
            gridMap.rgb[static_cast<size_t>(i) * 3 + 2] = r;
        }
        adam.resize(gridMap.rgb.size());
        const int adamSteps = 10;
        const int batch = std::min(nGrid, 16);
        std::uint32_t rng = 0x0A11DEu ^ static_cast<std::uint32_t>(cfg.seed);
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
                gp[gi] = std::clamp(gp[gi] + e, 0.04, 1.0);
                gm[gi] = std::clamp(gm[gi] - e, 0.04, 1.0);
                const double gval = (lossAtGrid(gp) - lossAtGrid(gm)) / (gp[gi] - gm[gi] + 1e-12);
                grad[gi * 3 + 0] += static_cast<float>(gval);
                grad[gi * 3 + 1] += static_cast<float>(gval);
                grad[gi * 3 + 2] += static_cast<float>(gval);
            }
            adam.step(gridMap, grad, 0.15f);
            for (int i = 0; i < nGrid; ++i) {
                best[static_cast<size_t>(i)] = std::clamp(
                    static_cast<double>(gridMap.rgb[static_cast<size_t>(i) * 3 + 1]), 0.04, 1.0);
                const float r = static_cast<float>(best[static_cast<size_t>(i)]);
                gridMap.rgb[static_cast<size_t>(i) * 3 + 0] = r;
                gridMap.rgb[static_cast<size_t>(i) * 3 + 1] = r;
                gridMap.rgb[static_cast<size_t>(i) * 3 + 2] = r;
            }
            const double L = lossAtGrid(best);
            if (L < bestLoss) bestLoss = L;
            if ((s + 1) % 2 == 0)
                std::cout << "  [dense-orm] Adam step " << (s + 1) << "/" << adamSteps
                          << " loss=" << bestLoss << std::endl;
        }
    }

    ohao::diff::gridIntoRoughMap(best, G, work);
    const double finalLoss =
        std::min(bestLoss, lossOrm(renderer, inv, fixedAlb, work, nViews, targets, kFrames));
    auto recImg0 = forwardOrm(renderer, inv, fixedAlb, work, 0, kFrames, true);
    const double finalMse = beautyMse(recImg0, targets[0]);
    const double finalPsnr = psnrFromMse(finalMse);
    result.finalLoss = finalLoss;
    result.trainPsnr = finalPsnr;
    result.roughMseRec = ohao::diff::roughMapMse(work, gtRough);

    savePNG(recImg0, outDir / "orm_recovered.png");
    saveMapPng(work, outDir / "materials" / "ground_rough_recovered.png");
    {
        ohao::diff::DiffAlbedoMap recOrm;
        ohao::diff::packOrmMap(work, kOrmGroundMetal, recOrm);
        saveMapPng(recOrm, outDir / "materials" / "ground_orm_recovered.png");
    }

    // Synthetic Deferred relight: boost key light (same maps, new lighting).
    auto measureRelightPsnr = [&](const ohao::diff::DiffAlbedoMap& rough) {
        float saved = 1.f;
        if (inv.keyLight) {
            saved = inv.keyLight->getIntensity();
            inv.keyLight->setIntensity(saved * 2.5f);
        }
        auto tgt = forwardOrm(renderer, inv, fixedAlb, gtRough, 0, kFrames, true);
        auto img = forwardOrm(renderer, inv, fixedAlb, rough, 0, kFrames);
        if (inv.keyLight) inv.keyLight->setIntensity(saved);
        return psnrFromMse(beautyMse(img, tgt));
    };

    // Init / recovered under relight (each call restores key).
    {
        ohao::diff::DiffAlbedoMap initRough;
        initRough.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
        ohao::diff::gridIntoRoughMap(th, G, initRough);
        result.relightInitPsnr = measureRelightPsnr(initRough);
        result.relightRecPsnr = measureRelightPsnr(work);
        // Capture stills under relight with recovered.
        if (inv.keyLight) {
            const float saved = inv.keyLight->getIntensity();
            inv.keyLight->setIntensity(saved * 2.5f);
            auto relT = forwardOrm(renderer, inv, fixedAlb, gtRough, 0, kFrames, true);
            auto relR = forwardOrm(renderer, inv, fixedAlb, work, 0, kFrames);
            savePNG(relT, outDir / "orm_relight_truth.png");
            savePNG(relR, outDir / "orm_relight_recovered.png");
            inv.keyLight->setIntensity(saved);
        }
    }

    // HD / quality plate stills: re-render truth/init/recovered/relight at SHOW resolution.
    if (wantShowStills) {
        std::cout << "  [dense-orm] SHOW plate stills " << showW << "x" << showH
                  << " @" << showFrames << " frames"
                  << (qualityPlate ? " (quality-plate bar)" : "") << "\n";
        inv.scene.reset();
        InverseScene invShow = InverseScene::buildStudio(cfg);
        invShow.applyTruth();
        invShow.truthPrimary.metallic = kOrmGroundMetal;
        VulkanRenderer showR(showW, showH);
        if (showR.initialize()) {
            showR.setRenderMode(RenderMode::Deferred);
            if (std::filesystem::exists(invShow.envPath)) applyEnv(showR, invShow.envPath);
            showR.setScene(invShow.scene.get());
            (void)showR.updateSceneBuffers();
            auto showSave = [&](const ohao::diff::DiffAlbedoMap& rough, const char* name,
                                bool force = false) {
                auto img = forwardOrm(showR, invShow, fixedAlb, rough, 0, showFrames, force);
                savePNG(img, outDir / name);
                return img;
            };
            ohao::diff::DiffAlbedoMap initRough;
            initRough.allocate(static_cast<std::uint32_t>(mapPx), static_cast<std::uint32_t>(mapPx));
            ohao::diff::gridIntoRoughMap(th, G, initRough);
            showSave(gtRough, "orm_forward_truth_show.png", true);
            showSave(initRough, "orm_init_show.png");
            showSave(work, "orm_recovered_show.png");
            if (invShow.keyLight) {
                const float saved = invShow.keyLight->getIntensity();
                invShow.keyLight->setIntensity(saved * 2.5f);
                showSave(gtRough, "orm_relight_truth_show.png", true);
                showSave(work, "orm_relight_recovered_show.png");
                invShow.keyLight->setIntensity(saved);
            }
            invShow.scene.reset();
        }
    }

    std::cout << "  final loss=" << finalLoss << "  train PSNR=" << finalPsnr
              << "  (wrong-init PSNR was " << initPsnr << ")\n";
    std::cout << "  rough_mse_init=" << result.roughMseInit
              << "  rough_mse_recovered=" << result.roughMseRec << "\n";
    std::cout << "  relight PSNR init=" << result.relightInitPsnr
              << "  recovered=" << result.relightRecPsnr
              << "  (Δ=" << (result.relightRecPsnr - result.relightInitPsnr) << " dB)\n";

    {
        std::ofstream mj(outDir / "dense_orm_metrics.json");
        mj << "{\n"
           << "  \"backend\": \"diff\",\n"
           << "  \"mode\": \"dense_orm\",\n"
           << "  \"metric_domain\": \"vulkan_deferred_studio\",\n"
           << "  \"beauty_theta_path\": \"dense_orm_bindless_deferred\",\n"
           << "  \"dense_orm_sot\": true,\n"
           << "  \"quality_plate\": " << (qualityPlate ? "true" : "false") << ",\n"
           << "  \"preset\": \"" << cfg.preset << "\",\n"
           << "  \"fit_wh\": [" << W << ", " << H << "],\n"
           << "  \"show_wh\": [" << showW << ", " << showH << "],\n"
           << "  \"fit_frames\": " << kFrames << ",\n"
           << "  \"show_frames\": " << showFrames << ",\n"
           << "  \"n_views\": " << nViews << ",\n"
           << "  \"dense_map_res\": " << mapPx << ",\n"
           << "  \"dense_grid\": " << G << ",\n"
           << "  \"map_upload\": \"in_place_updateTextureFromMemory\",\n"
           << "  \"wrong_init_source\": \"high_rough_solid\",\n"
           << "  \"gt_rough_pattern\": \"" << gtPattern << "\",\n"
           << "  \"loss_crop_y_min\": " << kOrmCropYMin << ",\n"
           << "  \"ground_metal\": " << kOrmGroundMetal << ",\n"
           << "  \"albedo_free\": false,\n"
           << "  \"init_loss\": " << initLoss << ",\n"
           << "  \"final_loss\": " << finalLoss << ",\n"
           << "  \"init_psnr\": " << initPsnr << ",\n"
           << "  \"train_psnr\": " << finalPsnr << ",\n"
           << "  \"rough_mse_init\": " << result.roughMseInit << ",\n"
           << "  \"rough_mse_recovered\": " << result.roughMseRec << ",\n"
           << "  \"psnr_improve_db\": " << (finalPsnr - initPsnr) << ",\n"
           << "  \"relight_init_psnr\": " << result.relightInitPsnr << ",\n"
           << "  \"relight_recovered_psnr\": " << result.relightRecPsnr << ",\n"
           << "  \"relight_improve_db\": "
           << (result.relightRecPsnr - result.relightInitPsnr) << "\n"
           << "}\n";
    }
    {
        std::ofstream tj(outDir / "trajectory.json");
        tj << "{\n  \"backend\": \"diff\",\n  \"mode\": \"dense_orm\",\n"
           << "  \"schedule\": \"dense_rough_grid_coord_fd_adam\",\n"
           << "  \"best_loss\": " << finalLoss << ",\n"
           << "  \"init_loss\": " << initLoss << "\n}\n";
    }

    // M2a gates: rough map drop + train PSNR + synthetic relight improve.
    const bool mapDrop = result.roughMseRec < result.roughMseInit * 0.85;
    const bool psnrGain = finalPsnr >= initPsnr + 2.0;
    const bool lossDrop = finalLoss < initLoss * 0.90;
    const bool relightGain = result.relightRecPsnr >= result.relightInitPsnr + 1.5;
    result.pass = mapDrop && psnrGain && lossDrop && relightGain;
    std::cout << (result.pass ? "MAPTEST PASS" : "MAPTEST FAIL")
              << " (rough_mse " << result.roughMseInit << " → " << result.roughMseRec << "; PSNR "
              << initPsnr << " → " << finalPsnr << " dB; relight "
              << result.relightInitPsnr << " → " << result.relightRecPsnr << " dB)\n";
    if (!result.pass) {
        std::cout << "  gates: map_drop=" << mapDrop << " psnr_gain=" << psnrGain
                  << " loss_drop=" << lossDrop << " relight_gain=" << relightGain << "\n";
    }

    inv.scene.reset();
    return result;
}

[[nodiscard]] inline int runDenseOrmFitCli(FitConfig cfg) {
    return runDenseOrmFit(std::move(cfg)).pass ? 0 : 1;
}

} // namespace ohao::inverse
