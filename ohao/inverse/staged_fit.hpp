#pragma once

// Staged FD+Adam machinery: loss, regularizers, multi-start, stage runner.

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/optimizer.hpp"
#include "inverse/param_space.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ohao::inverse {

/// Build ParamSpace from scene init θ with tighter light bounds.
[[nodiscard]] inline ParamSpace buildParamSpace(const InverseScene& inv) {
    ParamSpace space;
    const auto initV = inv.initTheta();
    if (inv.mapGround) {
        const int nT = inv.tileCount();
        for (int ti = 0; ti < nT; ++ti) {
            const size_t o = static_cast<size_t>(ti) * 3;
            const std::string pref = "tile" + std::to_string(ti) + ".";
            space.add(pref + "R", initV[o], 0.0, 1.0);
            space.add(pref + "G", initV[o + 1], 0.0, 1.0);
            space.add(pref + "B", initV[o + 2], 0.0, 1.0);
        }
        const size_t rb = static_cast<size_t>(nT) * 3;
        space.add("primary.rough", initV[rb], 0.04, 1.0);
        space.add("primary.metal", initV[rb + 1], 0.0, 1.0);
    } else {
        space.add("primary.R", initV[0], 0.0, 1.0);
        space.add("primary.G", initV[1], 0.0, 1.0);
        space.add("primary.B", initV[2], 0.0, 1.0);
        space.add("primary.rough", initV[3], 0.04, 1.0);
        space.add("primary.metal", initV[4], 0.0, 1.0);
    }
    size_t off = inv.primaryDims();
    if (inv.fitPedestal && inv.pedestalMat) {
        space.add("pedestal.R", initV[off], 0.0, 1.0);
        space.add("pedestal.G", initV[off + 1], 0.0, 1.0);
        space.add("pedestal.B", initV[off + 2], 0.0, 1.0);
        off += 3;
    }
    if (inv.fitKeyLight && inv.keyLight) {
        space.add("key.I_scale", initV[off], 0.12, 0.95); // ~4.8–38 intensity
        ++off;
    }
    if (inv.fitFillLight && inv.fillLight) {
        space.add("fill.I_scale", initV[off], 0.06, 0.50);
        ++off;
    }
    if (inv.fitRimLight && inv.rimLight) {
        space.add("rim.I_scale", initV[off], 0.06, 0.50);
        ++off;
    }
    if (inv.fitEnvScale) {
        space.add("env.scale", initV[off], 0.30, 1.50);
        ++off;
    }
    if (inv.fitExposure) {
        space.add("exposure", initV[off], 0.35, 2.50);
    }
    return space;
}

/// Shared FD runtime: multi-view loss, light reg, Adam stages, multi-start.
struct StagedFitter {
    FitConfig& cfg;
    InverseScene& inv;
    RenderSession& session;
    ParamSpace& space;
    int nViews{1};
    const std::vector<ImageRGBA8>* targetsFit{nullptr};

    // Visual-polish state (owned here so lossAt can switch targets).
    std::vector<ImageRGBA8> targetsPolish;
    RenderBudget polishBudget{};
    bool usePolishTargets{false};
    std::vector<double> polishAnchor;
    double polishProx{0.0};

    size_t roughIdx{3};
    size_t metalIdx{4};
    double highlightScore{0.0};

    double loss{0.0};
    std::vector<double> bestTheta;
    double bestLoss{0.0};
    int bestIter{0};

    void initIndices() {
        if (inv.mapGround) {
            const size_t rb = static_cast<size_t>(inv.tileCount()) * 3u;
            roughIdx = rb;
            metalIdx = rb + 1;
        } else {
            roughIdx = 3u;
            metalIdx = 4u;
        }
    }

    /// Near-mirror conductor floor (high metal, low rough).
    [[nodiscard]] bool isMirrorPreset() const { return cfg.preset == "mirror"; }

    /// Metal/rough chart — mid metal floor (~0.55), not full mirror.
    [[nodiscard]] bool isSpheresPreset() const { return cfg.preset == "spheres"; }

    /// Known metallic floor presets — never use soft-diffuse metal prior here.
    [[nodiscard]] bool isSpecularPreset() const {
        return isMirrorPreset() || isSpheresPreset();
    }

    /// Known product / dielectric floor packs (hero glints inflate highlight score).
    [[nodiscard]] bool isProductPreset() const {
        const auto& p = cfg.preset;
        return p == "lantern" || p == "default" || p.empty() || p == "outdoor" ||
               p == "helmet" || p == "bottle" || p == "toycar" || p == "boombox" ||
               p == "chess" || p == "cornell";
    }

    /// Strong specular target. Preset wins over image metric — product heroes
    /// (lantern chrome bits) make highlightScore look "specular" even on matte floors.
    [[nodiscard]] bool isSpecularTarget() const {
        if (isSpecularPreset()) return true;
        if (isProductPreset()) return false;
        return highlightScore > 0.32; // unknown preset / external photo
    }

    /// Product / dielectric floor — metal should stay low.
    [[nodiscard]] bool isDiffuseTarget() const {
        if (isSpecularPreset()) return false;
        if (isProductPreset()) return true;
        return highlightScore < 0.18;
    }

    /// Target metal/rough basin for specular-ish floors.
    [[nodiscard]] double targetMetal() const {
        if (isMirrorPreset()) return 0.90;
        if (isSpheresPreset()) return 0.55;
        if (isSpecularTarget()) return 0.75;
        return 0.10;
    }
    [[nodiscard]] double targetRough() const {
        if (isMirrorPreset()) return 0.08;
        if (isSpheresPreset()) return 0.18;
        if (isSpecularTarget()) return 0.14;
        return 0.40;
    }

    [[nodiscard]] double lightRegularizer(const std::vector<double>& th) const {
        if (cfg.lightReg <= 0.0) return 0.0;
        size_t i = inv.lightBlockStart();
        double reg = 0.0;
        double key = 0.0, fill = 0.0, rim = 0.0, env = 0.0;
        bool hasKey = false, hasFill = false, hasRim = false, hasEnv = false;
        if (inv.fitKeyLight && inv.keyLight && th.size() > i) {
            key = th[i++];
            hasKey = true;
            const double mid = 0.50;
            reg += (key - mid) * (key - mid);
        }
        if (inv.fitFillLight && inv.fillLight && th.size() > i) {
            fill = th[i++];
            hasFill = true;
            const double mid = 0.22;
            reg += 0.6 * (fill - mid) * (fill - mid);
        }
        if (inv.fitRimLight && inv.rimLight && th.size() > i) {
            rim = th[i++];
            hasRim = true;
            const double mid = 0.25;
            reg += 0.6 * (rim - mid) * (rim - mid);
        }
        if (inv.fitEnvScale && th.size() > i) {
            env = th[i++];
            hasEnv = true;
            const double mid = 0.95;
            reg += 0.4 * (env - mid) * (env - mid);
        }
        if (hasKey && hasFill && fill > key) reg += 2.0 * (fill - key) * (fill - key);
        if (hasKey && hasRim && rim > key) reg += 2.0 * (rim - key) * (rim - key);
        if (hasKey || hasFill || hasRim) {
            const double total =
                (hasKey ? key : 0.0) + (hasFill ? fill : 0.0) + (hasRim ? rim : 0.0);
            const double targetTotal = 0.50 + (hasFill ? 0.22 : 0.0) + (hasRim ? 0.25 : 0.0);
            reg += 0.35 * (total - targetTotal) * (total - targetTotal);
        }
        (void)hasEnv;
        (void)env;
        return cfg.lightReg * reg;
    }

    /// Multi-view hybrid + specular FIT loss.
    /// pureImage: no light/metal regularizers (final visual match).
    /// fullFrame: ignore crop mask (floor+hero+backdrop all contribute).
    [[nodiscard]] double lossAt(const std::vector<double>& th, float sppScale = 1.0f,
                                double specularW = -1.0, bool pureImage = false,
                                bool fullFrame = false) {
        inv.applyTheta(th);
        RenderBudget budget = usePolishTargets ? polishBudget : cfg.fit;
        if (!usePolishTargets && sppScale > 1.001f) {
            budget.spp = std::max(1, static_cast<int>(std::lround(budget.spp * sppScale)));
        } else if (usePolishTargets && sppScale > 1.001f) {
            budget.spp = std::max(
                1, static_cast<int>(
                       std::lround(static_cast<double>(polishBudget.spp) * sppScale)));
        }
        const double sw = (specularW >= 0.0) ? specularW : cfg.specularWeight;
        const double mx = fullFrame ? 1.0 : cfg.maskX;
        const double my = fullFrame ? 0.0 : cfg.maskYMin;
        const auto& targets = usePolishTargets ? targetsPolish : *targetsFit;
        double L = 0.0;
        double wSum = 0.0;
        for (int v = 0; v < nViews; ++v) {
            if (static_cast<size_t>(v) >= targets.size()) continue;
            // Lab trains against capture stills (exported without denoise).
            const DenoiseMode dm =
                (!cfg.labBundle.empty()) ? DenoiseMode::None : DenoiseMode::None;
            ImageRGBA8 img = session.render(v, budget, cfg.seed, dm);
            if (img.empty() || targets[static_cast<size_t>(v)].empty()) continue;
            if (inv.fitExposure) {
                img = applyExposure(img, inv.currentExposure);
            }
            // Lab pure-image: MSE-only (sharper FD for PSNR); elsewhere hybrid.
            const double m =
                pureImage
                    ? ((!cfg.labBundle.empty())
                           ? mseRGB(img, targets[static_cast<size_t>(v)], mx, my)
                           : hybridRGB(img, targets[static_cast<size_t>(v)], mx, my, 0.45))
                    : hybridSpecularRGB(img, targets[static_cast<size_t>(v)], mx, my, 0.35, sw);
            if (!std::isfinite(m)) continue;
            const double w = (v == 0) ? 1.0 : 0.5;
            L += w * m;
            wSum += w;
        }
        const double imgLoss = wSum > 0.0 ? L / wSum : 1e6;
        if (pureImage) {
            double prox = 0.0;
            if (polishProx > 0.0 && polishAnchor.size() == th.size()) {
                for (size_t i = 0; i < th.size(); ++i) {
                    const double d = th[i] - polishAnchor[i];
                    double wi = 1.0;
                    if (i < 3)
                        wi = 8.0;
                    else if (i == roughIdx || i == metalIdx)
                        wi = 3.0;
                    else if (i >= inv.primaryDims() && i < inv.lightBlockStart())
                        wi = 2.0; // pedestal
                    else
                        wi = 0.4; // lights/env freer for tone match
                    prox += wi * d * d;
                }
            }
            return imgLoss + polishProx * prox;
        }
        // Metal/rough prior — preset-aware (hero glints must not push product floors metal).
        double metalPrior = 0.0;
        if (th.size() > metalIdx) {
            const double metal = th[metalIdx];
            const double rough = th[roughIdx];
            if (isSpecularTarget()) {
                // Conductor / chart basin (preset-specific targets).
                const double targetM = targetMetal();
                const double targetR = targetRough();
                const double w = isSpecularPreset() ? 0.22 : (0.10 + 0.12 * highlightScore);
                metalPrior += w * (metal - targetM) * (metal - targetM);
                metalPrior += 0.55 * w * (rough - targetR) * (rough - targetR);
            } else if (isDiffuseTarget()) {
                // Dielectric product floor: low metal, mid-high rough.
                // Was 0.015 — too weak; FD walks metal up to match hero glints.
                const double targetM = 0.10;
                const double targetR = 0.40;
                const double w = 0.12;
                metalPrior += w * (metal - targetM) * (metal - targetM);
                if (metal > 0.35)
                    metalPrior += 0.25 * (metal - 0.35) * (metal - 0.35); // hard penalty
                metalPrior += 0.03 * (rough - targetR) * (rough - targetR);
            } else if (highlightScore < 0.22) {
                // Mixed: mild dielectric pull.
                metalPrior += 0.04 * (metal - 0.18) * (metal - 0.18);
            }
        }
        return imgLoss + lightRegularizer(th) + metalPrior;
    }

    void runStage(const char* name, const std::vector<size_t>& activeIdx, int iters,
                  std::ofstream& traj, bool& firstTraj, double lrMul = 1.0, double epsMul = 1.0,
                  float sppScale = 1.0f, double specularW = -1.0, bool pureImage = false,
                  bool fullFrame = false, int patience = 5) {
        if (activeIdx.empty() || iters <= 0) return;
        std::cout << "── stage " << name << " (" << activeIdx.size() << " params, " << iters
                  << " iters, lr×" << lrMul;
        if (sppScale > 1.001f) std::cout << ", spp×" << sppScale;
        if (pureImage) std::cout << ", pure-image";
        if (fullFrame) std::cout << ", full-frame";
        std::cout << ") ──\n";
        AdamState adam;
        adam.resize(space.size());
        int stageBestIter = 0;
        int stageWorse = 0;
        double stageBest = lossAt(space.values, sppScale, specularW, pureImage, fullFrame);
        std::vector<double> stageBestTh = space.values;
        const double stageLr = cfg.lr * lrMul;
        const double stageEps = cfg.eps * epsMul;
        for (int it = 0; it < iters; ++it) {
            std::vector<double> g(space.size(), 0.0);
            std::vector<double> theta = space.values;
            for (size_t ai : activeIdx) {
                const double v0 = theta[ai];
                const double span = std::max(1e-3, space.hi[ai] - space.lo[ai]);
                const double rel = pureImage ? 0.025 : 0.02;
                const double epsI = std::max(stageEps, rel * span);
                const double hi = space.project(ai, v0 + epsI);
                const double lo = space.project(ai, v0 - epsI);
                const double denom = hi - lo;
                if (denom < 1e-12) continue;
                theta[ai] = hi;
                const double Lh = lossAt(theta, sppScale, specularW, pureImage, fullFrame);
                theta[ai] = lo;
                const double Ll = lossAt(theta, sppScale, specularW, pureImage, fullFrame);
                theta[ai] = v0;
                g[ai] = (Lh - Ll) / denom;
            }
            const std::vector<double> before = space.values;
            if (cfg.useAdam)
                adam.step(space, g, stageLr);
            else {
                for (size_t ai : activeIdx) {
                    space.values[ai] =
                        space.project(ai, space.values[ai] - stageLr * 50.0 * g[ai]);
                }
            }
            for (size_t i = 0; i < space.size(); ++i) {
                bool active = false;
                for (size_t ai : activeIdx) {
                    if (ai == i) {
                        active = true;
                        break;
                    }
                }
                if (!active) space.values[i] = before[i];
            }
            loss = lossAt(space.values, sppScale, specularW, pureImage, fullFrame);
            std::cout << "  [" << name << "] " << (it + 1) << "/" << iters << "  loss=" << loss
                      << "  θ=" << formatTheta(space.values) << "\n";
            if (!firstTraj) traj << ",\n";
            firstTraj = false;
            traj << "    {\"stage\":\"" << name << "\",\"i\":" << (it + 1) << ",\"loss\":" << loss
                 << ",\"theta\":[";
            for (size_t k = 0; k < space.values.size(); ++k) {
                if (k) traj << ",";
                traj << space.values[k];
            }
            traj << "]}";

            if (loss + 1e-9 < stageBest) {
                stageBest = loss;
                stageBestTh = space.values;
                stageBestIter = it + 1;
                stageWorse = 0;
            } else {
                ++stageWorse;
            }
            if (loss > 0.0 && loss < 5e-5) {
                std::cout << "  early stop (loss floor)\n";
                break;
            }
            const double stopFloor = pureImage ? 5e-4 : 2e-3;
            if (stageWorse >= patience && stageBest < stopFloor) {
                std::cout << "  early stop (best @ " << stageBestIter << ")\n";
                break;
            }
            if (pureImage && stageWorse >= patience + 4) {
                std::cout << "  early stop (no improve @ " << stageBestIter << ")\n";
                break;
            }
        }
        space.values = stageBestTh;
        loss = stageBest;
        if (stageBest < bestLoss) {
            bestLoss = stageBest;
            bestTheta = stageBestTh;
            bestIter = stageBestIter;
        }
    }

    /// Probe multi-start candidates; leave winner in space.values / loss.
    void multiStartProbe(const std::vector<double>& initV) {
        if (cfg.multiStart <= 1) return;
        std::cout << "── multi-start probe (" << cfg.multiStart << " candidates) ──\n";
        std::vector<double> bestStart = space.values;
        double bestStartLoss = loss;
        std::vector<std::vector<double>> candidates;
        candidates.push_back(initV);

        {
            auto mid = initV;
            if (inv.mapGround) {
                const int nT = inv.tileCount();
                for (int k = 0; k < nT * 3; ++k) mid[static_cast<size_t>(k)] = 0.45;
                mid[static_cast<size_t>(nT) * 3] = 0.45;
                mid[static_cast<size_t>(nT) * 3 + 1] = 0.25;
            } else {
                mid[0] = mid[1] = mid[2] = 0.45;
                mid[3] = 0.45;
                mid[4] = 0.25;
            }
            size_t li = inv.lightBlockStart();
            if (inv.fitPedestal && inv.pedestalMat && mid.size() >= li) {
                mid[inv.primaryDims()] = 0.25;
                mid[inv.primaryDims() + 1] = 0.25;
                mid[inv.primaryDims() + 2] = 0.25;
            }
            size_t i = li;
            if (inv.fitKeyLight && inv.keyLight && mid.size() > i) mid[i++] = 0.50;
            if (inv.fitFillLight && inv.fillLight && mid.size() > i) mid[i++] = 0.22;
            if (inv.fitRimLight && inv.rimLight && mid.size() > i) mid[i++] = 0.25;
            if (inv.fitEnvScale && mid.size() > i) mid[i++] = 0.90;
            if (inv.fitExposure && mid.size() > i) mid[i] = 1.0;
            candidates.push_back(mid);
        }
        // Map-ground lab: variegated warm floor (breaks uniform-tile multi-start trap).
        if (inv.mapGround && inv.tileCount() >= 4) {
            auto warm = initV;
            const double tiles[4][3] = {{0.55, 0.42, 0.36},
                                       {0.48, 0.34, 0.26},
                                       {0.40, 0.40, 0.32},
                                       {0.72, 0.55, 0.38}};
            for (int ti = 0; ti < 4 && ti < inv.tileCount(); ++ti) {
                const size_t o = static_cast<size_t>(ti) * 3u;
                warm[o] = tiles[ti][0];
                warm[o + 1] = tiles[ti][1];
                warm[o + 2] = tiles[ti][2];
            }
            const size_t rb = static_cast<size_t>(inv.tileCount()) * 3u;
            warm[rb] = 0.32;
            warm[rb + 1] = 0.12;
            if (inv.fitPedestal && inv.pedestalMat) {
                const size_t pb = inv.primaryDims();
                warm[pb] = 0.20;
                warm[pb + 1] = 0.20;
                warm[pb + 2] = 0.22;
            }
            size_t i = inv.lightBlockStart();
            if (inv.fitKeyLight && inv.keyLight && warm.size() > i) warm[i++] = 0.52;
            if (inv.fitFillLight && inv.fillLight && warm.size() > i) warm[i++] = 0.22;
            if (inv.fitRimLight && inv.rimLight && warm.size() > i) warm[i++] = 0.24;
            if (inv.fitEnvScale && warm.size() > i) warm[i++] = 0.95;
            candidates.push_back(warm);
        }
        {
            auto m = initV;
            if (inv.mapGround) {
                const size_t rb = static_cast<size_t>(inv.tileCount()) * 3u;
                m[rb] = 0.12;
                m[rb + 1] = 0.85;
            } else {
                m[3] = 0.12;
                m[4] = 0.85;
            }
            candidates.push_back(m);
        }
        {
            auto m = initV;
            if (inv.mapGround) {
                const size_t rb = static_cast<size_t>(inv.tileCount()) * 3u;
                m[rb] = 0.80;
                m[rb + 1] = 0.05;
            } else {
                m[3] = 0.80;
                m[4] = 0.05;
            }
            candidates.push_back(m);
        }
        for (int c = static_cast<int>(candidates.size()); c < cfg.multiStart; ++c) {
            candidates.push_back(
                inv.sampleRandomTheta(cfg.seed + 17u * static_cast<uint32_t>(c + 3)));
        }

        const int nProbe = std::min(cfg.multiStart, static_cast<int>(candidates.size()));
        for (int c = 0; c < nProbe; ++c) {
            std::vector<double> th = candidates[static_cast<size_t>(c)];
            if (th.size() != space.size()) continue;
            for (size_t i = 0; i < th.size(); ++i) th[i] = space.project(i, th[i]);
            // Lab: avoid multi-start spp inflation (full-res capture-matched FIT is already heavy).
            const float probeSpp =
                !cfg.labBundle.empty() ? 1.0f
                : (highlightScore > 0.16) ? 2.0f
                                          : 1.25f;
            const double probeSw = (highlightScore > 0.16)
                                       ? std::min(1.0, cfg.specularWeight + 0.2)
                                       : cfg.specularWeight * 0.6;
            double Lc = lossAt(th, probeSpp, probeSw);
            if (highlightScore > 0.16 && th.size() > metalIdx) {
                if (th[metalIdx] > 0.55 && th[roughIdx] < 0.35) Lc *= 0.82;
                if (th[metalIdx] < 0.30) Lc *= 1.18;
            }
            std::cout << "  candidate " << (c + 1) << "/" << nProbe << "  loss=" << Lc << "\n";
            if (Lc < bestStartLoss) {
                bestStartLoss = Lc;
                bestStart = th;
            }
        }
        // Only reinject metal seed for true specular targets (not product floors
        // that merely have bright hero pixels in the crop).
        if (isSpecularTarget() && bestStart.size() > metalIdx) {
            const double tm = targetMetal();
            const double tr = targetRough();
            const double loM = tm - 0.25;
            if (bestStart[metalIdx] < loM) {
                bestStart[metalIdx] = tm;
                bestStart[roughIdx] = tr;
                for (size_t i = 0; i < bestStart.size(); ++i)
                    bestStart[i] = space.project(i, bestStart[i]);
                bestStartLoss =
                    lossAt(bestStart, 2.0f, std::min(1.0, cfg.specularWeight + 0.2));
                std::cout << "  specular reinject metal→" << tm << " rough→" << tr
                          << "  loss=" << bestStartLoss << "\n";
            }
        }
        // Product floors: reject high-metal multi-start winners when image is diffuse-ish.
        if (isDiffuseTarget() && bestStart.size() > metalIdx && bestStart[metalIdx] > 0.45) {
            bestStart[metalIdx] = 0.12;
            bestStart[roughIdx] = std::max(bestStart[roughIdx], 0.35);
            for (size_t i = 0; i < bestStart.size(); ++i)
                bestStart[i] = space.project(i, bestStart[i]);
            bestStartLoss = lossAt(bestStart, 1.5f, cfg.specularWeight * 0.4);
            std::cout << "  diffuse reinject metal↓/rough↑ → loss=" << bestStartLoss << "\n";
        }
        space.values = bestStart;
        loss = bestStartLoss;
        std::cout << "  multi-start winner loss=" << loss << "  θ=" << formatTheta(space.values)
                  << "\n";
    }

    /// Collect active index groups for the B6 schedule.
    struct IndexGroups {
        std::vector<size_t> albedo;
        std::vector<size_t> brdf;
        std::vector<size_t> pedestal;
        std::vector<size_t> light;
        std::vector<size_t> env;
        std::vector<size_t> exposure;
    };

    [[nodiscard]] IndexGroups makeIndexGroups() const {
        IndexGroups g;
        if (inv.mapGround) {
            const size_t nRgb = static_cast<size_t>(inv.tileCount()) * 3u;
            for (size_t i = 0; i < nRgb; ++i) g.albedo.push_back(i);
            g.brdf = {nRgb, nRgb + 1};
        } else {
            g.albedo = {0, 1, 2};
            g.brdf = {3, 4};
        }
        if (inv.fitPedestal && inv.pedestalMat) {
            const size_t base = inv.primaryDims();
            g.pedestal = {base, base + 1, base + 2};
        }
        size_t cursor = inv.lightBlockStart();
        if (inv.fitKeyLight && inv.keyLight) g.light.push_back(cursor++);
        if (inv.fitFillLight && inv.fillLight) g.light.push_back(cursor++);
        if (inv.fitRimLight && inv.rimLight) g.light.push_back(cursor++);
        if (inv.fitEnvScale) g.env.push_back(cursor++);
        if (inv.fitExposure) g.exposure.push_back(cursor);
        return g;
    }

    /// B6 / NN soft / NN hard-BRDF schedule.
    void runSchedule(bool usedNnPrior, double showInitRmse, std::ofstream& traj,
                     bool& firstTraj) {
        const auto g = makeIndexGroups();
        const int envIters = g.env.empty() ? 0 : std::max(6, (cfg.iters * 15) / 100);
        const int lightIters = g.light.empty() ? 0 : std::max(8, (cfg.iters * 22) / 100);
        const int albedoIters = std::max(10, (cfg.iters * 22) / 100);
        const int brdfIters = std::max(10, (cfg.iters * 20) / 100);
        const int pedestalIters = g.pedestal.empty() ? 0 : std::max(5, (cfg.iters * 12) / 100);
        const int refineIters = std::max(5, (cfg.iters * 12) / 100);
        const int exposureIters =
            g.exposure.empty() ? 0 : std::max(4, (cfg.iters * 8) / 100);

        bestTheta = space.values;
        bestLoss = loss;

        // Photo / lab: accept softer NN/multi-start match — full staged FD often walks off.
        const bool externalTarget = !cfg.targetImage.empty();
        const bool labBundle = !cfg.labBundle.empty();
        const double nnShowGoodThresh = (externalTarget || labBundle) ? 0.60 : 0.15;
        const double nnLossGoodThresh = (externalTarget || labBundle) ? 0.55 : 0.12;
        const bool nnImageGood =
            usedNnPrior &&
            (labBundle || showInitRmse < nnShowGoodThresh || loss < nnLossGoodThresh);
        const bool nnSpecular =
            usedNnPrior && (cfg.preset == "mirror" || cfg.preset == "spheres");
        // Product: soft refine when NN is good.
        const bool nnSoftRefine = nnImageGood && !nnSpecular;
        // Specular schedules gate on SHOW RMSE (FIT loss alone can look good while stills fail).
        const bool nnSpecularShowGood = nnSpecular && showInitRmse < 0.10;
        // Specular + good SHOW: hold / light polish (heavy meshes; avoid wrecking NN).
        const bool nnSpecularSoft = nnSpecularShowGood;
        // Specular + weak SHOW: hammer metal/rough with hi-spp specular loss.
        const bool nnSoftColorHardBrdf = nnSpecular && !nnSpecularShowGood;

        if (nnSoftRefine) {
            std::cout << "C1 soft refine (init SHOW RMSE=" << showInitRmse << " loss=" << loss
                      << ") — skipping full staged FD\n";
            runStage("lights_soft", g.light, std::max(3, lightIters / 3), traj, firstTraj, 0.35,
                     0.7);
            // Spatial maps: emphasize albedo recovery (tile RGB). Lab: more iters + pedestal.
            const bool labSoft = !cfg.labBundle.empty();
            const int albI = inv.mapGround
                                 ? std::max(labSoft ? 16 : 12, albedoIters + (labSoft ? 4 : 0))
                                 : std::max(4, albedoIters / 2);
            runStage("albedo_soft", g.albedo, albI, traj, firstTraj,
                     inv.mapGround ? (labSoft ? 0.65 : 0.55) : 0.35, 0.7);
            if (labSoft && !g.pedestal.empty()) {
                runStage("pedestal_soft", g.pedestal, std::max(4, pedestalIters / 2), traj,
                         firstTraj, 0.45, 0.7);
            }
            runStage("brdf_soft", g.brdf, std::max(4, brdfIters / 3), traj, firstTraj, 0.35, 0.55,
                     cfg.brdfSppMul, cfg.specularWeight * 0.5);
            // Second albedo pass after BRDF (decouples color from metal/rough).
            if (labSoft && inv.mapGround) {
                runStage("albedo_soft2", g.albedo, std::max(8, albI / 2), traj, firstTraj, 0.40,
                         0.55);
            }
            std::vector<size_t> refineIdx = g.brdf;
            if (!g.env.empty()) refineIdx.insert(refineIdx.end(), g.env.begin(), g.env.end());
            if (!g.light.empty()) refineIdx.push_back(g.light.front());
            refineIdx.insert(refineIdx.end(), g.albedo.begin(), g.albedo.end());
            if (labSoft && !g.pedestal.empty())
                refineIdx.insert(refineIdx.end(), g.pedestal.begin(), g.pedestal.end());
            runStage("refine", refineIdx, std::max(labSoft ? 8 : 6, refineIters), traj, firstTraj,
                     0.20, 0.45, 1.5f, cfg.specularWeight * 0.4);
        } else if (nnSpecularSoft) {
            // NN already matches well — FD is more likely to ruin SHOW than improve it
            // (esp. huge heroes like MetalRoughSpheres). Only touch BRDF if metal is off.
            if (showInitRmse < 0.06 && space.size() > metalIdx) {
                const double tm = targetMetal();
                const double m = space.values[metalIdx];
                if (std::abs(m - tm) < 0.20) {
                    std::cout << "C1 specular hold (SHOW RMSE=" << showInitRmse
                              << " metal=" << m << " — keeping NN prior)\n";
                    bestTheta = space.values;
                    bestLoss = loss;
                } else {
                    std::cout << "C1 specular hold + metal snap (SHOW RMSE=" << showInitRmse
                              << " metal " << m << " → " << tm << ")\n";
                    space.values[metalIdx] = space.project(metalIdx, tm);
                    space.values[roughIdx] = space.project(roughIdx, targetRough());
                    inv.applyTheta(space.values);
                    loss = lossAt(space.values, 1.5f, 0.9);
                    bestTheta = space.values;
                    bestLoss = loss;
                    runStage("brdf_soft", g.brdf, std::max(3, brdfIters / 3), traj, firstTraj, 0.35,
                             0.5, cfg.brdfSppMul, 0.85);
                }
            } else {
                std::cout << "C1 specular soft polish (SHOW RMSE=" << showInitRmse
                          << " — light BRDF only)\n";
                runStage("lights_soft", g.light, std::max(3, lightIters / 3), traj, firstTraj, 0.35,
                         0.7);
                runStage("brdf_soft", g.brdf, std::max(5, brdfIters / 2), traj, firstTraj, 0.50, 0.5,
                         std::max(cfg.brdfSppMul, 2.0f),
                         std::min(1.0, cfg.specularWeight + 0.2));
                std::vector<size_t> refineIdx = g.brdf;
                if (!g.light.empty()) refineIdx.push_back(g.light.front());
                runStage("refine", refineIdx, std::max(4, refineIters / 2), traj, firstTraj, 0.25,
                         0.45, 1.5f, 0.85);
            }
        } else if (nnSoftColorHardBrdf) {
            std::cout << "C1 soft-color + hard BRDF (SHOW RMSE=" << showInitRmse
                      << " highlight=" << highlightScore << ")\n";
            runStage("lights_soft", g.light, std::max(3, lightIters / 3), traj, firstTraj, 0.40,
                     0.7);
            runStage("brdf_pre", g.brdf, std::max(8, brdfIters), traj, firstTraj, 0.85, 0.55,
                     std::max(cfg.brdfSppMul, 2.5f), std::min(1.0, cfg.specularWeight + 0.35));
            runStage("brdf", g.brdf, std::max(8, brdfIters), traj, firstTraj, 0.70, 0.5,
                     std::max(cfg.brdfSppMul, 2.5f), std::min(1.0, cfg.specularWeight + 0.4));
            runStage("brdf2", g.brdf, std::max(6, brdfIters / 2), traj, firstTraj, 0.45, 0.45,
                     std::max(cfg.brdfSppMul, 2.5f), 1.0);
            runStage("albedo_soft", g.albedo, std::max(3, albedoIters / 3), traj, firstTraj, 0.25,
                     0.7);
            runStage("lights2", g.light, std::max(3, lightIters / 3), traj, firstTraj, 0.40, 0.7);
            std::vector<size_t> refineIdx = g.brdf;
            refineIdx.insert(refineIdx.end(), g.albedo.begin(), g.albedo.end());
            if (!g.env.empty()) refineIdx.insert(refineIdx.end(), g.env.begin(), g.env.end());
            if (!g.light.empty()) refineIdx.push_back(g.light.front());
            runStage("refine", refineIdx, std::max(5, refineIters), traj, firstTraj, 0.25, 0.5,
                     1.5f, 0.85);
        } else {
            runStage("env", g.env, envIters, traj, firstTraj, 1.0, 1.0);
            if (!g.exposure.empty()) {
                runStage("exposure", g.exposure, exposureIters, traj, firstTraj, 1.0, 1.0);
            }
            runStage("lights", g.light, lightIters, traj, firstTraj, 0.85, 1.0);
            if (highlightScore > 0.24) {
                const int brdfPreIters = std::max(6, brdfIters / 2);
                const float preSpp = std::max(cfg.brdfSppMul, 2.0f);
                const double preSw = std::min(1.0, cfg.specularWeight + 0.30);
                runStage("brdf_pre", g.brdf, brdfPreIters, traj, firstTraj, 0.80, 0.65, preSpp,
                         preSw);
            }
            const double albedoLr = usedNnPrior ? 0.45 : 0.8;
            runStage("albedo", g.albedo, albedoIters, traj, firstTraj, albedoLr, 0.9);
            runStage("brdf", g.brdf, brdfIters, traj, firstTraj, 0.75, 0.7, cfg.brdfSppMul,
                     std::min(1.0, cfg.specularWeight + 0.1));
            runStage("brdf2", g.brdf, std::max(4, brdfIters / 2), traj, firstTraj, 0.45, 0.55,
                     cfg.brdfSppMul, std::min(1.0, cfg.specularWeight + 0.15));
            runStage("pedestal", g.pedestal, pedestalIters, traj, firstTraj, 0.65, 1.0);
            runStage("lights2", g.light, std::max(3, lightIters / 2), traj, firstTraj, 0.50, 0.75);
            std::vector<size_t> refineIdx = g.albedo;
            refineIdx.insert(refineIdx.end(), g.brdf.begin(), g.brdf.end());
            if (!g.env.empty()) refineIdx.insert(refineIdx.end(), g.env.begin(), g.env.end());
            if (!g.light.empty()) refineIdx.push_back(g.light.front());
            if (!g.exposure.empty())
                refineIdx.insert(refineIdx.end(), g.exposure.begin(), g.exposure.end());
            runStage("refine", refineIdx, refineIters, traj, firstTraj, 0.35, 0.55, 1.25f,
                     cfg.specularWeight * 0.7);
        }

        // ── Metal lock: snap BRDF mode after coarse stages, short hi-spp polish ──
        metalLockStage(g, traj, firstTraj, brdfIters);
    }

    /// After staged FD, snap metal/rough into the right basin and refine BRDF only.
    void metalLockStage(const IndexGroups& g, std::ofstream& traj, bool& firstTraj,
                        int brdfIters) {
        if (space.size() <= metalIdx) return;
        const double m = space.values[metalIdx];
        const double r = space.values[roughIdx];

        if (isSpecularTarget()) {
            const double tm = targetMetal();
            const double tr = targetRough();
            // Snap if far from basin (mirror low-metal trap, or spheres overshoot to chrome).
            const bool tooLow = m < tm - 0.22;
            const bool tooHigh = isSpheresPreset() && m > tm + 0.28;
            if (tooLow || tooHigh) {
                std::cout << "  metal lock (specular): metal " << m << " → " << tm << ", rough "
                          << r << " → " << tr << "\n";
                space.values[metalIdx] = space.project(metalIdx, tm);
                space.values[roughIdx] = space.project(roughIdx, tr);
                inv.applyTheta(space.values);
                loss = lossAt(space.values, 2.0f, 1.0);
                if (loss < bestLoss) {
                    bestLoss = loss;
                    bestTheta = space.values;
                }
                runStage("metal_lock", g.brdf, std::max(6, brdfIters / 2), traj, firstTraj, 0.55,
                         0.4, std::max(cfg.brdfSppMul, 2.5f), 1.0);
            }
        } else if (isDiffuseTarget() && m > 0.35) {
            std::cout << "  metal lock (diffuse): metal " << m << " → 0.12, rough " << r
                      << " → " << std::max(r, 0.32) << "\n";
            space.values[metalIdx] = space.project(metalIdx, 0.12);
            space.values[roughIdx] = space.project(roughIdx, std::max(r, 0.32));
            inv.applyTheta(space.values);
            loss = lossAt(space.values, 1.5f, cfg.specularWeight * 0.3);
            if (loss < bestLoss) {
                bestLoss = loss;
                bestTheta = space.values;
            }
            runStage("metal_lock", g.brdf, std::max(4, brdfIters / 3), traj, firstTraj, 0.40, 0.5,
                     1.25f, cfg.specularWeight * 0.35);
        }
    }
};

} // namespace ohao::inverse
