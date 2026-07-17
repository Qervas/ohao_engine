#pragma once

// Inverse-fit orchestrator (thin): setup → targets → staged FD → polish → lab eval.
// Heavy logic lives in fit_targets / fit_lab_eval / staged_fit / backend/*.

#include "inverse/backend/image_formation.hpp"
#include "inverse/backend/pt_formation.hpp"
#include "inverse/hybrid_eval.hpp"
#include "inverse/diff_fit.hpp"
#include "inverse/export_capture.hpp"
#include "inverse/export_dataset.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/fit_lab_eval.hpp"
#include "inverse/fit_targets.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/param_space.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"
#include "inverse/staged_fit.hpp"
#include "inverse/visual_polish.hpp"

#include "gpu/vulkan/renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "scene/component/light_component.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ohao::inverse {

[[nodiscard]] inline int runInverseFit(FitConfig cfg) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    const bool wantCornell = (cfg.scene == "cornell" || cfg.preset == "cornell");
    applyPreset(cfg);
    if (wantCornell) {
        cfg.scene = "cornell";
        cfg.preset = "cornell";
        cfg.maskX = 0.40;
        cfg.maskYMin = 0.0;
        cfg.presetNote = "classic cornell box";
    }
    resolveAssetFallbacks(cfg);

    const bool studio = (cfg.scene != "cornell");
    if (studio && cfg.maskYMin <= 0.0) cfg.maskYMin = 0.25;
    if (!studio) cfg.fitPedestal = false;

    // Lab: map_res from capture.json before scene build.
    if (!cfg.labBundle.empty()) {
        std::filesystem::path cap = cfg.labBundle;
        if (!std::filesystem::exists(cap / "capture.json") &&
            std::filesystem::exists(cap / "capture" / "capture.json")) {
            cap = cap / "capture";
        }
        const auto manPath = cap / "capture.json";
        if (std::filesystem::exists(manPath)) {
            std::ifstream manIn(manPath);
            std::stringstream buf;
            buf << manIn.rdbuf();
            const std::string man = buf.str();
            const auto key = man.find("\"map_res\"");
            if (key != std::string::npos) {
                const auto colon = man.find(':', key);
                if (colon != std::string::npos) {
                    cfg.mapRes =
                        std::clamp(std::atoi(man.c_str() + static_cast<long>(colon + 1)), 1, 8);
                    cfg.mapGround = cfg.mapRes >= 2;
                }
            }
        }
    }

    InverseScene inv =
        studio ? InverseScene::buildStudio(cfg) : InverseScene::buildCornell(cfg);
    if (!inv.primaryMat) {
        std::cerr << "FATAL: optimizable primary material missing\n";
        return 1;
    }

    std::cout << "OHAO inverse_fit — multi-param IR\n";
    std::cout << "  preset=" << cfg.preset << "  (" << cfg.presetNote << ")\n";
    std::cout << "  model=" << cfg.modelPath << "\n";
    std::cout << "  backend=" << inverseBackendName(cfg.backend) << "  θ dims=" << inv.thetaDims()
              << "\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << cfg.numViews
              << "  quality=" << cfg.quality.name
              << (cfg.mapGround ? "  map-ground" : "")
              << (cfg.multiStart > 1 ? ("  multi-start=" + std::to_string(cfg.multiStart)) : "")
              << (!cfg.targetImage.empty() ? "  target-image" : "") << "\n";
    std::cout << "  FIT " << cfg.fit.width << "x" << cfg.fit.height << " @" << cfg.fit.spp
              << "  SHOW " << cfg.show.width << "x" << cfg.show.height << " @" << cfg.show.spp
              << "  denoise SHOW=" << denoiseModeName(cfg.showDenoise) << " FIT=none\n";
    if (cfg.exportDataset > 0) std::cout << "  mode=export-dataset N=" << cfg.exportDataset << "\n";
    if (cfg.exportCapture) std::cout << "  mode=export-capture\n";
    if (!cfg.labBundle.empty()) std::cout << "  mode=lab-bundle " << cfg.labBundle << "\n";

    if (cfg.backend == InverseBackend::Diff) {
        // Diff-IR albedo inverse (Deferred map SoT selftest).
        return runDiffFit(std::move(cfg));
    }
    if (cfg.backend == InverseBackend::Hybrid) {
        // Diff fit → PT capture-gated holdout/relight (requires --lab-bundle).
        return runHybridDiffPt(std::move(cfg));
    }

    VulkanRenderer renderer(cfg.show.width, cfg.show.height);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    int nViews = std::min(cfg.numViews, static_cast<int>(inv.views.size()));
    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.setRenderSeed(cfg.seed);
    if (studio) applyEnv(renderer, inv.envPath);

    RenderSession session{renderer, inv, false};
    PtImageFormation formation(session);

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir);

    if (cfg.exportDataset > 0) {
        const int rc = runExportDataset(cfg, inv, session, studio, outDir);
        inv.scene.reset();
        return rc;
    }
    if (cfg.exportCapture) {
        const int rc = runExportCapture(cfg, inv, session, studio, outDir);
        inv.scene.reset();
        return rc;
    }

    inv.applyTruth();
    const auto truthV = inv.truthTheta();
    FitTargetBundle tb;
    auto t0 = std::chrono::steady_clock::now();
    if (const int rc = loadFitTargets(cfg, inv, session, tb, nViews, truthV, outDir); rc != 0) {
        inv.scene.reset();
        return rc;
    }
    nViews = tb.nViews;
    const bool labMode = tb.labMode;
    const bool externalTarget = tb.externalTarget;
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  targets done in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms\n";

    // NN prior path (optional).
    if (!cfg.nnModelPath.empty() && cfg.thetaInitPath.empty()) {
        if (!std::filesystem::exists(cfg.nnModelPath)) {
            std::cerr << "FATAL: --nn-model not found: " << cfg.nnModelPath << "\n";
            inv.scene.reset();
            return 1;
        }
        const auto fitTargetPath = outDir / "target_fit.png";
        if (tb.targetsFit[0].empty() || !savePNG(tb.targetsFit[0], fitTargetPath)) {
            if (!savePNG(tb.targetsShow[0], fitTargetPath)) {
                std::cerr << "FATAL: could not write FIT target for NN infer\n";
                inv.scene.reset();
                return 1;
            }
        }
        const auto thetaPath = outDir / "theta_prior.json";
        std::filesystem::path inferScript = "tools/inverse_c1/infer.py";
        if (!std::filesystem::exists(inferScript))
            inferScript = std::filesystem::path("..") / inferScript;
        std::ostringstream cmd;
        cmd << cfg.nnPython << " " << inferScript.string() << " --model "
            << std::filesystem::absolute(cfg.nnModelPath).string() << " --image "
            << std::filesystem::absolute(fitTargetPath).string() << " --out "
            << std::filesystem::absolute(thetaPath).string();
        std::cout << "C1 NN infer: " << cmd.str() << "\n";
        const int rc = std::system(cmd.str().c_str());
        if (rc != 0 || !std::filesystem::exists(thetaPath)) {
            std::cerr << "FATAL: NN infer failed (rc=" << rc << ")\n";
            inv.scene.reset();
            return 1;
        }
        cfg.thetaInitPath = thetaPath.string();
    }

    ParamSpace space = buildParamSpace(inv);
    const auto initV = inv.initTheta();
    StagedFitter fit{cfg, inv, session, space, nViews, &tb.targetsFit};
    fit.initIndices();
    fit.polishBudget = cfg.fit;

    std::cout << "SHOW init (wrong multi-param guess)...\n";
    inv.applyTheta(space.values);
    writeGroundMapsFromTheta(inv, space.values, outDir / "materials", "init");
    ImageRGBA8 wrongInitShow =
        formation.forward({0, cfg.show, cfg.seed, labMode ? DenoiseMode::None : cfg.showDenoise});
    if (inv.fitExposure) wrongInitShow = applyExposure(wrongInitShow, inv.currentExposure);
    savePNG(wrongInitShow, outDir / "init_show.png");
    savePNG(wrongInitShow, outDir / "init_wrong_show.png");
    LabWrongInitMetrics wrong{};
    wrong.showRmse = rmseRGB(wrongInitShow, tb.targetsShow[0]);
    wrong.showPsnr = psnrRGB(wrongInitShow, tb.targetsShow[0]);
    wrong.showSsim = ssimRGB(wrongInitShow, tb.targetsShow[0]);
    std::cout << "  wrong-init SHOW RMSE vs target=" << wrong.showRmse
              << "  PSNR=" << wrong.showPsnr << "  SSIM=" << wrong.showSsim << "\n";
    measureWrongInitHoldout(session, inv, cfg, tb, space, outDir, wrong);

    bool usedNnPrior = false;
    if (!cfg.thetaInitPath.empty()) {
        std::vector<double> nnTh;
        if (!loadThetaInit(cfg.thetaInitPath, nnTh)) {
            std::cerr << "FATAL: failed to parse --theta-init " << cfg.thetaInitPath << "\n";
            inv.scene.reset();
            return 1;
        }
        if (nnTh.size() + 1 == space.size() && inv.fitExposure) {
            const double e0 = cfg.exposure > 0.0f ? static_cast<double>(cfg.exposure) : 1.0;
            nnTh.push_back(e0);
        }
        if (nnTh.size() != space.size()) {
            std::cerr << "FATAL: --theta-init dims " << nnTh.size() << " != space " << space.size()
                      << "\n";
            inv.scene.reset();
            return 1;
        }
        for (size_t i = 0; i < space.size(); ++i)
            space.values[i] = space.project(i, nnTh[i]);
        usedNnPrior = true;
        if (cfg.nnSkipMultiStart) cfg.multiStart = 1;
        std::cout << "C1 NN prior loaded from " << cfg.thetaInitPath
                  << "  θ=" << formatTheta(space.values) << "\n";
        if (cfg.preset == "mirror" || cfg.preset == "spheres") {
            const size_t mi = inv.mapGround ? 13u : 4u;
            const size_t ri = inv.mapGround ? 12u : 3u;
            const double tm = (cfg.preset == "mirror") ? 0.90 : 0.55;
            const double tr = (cfg.preset == "mirror") ? 0.08 : 0.18;
            if (space.size() > mi && space.values[mi] < tm - 0.20) {
                space.values[mi] = space.project(mi, tm);
                space.values[ri] = space.project(ri, tr);
            }
        }
        inv.applyTheta(space.values);
        ImageRGBA8 nnShow = formation.forward({0, cfg.show, cfg.seed, cfg.showDenoise});
        if (inv.fitExposure) nnShow = applyExposure(nnShow, inv.currentExposure);
        savePNG(nnShow, outDir / "nn_prior_show.png");
    }

    fit.highlightScore = targetHighlightScore(tb.targetsFit[0], cfg.maskX, cfg.maskYMin);
    if (cfg.preset == "mirror" || cfg.preset == "spheres")
        fit.highlightScore = std::max(fit.highlightScore, 0.28);

    inv.applyTheta(space.values);
    ImageRGBA8 initShow = formation.forward({0, cfg.show, cfg.seed, cfg.showDenoise});
    if (inv.fitExposure) initShow = applyExposure(initShow, inv.currentExposure);
    savePNG(initShow, usedNnPrior ? outDir / "init_opt_start_show.png" : outDir / "init_show.png");

    fit.loss = fit.lossAt(space.values);
    std::cout << "  fit-start FIT loss=" << fit.loss
              << "  SHOW RMSE vs target=" << rmseRGB(initShow, tb.targetsShow[0]) << "\n";

    fit.multiStartProbe(initV);
    inv.applyTheta(space.values);
    ImageRGBA8 postMsShow = formation.forward({0, cfg.show, cfg.seed, cfg.showDenoise});
    if (inv.fitExposure) postMsShow = applyExposure(postMsShow, inv.currentExposure);
    const double showAfterMs = rmseRGB(postMsShow, tb.targetsShow[0]);
    fit.loss = fit.lossAt(space.values);
    std::cout << "  post-multi-start FIT loss=" << fit.loss << " SHOW RMSE=" << showAfterMs
              << "\n";

    const bool labSoftStart = labMode;
    const bool seedLikeNn = usedNnPrior || labSoftStart;
    const char* schedName = labSoftStart ? "lab_multistart_soft"
                           : usedNnPrior ? "nn_seeded_or_soft"
                                         : "multistart_staged";

    std::ofstream traj(outDir / "trajectory.json");
    traj << "{\n  \"scene\": \"" << cfg.scene << "\",\n  \"quality\": \"" << cfg.quality.name
         << "\",\n  \"views\": " << nViews << ",\n  \"dims\": " << space.size()
         << ",\n  \"schedule\": \"" << schedName << "\",\n"
         << "  \"backend\": \"" << inverseBackendName(cfg.backend) << "\",\n"
         << "  \"nn_prior\": " << (usedNnPrior ? "true" : "false")
         << ",\n  \"map_ground\": " << (inv.mapGround ? "true" : "false")
         << ",\n  \"lab_bundle\": " << (labMode ? "true" : "false") << ",\n  \"iters\": [\n";
    bool firstTraj = true;

    const auto fitStart = std::chrono::steady_clock::now();
    fit.runSchedule(seedLikeNn, showAfterMs, traj, firstTraj);
    if (labMode) {
        cfg.visualPolish = true;
        if (cfg.polishIters <= 0) cfg.polishIters = 8;
    }
    runVisualPolish(fit, externalTarget || labMode, tb.targetsShow, traj, firstTraj);
    space.values = fit.bestTheta;
    fit.loss = fit.bestLoss;
    traj << "\n  ],\n  \"best_loss\": " << fit.bestLoss << "\n}\n";
    traj.close();
    const auto fitEnd = std::chrono::steady_clock::now();

    std::cout << "SHOW recovered (all views)...\n";
    inv.applyTheta(space.values);
    ImageRGBA8 recoveredPrimary;
    for (int v = 0; v < nViews; ++v) {
        auto img = formation.forward({v, cfg.show, cfg.seed, cfg.showDenoise});
        if (inv.fitExposure) img = applyExposure(img, inv.currentExposure);
        savePNG(img, outDir / (std::string("recovered_") +
                               inv.views[static_cast<size_t>(v)].name + ".png"));
        if (v == 0) recoveredPrimary = std::move(img);
    }
    savePNG(recoveredPrimary, outDir / "recovered_show.png");
    writeGroundMapsFromTheta(inv, space.values, outDir / "materials", "recovered");
    writeGroundMapsFromTheta(inv, truthV, outDir / "materials", "gt");

    LabEvalMetrics labM{};
    runLabCaptureEval(session, inv, cfg, tb, space, truthV, studio, outDir, recoveredPrimary,
                      labM);
    if (!labMode) {
        labM.trainPsnr = psnrRGB(recoveredPrimary, tb.targetsShow[0]);
        labM.trainSsim = ssimRGB(recoveredPrimary, tb.targetsShow[0]);
        labM.showRmse = rmseRGB(recoveredPrimary, tb.targetsShow[0]);
    }

    // Non-lab relight showcase.
    if (studio && inv.keyLight && !externalTarget && !labMode) {
        const float savedKey = inv.keyLight->getIntensity();
        inv.keyLight->setIntensity(savedKey * 2.5f);
        (void)renderer.updateRTLightParams();
        savePNG(formation.forward({0, cfg.show, cfg.seed, cfg.showDenoise}),
                outDir / "recovered_relight.png");
        inv.applyTruth();
        inv.keyLight->setIntensity(inv.truthKeyI * 2.5f);
        (void)renderer.updateRTLightParams();
        savePNG(formation.forward({0, cfg.show, cfg.seed, cfg.showDenoise}),
                outDir / "truth_relight.png");
    }

    const double paramErr = externalTarget ? 0.0 : space.l2To(truthV);
    labM.paramRmse =
        externalTarget ? 0.0 : paramErr / std::sqrt(static_cast<double>(space.size()));

    std::cout << "\n=== inverse_fit result ===\n";
    std::cout << "  recovered θ = " << formatTheta(space.values) << "\n";
    std::cout << "  SHOW RMSE = " << labM.showRmse << "  PSNR = " << labM.trainPsnr
              << "  SSIM = " << labM.trainSsim << "\n";
    if (labM.haveHoldout)
        std::cout << "  holdout capture PSNR = " << labM.holdoutPsnr << "\n";
    if (labM.haveRelight)
        std::cout << "  relight capture PSNR = " << labM.relightPsnr << "\n";
    if (labMode && labM.haveHoldout)
        std::cout << "  holdout PSNR gain vs wrong-init (capture) = "
                  << (labM.holdoutPsnr - wrong.holdoutPsnr) << " dB\n";
    std::cout << "  fit wall time = "
              << std::chrono::duration_cast<std::chrono::seconds>(fitEnd - fitStart).count()
              << " s\n  wrote " << outDir << "/\n";

    if (labMode) writeLabMetricsJson(cfg, inv, labM, wrong, outDir);

    {
        std::filesystem::path cmpScript = "tools/inverse_c1/make_compare.py";
        if (!std::filesystem::exists(cmpScript))
            cmpScript = std::filesystem::path("..") / cmpScript;
        if (std::filesystem::exists(cmpScript)) {
            std::ostringstream cmd;
            cmd << cfg.nnPython << " " << cmpScript.string() << " "
                << std::filesystem::absolute(outDir).string();
            (void)std::system(cmd.str().c_str());
        }
    }

    double keyErr = 0.0, roughErr = 0.0, metalErr = 0.0;
    if (!externalTarget && inv.fitKeyLight && inv.keyLight) {
        const size_t lo = inv.lightBlockStart();
        if (truthV.size() > lo && space.size() > lo) {
            keyErr = std::abs(space.values[lo] - truthV[lo]) * InverseScene::kKeyIScale;
        }
    }
    if (!externalTarget && space.size() > fit.metalIdx && truthV.size() > fit.metalIdx) {
        roughErr = std::abs(space.values[fit.roughIdx] - truthV[fit.roughIdx]);
        metalErr = std::abs(space.values[fit.metalIdx] - truthV[fit.metalIdx]);
    }
    if (!inv.fitKeyLight) keyErr = 0.0;

    const bool ok = evaluatePassFail(cfg, labMode, externalTarget, labM.haveHoldout,
                                     labM.haveRelight, labM, wrong, keyErr, roughErr, metalErr,
                                     labM.paramRmse);
    inv.scene.reset();
    return ok ? 0 : 1;
}

} // namespace ohao::inverse
