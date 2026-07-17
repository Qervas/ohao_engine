#pragma once

// Lab holdout / relight eval + lab_metrics.json + LABTEST / SELFTEST gates.

#include "inverse/export_capture.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/fit_targets.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/param_space.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/rt/denoise/denoise_types.hpp"
#include "scene/component/light_component.hpp"

#include "gpu/vulkan/renderer.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::inverse {

struct LabWrongInitMetrics {
    double showRmse{0}, showPsnr{0}, showSsim{0};
    double holdoutPsnr{0}, holdoutSsim{0}, holdoutRmse{0}, holdoutPsnrLive{0};
};

struct LabEvalMetrics {
    double holdoutRmse{0}, holdoutPsnr{0}, holdoutSsim{0};
    double holdoutPsnrLive{0}, holdoutSsimLive{0}, oracleHoldoutPsnr{0};
    bool haveHoldout{false};
    double relightRmse{0}, relightPsnr{0}, relightSsim{0}, relightPsnrLive{0};
    bool haveRelight{false};
    double trainPsnr{0}, trainSsim{0}, trainPsnrLive{0};
    double showRmse{0};
    double paramRmse{0};
};

inline void measureWrongInitHoldout(RenderSession& session, InverseScene& inv,
                                    const FitConfig& cfg, const FitTargetBundle& tb,
                                    ParamSpace& space, const std::filesystem::path& outDir,
                                    LabWrongInitMetrics& w) {
    if (!tb.labMode || tb.holdoutShow.empty()) return;
    double sumP = 0, sumS = 0, sumR = 0, sumPLive = 0;
    int nH = 0;
    for (size_t i = 0; i < tb.holdoutShow.size(); ++i) {
        inv.applyTheta(space.values);
        ImageRGBA8 pred =
            session.render(tb.holdoutViewIdx[i], cfg.show, cfg.seed, DenoiseMode::None);
        inv.applyTruth();
        ImageRGBA8 gtLive =
            session.render(tb.holdoutViewIdx[i], cfg.show, cfg.seed, DenoiseMode::None);
        savePNG(pred, outDir / (std::string("init_holdout_") + std::to_string(i) + ".png"));
        sumP += psnrRGB(pred, tb.holdoutShow[i]);
        sumS += ssimRGB(pred, tb.holdoutShow[i]);
        sumR += rmseRGB(pred, tb.holdoutShow[i]);
        sumPLive += psnrRGB(pred, gtLive);
        ++nH;
    }
    inv.applyTheta(space.values);
    if (nH > 0) {
        w.holdoutPsnr = sumP / nH;
        w.holdoutSsim = sumS / nH;
        w.holdoutRmse = sumR / nH;
        w.holdoutPsnrLive = sumPLive / nH;
        std::cout << "  wrong-init holdout PSNR=" << w.holdoutPsnr << " SSIM=" << w.holdoutSsim
                  << " RMSE=" << w.holdoutRmse << " (vs capture GT; live diag "
                  << w.holdoutPsnrLive << " dB)\n";
    }
}

/// Holdout + relight vs capture GT; updates recovered primary for lab train metric.
inline void runLabCaptureEval(RenderSession& session, InverseScene& inv, const FitConfig& cfg,
                              const FitTargetBundle& tb, const ParamSpace& space,
                              const std::vector<double>& /*truthV*/, bool studio,
                              const std::filesystem::path& outDir, ImageRGBA8& recoveredPrimary,
                              LabEvalMetrics& m) {
    const RenderBudget evalShow = cfg.show;
    constexpr DenoiseMode kLabEvalDenoise = DenoiseMode::None;

    if (tb.labMode && !tb.holdoutShow.empty()) {
        double sumR = 0, sumP = 0, sumS = 0, sumPLive = 0, sumSLive = 0, sumOracle = 0;
        int nH = 0;
        for (size_t i = 0; i < tb.holdoutShow.size(); ++i) {
            const int vIdx = tb.holdoutViewIdx[i];
            inv.applyTheta(space.values);
            ImageRGBA8 pred = session.render(vIdx, evalShow, cfg.seed, kLabEvalDenoise);
            inv.applyTruth();
            ImageRGBA8 gtLive = session.render(vIdx, evalShow, cfg.seed, kLabEvalDenoise);
            savePNG(pred, outDir / (std::string("recovered_holdout_") + std::to_string(i) + ".png"));
            savePNG(gtLive,
                    outDir / (std::string("target_holdout_live_") + std::to_string(i) + ".png"));
            savePNG(tb.holdoutShow[i],
                    outDir / (std::string("target_holdout_") + std::to_string(i) + ".png"));
            const double r = rmseRGB(pred, tb.holdoutShow[i]);
            const double p = psnrRGB(pred, tb.holdoutShow[i]);
            const double s = ssimRGB(pred, tb.holdoutShow[i]);
            const double pLive = psnrRGB(pred, gtLive);
            const double sLive = ssimRGB(pred, gtLive);
            const double pOracle = psnrRGB(gtLive, tb.holdoutShow[i]);
            if (std::isfinite(r)) {
                sumR += r;
                sumP += p;
                sumS += s;
                sumPLive += pLive;
                sumSLive += sLive;
                sumOracle += pOracle;
                ++nH;
            }
            std::cout << "  holdout[" << i << "] view=" << vIdx << " capture PSNR=" << p
                      << " SSIM=" << s << " RMSE=" << r << "  (live diag PSNR=" << pLive
                      << ", oracle truth-vs-capture=" << pOracle << ")\n";
        }
        if (nH > 0) {
            const double invN = 1.0 / static_cast<double>(nH);
            m.holdoutRmse = sumR * invN;
            m.holdoutPsnr = sumP * invN;
            m.holdoutSsim = sumS * invN;
            m.holdoutPsnrLive = sumPLive * invN;
            m.holdoutSsimLive = sumSLive * invN;
            m.oracleHoldoutPsnr = sumOracle * invN;
            m.haveHoldout = true;
            std::cout << "  holdout capture PSNR = " << m.holdoutPsnr
                      << "  SSIM = " << m.holdoutSsim << "  RMSE = " << m.holdoutRmse
                      << "  (live diag " << m.holdoutPsnrLive << " dB, oracle ceiling "
                      << m.oracleHoldoutPsnr << " dB)\n";
        }
        inv.applyTheta(space.values);
    }

    if (tb.labMode && studio && !inv.relightEnvPath.empty() &&
        std::filesystem::exists(inv.relightEnvPath)) {
        session.rebindEnv(inv.relightEnvPath);
        inv.applyTheta(space.values);
        ImageRGBA8 pred = session.render(0, evalShow, cfg.seed + 17u, kLabEvalDenoise);
        inv.applyTruth();
        ImageRGBA8 gtLive = session.render(0, evalShow, cfg.seed + 17u, kLabEvalDenoise);
        savePNG(pred, outDir / "recovered_lab_relight.png");
        savePNG(gtLive, outDir / "target_lab_relight_live.png");
        if (!tb.relightShowGt.empty()) {
            savePNG(tb.relightShowGt[0], outDir / "target_lab_relight.png");
            m.relightRmse = rmseRGB(pred, tb.relightShowGt[0]);
            m.relightPsnr = psnrRGB(pred, tb.relightShowGt[0]);
            m.relightSsim = ssimRGB(pred, tb.relightShowGt[0]);
            m.haveRelight = std::isfinite(m.relightRmse);
        }
        m.relightPsnrLive = psnrRGB(pred, gtLive);
        std::cout << "  relight capture PSNR = " << m.relightPsnr << "  SSIM = " << m.relightSsim
                  << "  RMSE = " << m.relightRmse << "  (live diag " << m.relightPsnrLive
                  << " dB)\n";
        session.rebindEnv(inv.envPath);
        inv.applyTheta(space.values);
    }

    if (tb.labMode) {
        inv.applyTheta(space.values);
        recoveredPrimary = session.render(0, evalShow, cfg.seed, kLabEvalDenoise);
        inv.applyTruth();
        ImageRGBA8 trainGtLive = session.render(0, evalShow, cfg.seed, kLabEvalDenoise);
        savePNG(recoveredPrimary, outDir / "recovered_show.png");
        savePNG(trainGtLive, outDir / "target_train_live.png");
        inv.applyTheta(space.values);
    }
    m.trainPsnr = psnrRGB(recoveredPrimary, tb.targetsShow[0]);
    m.trainSsim = ssimRGB(recoveredPrimary, tb.targetsShow[0]);
    m.trainPsnrLive = m.trainPsnr;
    if (tb.labMode && std::filesystem::exists(outDir / "target_train_live.png")) {
        ImageRGBA8 trainGtLive = loadPNG(outDir / "target_train_live.png");
        if (!trainGtLive.empty()) m.trainPsnrLive = psnrRGB(recoveredPrimary, trainGtLive);
    }
    m.showRmse = rmseRGB(recoveredPrimary, tb.targetsShow[0]);
}

inline void writeLabMetricsJson(const FitConfig& cfg, const InverseScene& inv,
                                const LabEvalMetrics& m, const LabWrongInitMetrics& w,
                                const std::filesystem::path& outDir) {
    std::ofstream mj(outDir / "lab_metrics.json");
    mj << "{\n"
       << "  \"protocol\": \"ohao_inverse_lab_frontier_v1\",\n"
       << "  \"quality\": \"" << cfg.quality.name << "\",\n"
       << "  \"eval_show_spp\": " << cfg.show.spp << ",\n"
       << "  \"eval_show_wh\": [" << cfg.show.width << ", " << cfg.show.height << "],\n"
       << "  \"map_res\": " << inv.mapRes << ",\n"
       << "  \"train_only_loss\": true,\n"
       << "  \"lpips\": null,\n"
       << "  \"lpips_note\": \"LPIPS optional; not bundled — PSNR+SSIM are primary\",\n"
       << "  \"metric_gt\": \"capture_export_images\",\n"
       << "  \"metric_gt_note\": \"LABTEST gates use capture holdout/relight PNGs; "
          "live_* fields are diagnostics only\",\n"
       << "  \"train\": {\"rmse\": " << m.showRmse << ", \"psnr\": " << m.trainPsnr
       << ", \"ssim\": " << m.trainSsim << ", \"psnr_live_diag\": " << m.trainPsnrLive << "},\n"
       << "  \"wrong_init_train\": {\"rmse\": " << w.showRmse << ", \"psnr\": " << w.showPsnr
       << ", \"ssim\": " << w.showSsim << "},\n"
       << "  \"holdout\": {\"rmse\": " << m.holdoutRmse << ", \"psnr\": " << m.holdoutPsnr
       << ", \"ssim\": " << m.holdoutSsim << ", \"psnr_live_diag\": " << m.holdoutPsnrLive
       << ", \"ssim_live_diag\": " << m.holdoutSsimLive
       << ", \"oracle_truth_vs_capture_psnr\": " << m.oracleHoldoutPsnr << "},\n"
       << "  \"wrong_init_holdout\": {\"rmse\": " << w.holdoutRmse
       << ", \"psnr\": " << w.holdoutPsnr << ", \"ssim\": " << w.holdoutSsim
       << ", \"psnr_live_diag\": " << w.holdoutPsnrLive << "},\n"
       << "  \"relight\": {\"rmse\": " << m.relightRmse << ", \"psnr\": " << m.relightPsnr
       << ", \"ssim\": " << m.relightSsim << ", \"psnr_live_diag\": " << m.relightPsnrLive
       << "},\n"
       << "  \"holdout_psnr_gain_db\": " << (m.holdoutPsnr - w.holdoutPsnr) << ",\n"
       << "  \"param_rmse\": " << m.paramRmse << "\n"
       << "}\n";
    std::cout << "  wrote " << (outDir / "lab_metrics.json") << "\n";
}

/// Returns true if pass (LABTEST or SELFTEST).
[[nodiscard]] inline bool evaluatePassFail(const FitConfig& cfg, bool labMode, bool externalTarget,
                                           bool haveHoldout, bool haveRelight,
                                           const LabEvalMetrics& m, const LabWrongInitMetrics& w,
                                           double keyErr, double roughErr, double metalErr,
                                           double paramRmse) {
    constexpr double kLabHoldoutPsnr = 28.0;
    constexpr double kLabRelightPsnr = 26.0;
    constexpr double kLabHoldoutGain = 8.0;
    const double kShowRmseTol =
        externalTarget
            ? ((std::string_view(cfg.quality.name) == "draft") ? 0.22 : 0.18)
            : ((std::string_view(cfg.quality.name) == "draft") ? 0.155 : 0.12);
    constexpr double kKeyITol = 12.0;
    constexpr double kParamRmseSoft = 0.48;
    const double metalTol =
        (cfg.preset == "mirror" || cfg.preset == "spheres") ? 0.35 : 0.55;
    const double roughTol = 0.40;
    const bool brdfOk = externalTarget || (roughErr < roughTol && metalErr < metalTol);
    const bool keyOk = externalTarget || keyErr < kKeyITol;
    const bool showOk = m.showRmse < kShowRmseTol;
    const bool paramSoftOk = externalTarget || paramRmse < kParamRmseSoft;
    const double holdoutGain = m.holdoutPsnr - w.holdoutPsnr;
    const bool holdoutOk =
        !labMode || !haveHoldout ||
        (m.holdoutPsnr >= kLabHoldoutPsnr && holdoutGain >= kLabHoldoutGain);
    const bool relightOk = !labMode || !haveRelight || (m.relightPsnr >= kLabRelightPsnr);
    const bool ok =
        labMode ? (holdoutOk && relightOk && haveHoldout)
                : (showOk && keyOk && paramSoftOk && brdfOk);

    if (labMode) {
        std::cout << (ok ? "LABTEST PASS" : "LABTEST FAIL")
                  << " (capture-gated holdout PSNR " << m.holdoutPsnr
                  << (m.holdoutPsnr >= kLabHoldoutPsnr ? " >= " : " < ") << kLabHoldoutPsnr
                  << " dB, gain " << holdoutGain
                  << (holdoutGain >= kLabHoldoutGain ? " >= " : " < ") << kLabHoldoutGain
                  << " dB";
        if (haveRelight)
            std::cout << ", capture-gated relight PSNR " << m.relightPsnr
                      << (m.relightPsnr >= kLabRelightPsnr ? " >= " : " < ") << kLabRelightPsnr
                      << " dB";
        std::cout << ", train capture PSNR " << m.trainPsnr << ", live diag holdout "
                  << m.holdoutPsnrLive << " dB)\n";
    } else {
        std::cout << (ok ? "SELFTEST PASS" : "SELFTEST FAIL") << " (SHOW RMSE " << m.showRmse
                  << (showOk ? " < " : " >= ") << kShowRmseTol;
        if (!externalTarget) {
            std::cout << ", key|ΔI| " << keyErr << (keyOk ? " < " : " >= ") << kKeyITol
                      << ", param RMSE " << paramRmse << (paramSoftOk ? " < " : " >= ")
                      << kParamRmseSoft << ", |Δ|rough " << roughErr << " |Δ|metal " << metalErr
                      << (brdfOk ? " ok" : " FAIL");
        }
        std::cout << ")\n";
    }
    return ok;
}

} // namespace ohao::inverse
