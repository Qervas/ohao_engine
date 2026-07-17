#pragma once

// Hybrid plate: Diff-IR fit (Deferred dense-map SoT) → short PT light refine
// → capture-gated holdout/relight eval. LABTEST uses capture PNGs (same bar as pt).

#include "inverse/diff_fit.hpp"
#include "inverse/export_capture.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/fit_lab_eval.hpp"
#include "inverse/fit_targets.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/param_space.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"
#include "inverse/staged_fit.hpp"

#include "gpu/vulkan/renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::inverse {

/// Full studio θ = truth primary lights/BRDF base with ground tile RGB from Diff.
[[nodiscard]] inline std::vector<double> thetaWithTiles(const InverseScene& inv,
                                                        const std::vector<double>& tiles) {
    auto th = inv.truthTheta();
    if (!inv.mapGround) return th;
    const int nT = inv.tileCount();
    for (int k = 0; k < nT; ++k) {
        const size_t o = static_cast<size_t>(k) * 3u;
        if (o + 2 >= tiles.size() || o + 2 >= th.size()) break;
        th[o + 0] = tiles[o + 0];
        th[o + 1] = tiles[o + 1];
        th[o + 2] = tiles[o + 2];
    }
    return th;
}

/// Coordinate FD on index range [free0, free1) vs lab FIT train targets.
inline void refineHybridRange(RenderSession& session, InverseScene& inv, ParamSpace& space,
                              const FitTargetBundle& tb, const FitConfig& cfg, size_t free0,
                              size_t free1, int passes, double step0, const char* tag) {
    free1 = std::min(free1, space.values.size());
    if (free0 >= free1 || tb.targetsFit.empty()) return;

    RenderBudget fit = cfg.fit;
    fit.spp = std::max(fit.spp, 64);
    fit.width = std::max(fit.width, 384u);
    fit.height = std::max(fit.height, 216u);

    const int nTrain = std::min(tb.nViews, static_cast<int>(tb.targetsFit.size()));
    auto lossAt = [&](const std::vector<double>& th) -> double {
        inv.applyTheta(th);
        double sum = 0.0;
        for (int v = 0; v < nTrain; ++v) {
            auto img =
                session.render(v, fit, cfg.seed + static_cast<uint32_t>(v), DenoiseMode::None);
            if (inv.fitExposure) img = applyExposure(img, inv.currentExposure);
            ImageRGBA8 pred = img;
            const auto& tgt = tb.targetsFit[static_cast<size_t>(v)];
            if (pred.width != tgt.width || pred.height != tgt.height)
                pred = resizeNearest(pred, tgt.width, tgt.height);
            sum += mseRGB(pred, tgt);
        }
        return sum / static_cast<double>(nTrain);
    };

    double bestLoss = lossAt(space.values);
    std::vector<double> best = space.values;
    double step = step0;
    std::cout << "  [hybrid-pt] " << tag << " free=[" << free0 << "," << free1
              << ") fit=" << fit.width << "x" << fit.height << "@" << fit.spp
              << " init_loss=" << bestLoss << "\n";

    for (int p = 0; p < passes; ++p) {
        int accepts = 0;
        for (size_t i = free0; i < free1; ++i) {
            auto trialP = best;
            auto trialM = best;
            trialP[i] = space.project(i, trialP[i] + step);
            trialM[i] = space.project(i, trialM[i] - step);
            const double Lp = lossAt(trialP);
            const double Lm = lossAt(trialM);
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
        step *= 0.70;
        std::cout << "  [hybrid-pt] " << tag << " pass " << (p + 1) << "/" << passes
                  << " loss=" << bestLoss << " accepts=" << accepts << "\n";
        if (accepts == 0 && p > 0) break;
    }
    space.values = std::move(best);
    inv.applyTheta(space.values);
    std::cout << "  [hybrid-pt] " << tag << " done loss=" << bestLoss << "\n";
}

/// Lights first, then soft tile nudge under PT, then lights again (tiles from Diff seed).
inline void refineHybridLights(RenderSession& session, InverseScene& inv, ParamSpace& space,
                               const FitTargetBundle& tb, const FitConfig& cfg, int /*passes*/ = 4) {
    const size_t nTileRgb =
        inv.mapGround ? static_cast<size_t>(inv.tileCount()) * 3u : 0u;
    const size_t light0 = inv.lightBlockStart();
    // Stage 1: lights/env only (best generalization so far).
    refineHybridRange(session, inv, space, tb, cfg, light0, space.values.size(), 4, 0.12,
                      "lights");
    // Stage 2: soft tile RGB nudge under PT (small step — Diff already placed them).
    if (nTileRgb > 0)
        refineHybridRange(session, inv, space, tb, cfg, 0, nTileRgb, 2, 0.05, "tiles-soft");
    // Stage 3: lights again after tile nudge.
    refineHybridRange(session, inv, space, tb, cfg, light0, space.values.size(), 2, 0.06,
                      "lights2");
}

/// Diff fit → PT light refine → capture-gated eval.
[[nodiscard]] inline int runHybridDiffPt(FitConfig cfg) {
    std::cout << std::unitbuf;
    applyPreset(cfg);
    resolveAssetFallbacks(cfg);
    cfg.mapGround = true;
    if (cfg.mapRes < 2) cfg.mapRes = 2;

    if (cfg.labBundle.empty()) {
        std::cerr << "FATAL: --backend hybrid requires --lab-bundle <capture/>\n";
        return 1;
    }

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir);

    std::cout << "=== Hybrid: Diff fit (Deferred map SoT) ===\n";
    FitConfig diffCfg = cfg;
    diffCfg.backend = InverseBackend::Diff;
    const DiffFitResult diff = runDiffFitDetailed(std::move(diffCfg));
    if (!diff.pass || diff.recoveredTiles.empty()) {
        std::cerr << "HYBRID FAIL: Diff DIFFTEST did not pass (skipping PT eval)\n";
        return 1;
    }

    std::cout << "=== Hybrid: PT light refine + capture-gated eval ===\n";
    InverseScene inv = InverseScene::buildStudio(cfg);
    if (!inv.mapGround) {
        std::cerr << "FATAL: hybrid needs map-ground studio\n";
        return 1;
    }

    VulkanRenderer renderer(cfg.show.width, cfg.show.height);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: hybrid PT renderer init failed\n";
        return 1;
    }
    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.setRenderSeed(cfg.seed);
    if (std::filesystem::exists(inv.envPath)) applyEnv(renderer, inv.envPath);

    RenderSession session{renderer, inv, false};
    int nViews = std::min(cfg.numViews, static_cast<int>(inv.views.size()));
    inv.applyTruth();
    const auto truthV = inv.truthTheta();

    FitTargetBundle tb;
    if (const int rc = loadFitTargets(cfg, inv, session, tb, nViews, truthV, outDir); rc != 0) {
        inv.scene.reset();
        return rc;
    }
    if (!tb.labMode) {
        std::cerr << "FATAL: hybrid PT eval requires a valid lab capture bundle\n";
        inv.scene.reset();
        return 1;
    }

    // Wrong-init under PT (gain baseline).
    ParamSpace wrongSpace = buildParamSpace(inv);
    wrongSpace.values = inv.initTheta();
    LabWrongInitMetrics wrong{};
    {
        inv.applyTheta(wrongSpace.values);
        ImageRGBA8 initShow = session.render(0, cfg.show, cfg.seed, DenoiseMode::None);
        savePNG(initShow, outDir / "hybrid_pt_init_show.png");
        wrong.showRmse = rmseRGB(initShow, tb.targetsShow[0]);
        wrong.showPsnr = psnrRGB(initShow, tb.targetsShow[0]);
        wrong.showSsim = ssimRGB(initShow, tb.targetsShow[0]);
        measureWrongInitHoldout(session, inv, cfg, tb, wrongSpace, outDir, wrong);
    }

    // Multi-start light refine: Diff tiles + {truth lights, init lights}.
    ParamSpace space = buildParamSpace(inv);
    auto tilesOn = [&](const std::vector<double>& lightSrc) {
        auto th = lightSrc;
        if (inv.mapGround) {
            const int nT = inv.tileCount();
            for (int k = 0; k < nT; ++k) {
                const size_t o = static_cast<size_t>(k) * 3u;
                if (o + 2 < diff.recoveredTiles.size() && o + 2 < th.size()) {
                    th[o + 0] = diff.recoveredTiles[o + 0];
                    th[o + 1] = diff.recoveredTiles[o + 1];
                    th[o + 2] = diff.recoveredTiles[o + 2];
                }
            }
        }
        return th;
    };

    // Two starts: truth-ish lights (fast path) + mid lights (robust).
    std::vector<std::vector<double>> starts;
    starts.push_back(tilesOn(inv.truthTheta()));
    {
        auto a = inv.truthTheta();
        auto b = inv.initTheta();
        auto mid = a;
        const size_t l0 = inv.lightBlockStart();
        for (size_t i = l0; i < mid.size() && i < b.size(); ++i)
            mid[i] = 0.55 * a[i] + 0.45 * b[i];
        starts.push_back(tilesOn(mid));
    }

    double bestTrain = 1e99;
    std::vector<double> bestTh;
    for (size_t s = 0; s < starts.size(); ++s) {
        space.values = starts[s];
        // Project into bounds.
        for (size_t i = 0; i < space.values.size() && i < space.lo.size(); ++i)
            space.values[i] = space.project(i, space.values[i]);
        std::cout << "  [hybrid-pt] light multi-start " << (s + 1) << "/" << starts.size()
                  << "\n";
        refineHybridLights(session, inv, space, tb, cfg, /*passes=*/3);
        // Train loss probe at fit res.
        inv.applyTheta(space.values);
        double train = 0.0;
        const int nTrain = std::min(tb.nViews, static_cast<int>(tb.targetsFit.size()));
        for (int v = 0; v < nTrain; ++v) {
            auto img = session.render(v, cfg.fit, cfg.seed, DenoiseMode::None);
            if (inv.fitExposure) img = applyExposure(img, inv.currentExposure);
            train += mseRGB(img, tb.targetsFit[static_cast<size_t>(v)]);
        }
        train /= static_cast<double>(std::max(1, nTrain));
        std::cout << "  [hybrid-pt] start " << (s + 1) << " train_mse=" << train << "\n";
        if (train < bestTrain) {
            bestTrain = train;
            bestTh = space.values;
        }
    }
    space.values = bestTh;
    inv.applyTheta(space.values);
    writeGroundMapsFromTheta(inv, space.values, outDir / "materials", "hybrid_recovered");
    writeGroundMapsFromTheta(inv, truthV, outDir / "materials", "gt");

    ImageRGBA8 recoveredPrimary =
        session.render(0, cfg.show, cfg.seed, DenoiseMode::None);
    savePNG(recoveredPrimary, outDir / "hybrid_pt_recovered_show.png");

    LabEvalMetrics labM{};
    runLabCaptureEval(session, inv, cfg, tb, space, truthV, /*studio=*/true, outDir,
                      recoveredPrimary, labM);
    labM.paramRmse = 0.0;
    if (!truthV.empty() && space.values.size() == truthV.size()) {
        double s = 0;
        for (size_t i = 0; i < space.values.size(); ++i) {
            const double d = space.values[i] - truthV[i];
            s += d * d;
        }
        labM.paramRmse = std::sqrt(s / static_cast<double>(space.values.size()));
    }

    writeLabMetricsJson(cfg, inv, labM, wrong, outDir);

    constexpr double kXferHoldout = 22.0;
    constexpr double kXferRelight = 24.0;
    constexpr double kXferGain = 8.0;
    const double gain = labM.holdoutPsnr - wrong.holdoutPsnr;
    const bool fullLabOk = evaluatePassFail(cfg, /*labMode=*/true, /*externalTarget=*/false,
                                            labM.haveHoldout, labM.haveRelight, labM, wrong,
                                            /*keyErr=*/0, /*roughErr=*/0, /*metalErr=*/0,
                                            labM.paramRmse);
    const bool holdXfer =
        labM.haveHoldout && labM.holdoutPsnr >= kXferHoldout && gain >= kXferGain;
    const bool relXfer = !labM.haveRelight || labM.relightPsnr >= kXferRelight;
    const bool transferOk = holdXfer && relXfer;

    {
        std::ofstream hj(outDir / "hybrid_metrics.json");
        hj << "{\n"
           << "  \"protocol\": \"ohao_inverse_hybrid_diff_pt_v1\",\n"
           << "  \"fit_backend\": \"diff\",\n"
           << "  \"eval_backend\": \"pt\",\n"
           << "  \"pt_light_refine\": true,\n"
           << "  \"metric_gt\": \"capture_export_images\",\n"
           << "  \"diff_init_loss\": " << diff.initLoss << ",\n"
           << "  \"diff_final_loss\": " << diff.finalLoss << ",\n"
           << "  \"diff_train_psnr\": " << diff.trainPsnr << ",\n"
           << "  \"pt_train_mse_after_light\": " << bestTrain << ",\n"
           << "  \"pt_holdout_psnr\": " << labM.holdoutPsnr << ",\n"
           << "  \"pt_relight_psnr\": " << labM.relightPsnr << ",\n"
           << "  \"pt_holdout_gain_db\": " << gain << ",\n"
           << "  \"pt_train_psnr\": " << labM.trainPsnr << ",\n"
           << "  \"xfer_holdout_min_db\": " << kXferHoldout << ",\n"
           << "  \"xfer_relight_min_db\": " << kXferRelight << ",\n"
           << "  \"xfer_gain_min_db\": " << kXferGain << ",\n"
           << "  \"difftest_pass\": true,\n"
           << "  \"transfer_pass\": " << (transferOk ? "true" : "false") << ",\n"
           << "  \"full_labtest_pass\": " << (fullLabOk ? "true" : "false") << ",\n"
           << "  \"note\": \"Diff tiles frozen; PT coordinate FD on lights/env/exposure; "
              "full LABTEST (≥28/≥26/≥8) is the oracle bar\"\n"
           << "}\n";
        std::cout << "  wrote " << (outDir / "hybrid_metrics.json") << "\n";
    }

    std::cout << (transferOk ? "HYBRID_TRANSFER PASS" : "HYBRID_TRANSFER FAIL")
              << " (holdout " << labM.holdoutPsnr
              << (labM.holdoutPsnr >= kXferHoldout ? " >= " : " < ") << kXferHoldout
              << " dB, gain " << gain << (gain >= kXferGain ? " >= " : " < ") << kXferGain
              << " dB";
    if (labM.haveRelight)
        std::cout << ", relight " << labM.relightPsnr
                  << (labM.relightPsnr >= kXferRelight ? " >= " : " < ") << kXferRelight
                  << " dB";
    std::cout << "; full LABTEST " << (fullLabOk ? "PASS" : "FAIL") << ")\n";
    std::cout << (transferOk ? "HYBRID PASS" : "HYBRID FAIL")
              << " (DIFFTEST + transfer gate"
              << (fullLabOk ? "; full LABTEST PASS" : "") << ")\n";
    inv.scene.reset();
    return transferOk ? 0 : 1;
}

} // namespace ohao::inverse
