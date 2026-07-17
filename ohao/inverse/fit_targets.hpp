#pragma once

// Load inverse targets: lab capture bundle | external photo | synthetic truth renders.

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/rt/denoise/denoise_types.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ohao::inverse {

struct FitTargetBundle {
    int nViews{1};
    bool labMode{false};
    bool externalTarget{false};
    std::filesystem::path labCap;
    std::vector<ImageRGBA8> targetsFit;
    std::vector<ImageRGBA8> targetsShow;
    std::vector<ImageRGBA8> holdoutShow;
    std::vector<int> holdoutViewIdx;
    std::vector<ImageRGBA8> relightShowGt;
};

/// Resolve lab bundle path; sets cfg.showDenoise = None when lab.
[[nodiscard]] inline bool resolveLabCapturePath(FitConfig& cfg, std::filesystem::path& labCap,
                                                std::string& err) {
    labCap = cfg.labBundle;
    if (std::filesystem::exists(labCap / "capture.json")) {
        return true;
    }
    if (std::filesystem::exists(labCap / "capture" / "capture.json")) {
        labCap = labCap / "capture";
        return true;
    }
    err = "FATAL: --lab-bundle missing capture.json under " + cfg.labBundle;
    return false;
}

/// Load train/holdout/relight from ohao_inverse_lab_capture; half-res FIT budget.
[[nodiscard]] inline int loadLabTargets(FitConfig& cfg, FitTargetBundle& tb,
                                        const std::filesystem::path& outDir) {
    std::string err;
    if (!resolveLabCapturePath(cfg, tb.labCap, err)) {
        std::cerr << err << "\n";
        return 1;
    }
    tb.labMode = true;
    cfg.showDenoise = DenoiseMode::None;

    std::ifstream camIn(tb.labCap / "cameras.jsonl");
    if (!camIn) {
        std::cerr << "FATAL: cannot read " << (tb.labCap / "cameras.jsonl") << "\n";
        return 1;
    }
    struct CamLine {
        int index{0};
        std::string file;
        std::string split;
    };
    std::vector<CamLine> cams;
    std::string line;
    auto jsonStr = [](const std::string& s, const char* key) -> std::string {
        const auto k = s.find(key);
        if (k == std::string::npos) return {};
        const auto colon = s.find(':', k);
        if (colon == std::string::npos) return {};
        size_t i = colon + 1;
        while (i < s.size() && s[i] == ' ') ++i;
        if (i < s.size() && s[i] == '\"') {
            const auto q1 = s.find('\"', i + 1);
            if (q1 == std::string::npos) return {};
            return s.substr(i + 1, q1 - i - 1);
        }
        size_t j = i;
        while (j < s.size() && s[j] != ',' && s[j] != '}') ++j;
        return s.substr(i, j - i);
    };
    while (std::getline(camIn, line)) {
        if (line.empty()) continue;
        CamLine c;
        c.index = std::atoi(jsonStr(line, "\"index\"").c_str());
        c.file = jsonStr(line, "\"file\"");
        c.split = jsonStr(line, "\"split\"");
        cams.push_back(std::move(c));
    }
    std::vector<CamLine> train, hold;
    for (const auto& c : cams) {
        if (c.split == "holdout")
            hold.push_back(c);
        else
            train.push_back(c);
    }
    if (train.empty()) {
        std::cerr << "FATAL: lab bundle has no train views\n";
        return 1;
    }
    tb.nViews = static_cast<int>(train.size());
    tb.targetsShow.resize(static_cast<size_t>(tb.nViews));
    tb.targetsFit.resize(static_cast<size_t>(tb.nViews));
    for (int i = 0; i < tb.nViews; ++i) {
        const auto path = tb.labCap / "images" / train[static_cast<size_t>(i)].file;
        ImageRGBA8 loaded = loadPNG(path);
        if (loaded.empty()) {
            std::cerr << "FATAL: failed to load lab image " << path << "\n";
            return 1;
        }
        tb.targetsShow[static_cast<size_t>(i)] =
            resizeNearest(loaded, cfg.show.width, cfg.show.height);
        tb.targetsFit[static_cast<size_t>(i)] =
            resizeNearest(loaded, cfg.fit.width, cfg.fit.height);
    }
    tb.holdoutShow.resize(hold.size());
    tb.holdoutViewIdx.resize(hold.size());
    for (size_t i = 0; i < hold.size(); ++i) {
        const auto path = tb.labCap / "images" / hold[i].file;
        tb.holdoutShow[i] = loadPNG(path);
        tb.holdoutViewIdx[i] = hold[i].index;
        if (tb.holdoutShow[i].empty()) {
            std::cerr << "FATAL: failed to load holdout " << path << "\n";
            return 1;
        }
        tb.holdoutShow[i] = resizeNearest(tb.holdoutShow[i], cfg.show.width, cfg.show.height);
    }
    const auto rel0 = tb.labCap / "relight" / "train_000.png";
    if (std::filesystem::exists(rel0)) {
        tb.relightShowGt.push_back(
            resizeNearest(loadPNG(rel0), cfg.show.width, cfg.show.height));
    }
    savePNG(tb.targetsShow[0], outDir / "target_show.png");
    savePNG(tb.targetsFit[0], outDir / "target_fit.png");

    cfg.fit.width = std::max(320u, cfg.show.width / 2);
    cfg.fit.height = std::max(180u, cfg.show.height / 2);
    cfg.fit.spp = std::max(64, std::min(cfg.show.spp, std::max(cfg.fit.spp, 64)));
    for (int i = 0; i < tb.nViews; ++i) {
        tb.targetsFit[static_cast<size_t>(i)] = resizeNearest(
            tb.targetsShow[static_cast<size_t>(i)], cfg.fit.width, cfg.fit.height);
    }
    for (auto& h : tb.holdoutShow) {
        if (h.width != cfg.show.width || h.height != cfg.show.height)
            h = resizeNearest(h, cfg.show.width, cfg.show.height);
    }
    for (auto& r : tb.relightShowGt) {
        if (!r.empty() && (r.width != cfg.show.width || r.height != cfg.show.height))
            r = resizeNearest(r, cfg.show.width, cfg.show.height);
    }
    std::ofstream used(outDir / "capture_used.json");
    used << "{\"lab_bundle\": \"" << tb.labCap.string() << "\", \"n_train\": " << tb.nViews
         << ", \"n_holdout\": " << tb.holdoutShow.size() << ", \"fit_wh\": [" << cfg.fit.width
         << "," << cfg.fit.height << "],"
         << "\"fit_spp\": " << cfg.fit.spp << ",\"show_spp\": " << cfg.show.spp << "}\n";
    std::cout << "Lab bundle " << tb.labCap << "  train=" << tb.nViews
              << " holdout=" << tb.holdoutShow.size() << "  fit=" << cfg.fit.width << "x"
              << cfg.fit.height << "@" << cfg.fit.spp << " spp  show_eval=@" << cfg.show.spp
              << " spp (capture W×H for eval)\n";
    return 0;
}

[[nodiscard]] inline int loadExternalTarget(FitConfig& cfg, FitTargetBundle& tb,
                                            const std::filesystem::path& outDir) {
    if (!std::filesystem::exists(cfg.targetImage)) {
        std::cerr << "FATAL: --target-image not found: " << cfg.targetImage << "\n";
        return 1;
    }
    ImageRGBA8 loaded = loadPNG(cfg.targetImage);
    if (loaded.empty()) {
        std::cerr << "FATAL: failed to load --target-image: " << cfg.targetImage << "\n";
        return 1;
    }
    if (!cfg.fitExposure) loaded = applyExposure(loaded, cfg.exposure);
    std::cout << "External target " << cfg.targetImage << " (" << loaded.width << "x"
              << loaded.height << ") exposure="
              << (cfg.fitExposure ? "fit" : std::to_string(cfg.exposure)) << "\n";
    tb.externalTarget = true;
    tb.nViews = 1;
    tb.targetsShow.resize(1);
    tb.targetsFit.resize(1);
    tb.targetsShow[0] = resizeNearest(loaded, cfg.show.width, cfg.show.height);
    tb.targetsFit[0] = resizeNearest(loaded, cfg.fit.width, cfg.fit.height);
    savePNG(tb.targetsShow[0], outDir / "target_show.png");
    savePNG(tb.targetsFit[0], outDir / "target_fit.png");
    savePNG(tb.targetsShow[0], outDir / "target_front.png");
    return 0;
}

[[nodiscard]] inline int loadSyntheticTargets(const FitConfig& cfg, InverseScene& inv,
                                              RenderSession& session, FitTargetBundle& tb,
                                              const std::vector<double>& truthV,
                                              const std::filesystem::path& outDir) {
    std::cout << "Rendering multi-view TARGETS (truth θ=" << formatTheta(truthV) << ")...\n";
    tb.targetsShow.resize(static_cast<size_t>(tb.nViews));
    tb.targetsFit.resize(static_cast<size_t>(tb.nViews));
    for (int v = 0; v < tb.nViews; ++v) {
        std::cout << "  SHOW " << inv.views[static_cast<size_t>(v)].name << "...\n";
        tb.targetsShow[static_cast<size_t>(v)] =
            session.render(v, cfg.show, cfg.seed, cfg.showDenoise);
        savePNG(tb.targetsShow[static_cast<size_t>(v)],
                outDir / (std::string("target_") + inv.views[static_cast<size_t>(v)].name +
                          ".png"));
        tb.targetsFit[static_cast<size_t>(v)] =
            session.render(v, cfg.fit, cfg.seed, DenoiseMode::None);
    }
    savePNG(tb.targetsShow[0], outDir / "target_show.png");
    return 0;
}

/// Populate targets from lab | external | synthetic. Updates tb.nViews.
[[nodiscard]] inline int loadFitTargets(FitConfig& cfg, InverseScene& inv, RenderSession& session,
                                        FitTargetBundle& tb, int maxViews,
                                        const std::vector<double>& truthV,
                                        const std::filesystem::path& outDir) {
    tb.nViews = std::min(maxViews, static_cast<int>(inv.views.size()));
    tb.externalTarget = !cfg.targetImage.empty() && cfg.labBundle.empty();

    if (!cfg.labBundle.empty()) return loadLabTargets(cfg, tb, outDir);
    if (tb.externalTarget) return loadExternalTarget(cfg, tb, outDir);
    return loadSyntheticTargets(cfg, inv, session, tb, truthV, outDir);
}

} // namespace ohao::inverse
