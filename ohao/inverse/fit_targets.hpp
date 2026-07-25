#pragma once

// Load inverse targets: lab capture bundle | external photo | synthetic truth renders.

#include "inverse/fit_config.hpp"
#include "inverse/image_loss.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"

#include "render/rt/denoise/denoise_types.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ohao::inverse {

// ── H3: full 6-DOF camera pose parsing from a lab cameras.jsonl ────────────────
// Extract a scalar (string or number) value for `key` from a JSON line.
[[nodiscard]] inline std::string labJsonScalar(const std::string& s, const char* key) {
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
}

// Parse up to `maxN` floats from the JSON array that immediately follows `key`.
// Returns the count parsed (0 if key/array absent). Skips the `position` array
// etc. because it anchors on the exact key requested.
[[nodiscard]] inline int labJsonFloatArray(const std::string& s, const char* key, float* out,
                                           int maxN) {
    const auto k = s.find(key);
    if (k == std::string::npos) return 0;
    const auto lb = s.find('[', k);
    if (lb == std::string::npos) return 0;
    const auto rb = s.find(']', lb);
    if (rb == std::string::npos) return 0;
    int n = 0;
    size_t i = lb + 1;
    while (i < rb && n < maxN) {
        while (i < rb && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) ++i;
        if (i >= rb) break;
        size_t j = i;
        while (j < rb && s[j] != ',') ++j;
        out[n++] = static_cast<float>(std::atof(s.substr(i, j - i).c_str()));
        i = j;
    }
    return n;
}

// Parse cameras.jsonl into per-index CameraViews. Lines carrying a 16-float
// "view" matrix become full 6-DOF poses (hasPose=true); other lines keep the
// legacy position/pitch/yaw fields. Returns true iff ≥1 line had a "view".
// `err` is set (and the return value is meaningless) when a row is malformed:
// callers MUST check it and abort. Silently degrading a malformed row to the
// euler path would point the fit at the wrong camera.
[[nodiscard]] inline bool parseLabCameraPoses(const std::filesystem::path& camerasJsonl,
                                              std::vector<CameraView>& poses, std::string& err) {
    std::ifstream in(camerasJsonl);
    if (!in) return false;
    bool anyPose = false;
    std::vector<std::pair<int, CameraView>> tmp;
    int maxIdx = -1;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.find('{') == std::string::npos) continue; // blank / CR-only line
        CameraView cv{};
        cv.name = "pose"; // string literal: static storage, safe for const char*
        // "index" must be present: std::atoi("") == 0 would collapse every row
        // that omits it onto view 0.
        const std::string idxStr = labJsonScalar(line, "\"index\"");
        if (idxStr.empty()) {
            err = "FATAL: " + camerasJsonl.string() + " line " + std::to_string(lineNo) +
                  " has no \"index\" field (would silently alias onto view 0)";
            return false;
        }
        const int idx = std::atoi(idxStr.c_str());
        // Legacy euler fields (fallback / reference).
        float pos[3] = {0, 0, 0};
        if (labJsonFloatArray(line, "\"position\"", pos, 3) == 3)
            cv.position = glm::vec3(pos[0], pos[1], pos[2]);
        const std::string pitch = labJsonScalar(line, "\"pitch_deg\"");
        const std::string yaw = labJsonScalar(line, "\"yaw_deg\"");
        if (!pitch.empty()) cv.pitchDeg = static_cast<float>(std::atof(pitch.c_str()));
        if (!yaw.empty()) cv.yawDeg = static_cast<float>(std::atof(yaw.c_str()));
        const std::string fov = labJsonScalar(line, "\"fov_deg\"");
        cv.fovDeg = fov.empty() ? 40.0f : static_cast<float>(std::atof(fov.c_str()));
        // Full 6-DOF view matrix (row-major → glm column-major). If the key is
        // present it MUST parse as exactly 16 floats — falling through to the
        // euler path on a truncated/malformed array would fit against a camera
        // that has nothing to do with the photo.
        float m[16];
        const bool hasViewKey = line.find("\"view\"") != std::string::npos;
        const int nView = labJsonFloatArray(line, "\"view\"", m, 16);
        if (hasViewKey && nView != 16) {
            err = "FATAL: " + camerasJsonl.string() + " line " + std::to_string(lineNo) +
                  " (index=" + std::to_string(idx) + ") has a \"view\" field that parsed as " +
                  std::to_string(nView) + " floats, expected 16";
            return false;
        }
        if (nView == 16) {
            glm::mat4 M(1.0f);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) M[c][r] = m[r * 4 + c];
            cv.view = M;
            cv.hasPose = true;
            anyPose = true;
        }
        maxIdx = std::max(maxIdx, idx);
        tmp.emplace_back(idx, cv);
    }
    poses.assign(static_cast<size_t>(std::max(0, maxIdx + 1)), CameraView{});
    for (auto& [i, cv] : tmp)
        if (i >= 0 && i < static_cast<int>(poses.size())) poses[static_cast<size_t>(i)] = cv;
    return anyPose;
}

// Inject full-pose cameras from a bundle into the scene's `views`, overriding by
// index. Views without a parsed pose keep the synthetic scene camera at that
// index (back-compat). Returns the number of poses injected.
// Returns -1 (and fills `err`) on a malformed cameras.jsonl — callers must abort.
inline int injectLabPoses(InverseScene& inv, const std::filesystem::path& camerasJsonl,
                          std::string& err) {
    std::vector<CameraView> poses;
    if (!parseLabCameraPoses(camerasJsonl, poses, err)) {
        if (!err.empty()) return -1;
        return 0; // legacy bundle: keep synthetic
    }
    const size_t n = std::max(poses.size(), inv.views.size());
    std::vector<CameraView> merged;
    merged.reserve(n);
    int injected = 0;
    for (size_t i = 0; i < n; ++i) {
        const bool hasParsed = (i < poses.size() && poses[i].hasPose);
        const bool hasSynth = (i < inv.views.size());
        if (hasParsed) {
            CameraView cv = poses[i];
            if (hasSynth && inv.views[i].name) cv.name = inv.views[i].name; // keep readable name
            merged.push_back(cv);
            ++injected;
        } else if (hasSynth) {
            merged.push_back(inv.views[i]); // back-compat: synthetic pitch/yaw camera
        } else if (i < poses.size()) {
            merged.push_back(poses[i]); // parsed euler-only line beyond synthetic set
        }
    }
    if (!merged.empty()) inv.views = std::move(merged);
    return injected;
}

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
[[nodiscard]] inline int loadLabTargets(FitConfig& cfg, InverseScene& inv, FitTargetBundle& tb,
                                        const std::filesystem::path& outDir) {
    std::string err;
    if (!resolveLabCapturePath(cfg, tb.labCap, err)) {
        std::cerr << err << "\n";
        return 1;
    }
    tb.labMode = true;
    cfg.showDenoise = DenoiseMode::None;

    // H3: adopt the bundle's real 6-DOF camera poses (COLMAP path). Lines without
    // a "view" matrix fall back to the synthetic scene cameras (back-compat).
    std::string poseErr;
    const int injected = injectLabPoses(inv, tb.labCap / "cameras.jsonl", poseErr);
    if (injected < 0) {
        std::cerr << poseErr << "\n";
        return 1;
    }
    if (injected > 0)
        std::cout << "Lab bundle: injected " << injected
                  << " full 6-DOF camera pose(s) from cameras.jsonl\n";

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
        if (line.find('{') == std::string::npos) continue; // blank / CR-only line
        CamLine c;
        const std::string idxStr = jsonStr(line, "\"index\"");
        if (idxStr.empty()) {
            std::cerr << "FATAL: " << (tb.labCap / "cameras.jsonl")
                      << " has a row with no \"index\" field (would collapse onto view 0): "
                      << line << "\n";
            return 1;
        }
        c.index = std::atoi(idxStr.c_str());
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
    // Partial COLMAP registration is a NORMAL outcome (photo_ingest.py emits rows
    // without an "R_view" when an image fails to register). If ANY row carried a
    // real pose, then EVERY row must: otherwise the unregistered photos would be
    // fitted against a leftover synthetic studio camera — or a default-constructed
    // CameraView at the world origin — and reported as a confident PSNR.
    if (injected > 0 && injected < static_cast<int>(cams.size())) {
        std::cerr << "FATAL: lab bundle carries real 6-DOF poses but only " << injected
                  << " of " << cams.size() << " camera row(s) have one ("
                  << train.size() << " train + " << hold.size()
                  << " holdout). Partial registration would fit the unregistered photos "
                     "against arbitrary cameras. Re-run COLMAP or drop the unregistered "
                     "rows from cameras.jsonl.\n";
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

    if (!cfg.labBundle.empty()) return loadLabTargets(cfg, inv, tb, outDir);
    if (tb.externalTarget) return loadExternalTarget(cfg, tb, outDir);
    return loadSyntheticTargets(cfg, inv, session, tb, truthV, outDir);
}

} // namespace ohao::inverse
