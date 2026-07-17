#pragma once

// High-spp pure-image polish with proximal pull + SHOW RMSE guard.

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/staged_fit.hpp"

#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace ohao::inverse {

/// Final appearance match after coarse staged FD.
/// Updates fitter.bestTheta / space on success; reverts if SHOW RMSE degrades.
inline void runVisualPolish(StagedFitter& fit, bool externalTarget,
                            const std::vector<ImageRGBA8>& targetsShow, std::ofstream& traj,
                            bool& firstTraj) {
    if (!fit.cfg.visualPolish) return;
    if (!fit.targetsFit) return;

    FitConfig& cfg = fit.cfg;
    InverseScene& inv = fit.inv;
    RenderSession& session = fit.session;
    ParamSpace& space = fit.space;
    const int nViews = fit.nViews;

    // Lab: polish at capture SHOW resolution; otherwise FIT budget with spp mul.
    if (!cfg.labBundle.empty()) {
        fit.polishBudget = cfg.show;
        fit.polishBudget.spp = std::max(
            96, static_cast<int>(std::lround(static_cast<double>(cfg.show.spp) * 0.85)));
    } else {
        fit.polishBudget = cfg.fit;
        fit.polishBudget.spp = std::max(
            fit.polishBudget.spp,
            static_cast<int>(std::lround(static_cast<double>(cfg.fit.spp) * cfg.polishSppMul)));
        if (std::string_view(cfg.quality.name) == "draft") {
            fit.polishBudget.spp = std::max(fit.polishBudget.spp, 96);
        } else {
            fit.polishBudget.spp = std::max(fit.polishBudget.spp, 160);
        }
    }
    std::cout << "── visual polish targets " << fit.polishBudget.width << "x"
              << fit.polishBudget.height << " @" << fit.polishBudget.spp << " spp ──\n";

    fit.targetsPolish.resize(static_cast<size_t>(nViews));
    if (!cfg.labBundle.empty()) {
        // Full-res capture train stills (not half-res FIT targets).
        for (int v = 0; v < nViews; ++v) {
            if (static_cast<size_t>(v) < targetsShow.size())
                fit.targetsPolish[static_cast<size_t>(v)] = targetsShow[static_cast<size_t>(v)];
        }
    } else if (!externalTarget) {
        inv.applyTruth();
        for (int v = 0; v < nViews; ++v) {
            fit.targetsPolish[static_cast<size_t>(v)] =
                session.render(v, fit.polishBudget, cfg.seed, DenoiseMode::None);
        }
        inv.applyTheta(fit.bestTheta);
        space.values = fit.bestTheta;
    } else {
        for (int v = 0; v < nViews; ++v) {
            if (static_cast<size_t>(v) < fit.targetsFit->size())
                fit.targetsPolish[static_cast<size_t>(v)] =
                    (*fit.targetsFit)[static_cast<size_t>(v)];
        }
    }
    fit.usePolishTargets = true;
    const std::vector<double> prePolishTh = fit.bestTheta;
    fit.polishAnchor = fit.bestTheta;
    // Lab / external: allow freer image match (maps + lights). Synthetic selftest: tighter.
    fit.polishProx = (!cfg.labBundle.empty() || !cfg.targetImage.empty()) ? 0.02 : 0.12;
    int pIters = cfg.polishIters > 0 ? cfg.polishIters : std::max(6, (cfg.iters * 20) / 100);
    // Lab: respect CLI polish-iters (soft refine already did heavy lifting).
    if (!cfg.labBundle.empty()) pIters = std::max(pIters, 6);
    inv.applyTheta(fit.bestTheta);
    space.values = fit.bestTheta;
    fit.loss = fit.lossAt(space.values, 1.0f, 0.0, true, cfg.polishFullFrame);
    fit.bestLoss = fit.loss;
    std::cout << "C1 visual polish (pure image + proximal, spp=" << fit.polishBudget.spp
              << ", fullFrame=" << (cfg.polishFullFrame ? "yes" : "no") << ")\n";

    const auto g = fit.makeIndexGroups();
    std::vector<size_t> vpLight = g.light;
    vpLight.insert(vpLight.end(), g.env.begin(), g.env.end());
    fit.runStage("vp_lights", vpLight, std::max(5, pIters), traj, firstTraj, 0.30, 0.5, 1.0f, 0.0,
                 true, cfg.polishFullFrame, 6);
    fit.runStage("vp_albedo", g.albedo, std::max(4, pIters * 2 / 3), traj, firstTraj, 0.18, 0.55,
                 1.0f, 0.0, true, cfg.polishFullFrame, 5);
    fit.runStage("vp_brdf", g.brdf, std::max(4, pIters * 2 / 3), traj, firstTraj, 0.22, 0.45, 1.0f,
                 0.0, true, cfg.polishFullFrame, 5);
    fit.runStage("vp_pedestal", g.pedestal, std::max(3, pIters / 2), traj, firstTraj, 0.18, 0.55,
                 1.0f, 0.0, true, cfg.polishFullFrame, 4);
    std::vector<size_t> polishIdx = g.albedo;
    polishIdx.insert(polishIdx.end(), g.brdf.begin(), g.brdf.end());
    polishIdx.insert(polishIdx.end(), g.light.begin(), g.light.end());
    polishIdx.insert(polishIdx.end(), g.env.begin(), g.env.end());
    fit.runStage("vp_joint", polishIdx, std::max(4, pIters / 2), traj, firstTraj, 0.10, 0.4, 1.0f,
                 0.0, true, cfg.polishFullFrame, 5);
    fit.polishProx = 0.0;
    fit.polishAnchor.clear();
    fit.usePolishTargets = false;

    // Guard: pick θ with better SHOW match (what the user actually sees).
    if (!targetsShow.empty() && !prePolishTh.empty()) {
        inv.applyTheta(prePolishTh);
        ImageRGBA8 preShow = session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        if (inv.fitExposure) preShow = applyExposure(preShow, inv.currentExposure);
        const double preRmse = rmseRGB(preShow, targetsShow[0]);

        inv.applyTheta(fit.bestTheta);
        ImageRGBA8 postShow = session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        if (inv.fitExposure) postShow = applyExposure(postShow, inv.currentExposure);
        const double postRmse = rmseRGB(postShow, targetsShow[0]);
        std::cout << "  polish SHOW RMSE pre=" << preRmse << " post=" << postRmse << "\n";
        if (postRmse > preRmse * 1.02) {
            std::cout << "  polish reverted (SHOW got worse)\n";
            fit.bestTheta = prePolishTh;
            space.values = prePolishTh;
        }
    }
}

} // namespace ohao::inverse
