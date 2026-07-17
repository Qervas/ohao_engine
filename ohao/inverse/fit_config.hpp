#pragma once

// Inverse-fit CLI config, quality wiring, and scene presets.

#include "inverse/backend/image_formation.hpp"
#include "inverse/quality.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace ohao::inverse {

struct CameraView {
    const char* name;
    glm::vec3 position;
    float pitchDeg;
    float yawDeg;
};

struct FitConfig {
    QualityPreset quality{kQualityHigh};
    RenderBudget fit{kQualityHigh.fit};
    RenderBudget show{kQualityHigh.show};
    int iters{40}; // staged Adam steps budget
    double lr{0.06};
    double eps{0.04};
    double maskX{1.0};     // studio: full width
    double maskYMin{0.22}; // floor + pedestal band
    uint32_t seed{1};
    std::string outDir{"renders/inverse"};
    bool useAdam{true};
    DenoiseMode showDenoise{DenoiseMode::OIDN};
    std::string scene{"studio"}; // studio | cornell
    int numViews{3};
    bool fitPedestal{true};
    bool fitKeyLight{true};
    bool fitFillLight{true};
    bool fitRimLight{true};
    bool fitEnvScale{true};
    int exportDataset{0}; // >0: write N (θ, FIT image) pairs then exit
    std::string preset{"lantern"}; // object/scene pack
    // Prefer showcase Lantern when present; fall back to tracked test assets.
    std::string modelPath{"assets/showcase_objects/Lantern.glb"};
    std::string envPath{"assets/hdri/brown_photostudio_02_2k.hdr"};
    std::string relightEnvPath{"assets/hdri/kloofendal_43d_clear_puresky_2k.hdr"};
    float heroScaleMul{1.0f};
    float camDistMul{1.0f};
    bool modelFromCli{false};
    bool envFromCli{false};
    // Ground truth/init (set by applyPreset / defaults in buildStudio)
    float truthR{0.72f}, truthG{0.55f}, truthB{0.42f}, truthRough{0.30f}, truthMetal{0.12f};
    float initR{0.20f}, initG{0.45f}, initB{0.70f}, initRough{0.88f}, initMetal{0.70f};
    const char* presetNote{"product studio / Lantern"};

    // ── B6 gap-close ──────────────────────────────────────────────────
    int multiStart{5};          // candidate inits (1 = off / single start)
    double lightReg{0.035};     // multi-light ambiguity regularizer
    double specularWeight{0.45};// highlight-biased image loss (metal/rough)
    float brdfSppMul{2.0f};     // BRDF stage FIT spp multiplier
    bool mapGround{false};      // ground albedo tiles (shared rough/metal); see mapRes
    int mapRes{2};              // tile grid N×N when mapGround (2=classic 2×2, 4=lab maps)
    std::string targetImage;    // external LDR target (PNG/JPG); empty = synthetic
    float exposure{1.0f};       // applied to external target (or fitted)
    bool fitExposure{false};    // add exposure as free θ dim (photo path)
    // C1: neural θ prior (JSON from tools/inverse_c1/infer.py)
    std::string thetaInitPath;  // if set, override init θ with predicted prior
    std::string nnModelPath;    // if set, run infer.py on FIT target → theta init
    std::string nnPython{"python3"};
    bool nnSkipMultiStart{true};// when θ-init present, skip multi-start (prior is the start)
    // C1 export generalization (L1–L4 ladder)
    bool domainRand{false};           // L2: randomize cam/lights/hero per sample
    bool exportExposureJitter{false}; // L4-lite: random exposure gain on export PNGs
    int exportViewMode{0};            // 0=primary, 1=random named view, 2=all views
    // Visual match polish (reduces SHOW gap after coarse / NN stages)
    bool visualPolish{true};          // final high-spp pure-image FD
    float polishSppMul{4.0f};         // FIT spp multiplier for polish
    bool polishFullFrame{true};       // ignore crop mask during polish
    int polishIters{0};               // 0 = auto from --iters
    // Lab track (multi-view capture / fit)
    bool exportCapture{false};        // write ohao_inverse_lab_capture bundle then exit
    std::string labBundle;            // path to capture/ dir (or parent containing capture/)
    // Image formation backend (pt = path tracer FD; diff = Diff-IR when wired)
    InverseBackend backend{InverseBackend::PathTrace};
};

// Resolve missing optional showcase assets to tracked test_models copies.
inline void resolveAssetFallbacks(FitConfig& cfg) {
    auto pick = [](std::string& path, std::initializer_list<const char*> alts) {
        if (std::filesystem::exists(path)) return;
        for (const char* a : alts) {
            if (std::filesystem::exists(a)) {
                path = a;
                return;
            }
        }
    };
    pick(cfg.modelPath, {"assets/showcase_objects/Lantern.glb",
                         "assets/test_models/DamagedHelmet.glb"});
    pick(cfg.envPath, {"assets/hdri/brown_photostudio_02_2k.hdr",
                       "assets/test_models/env_studio.hdr"});
    pick(cfg.relightEnvPath, {"assets/hdri/kloofendal_43d_clear_puresky_2k.hdr",
                              "assets/test_models/env_outdoor.hdr",
                              "assets/test_models/env_studio.hdr"});
}

/// Object / lighting packs — including tricky heroes (metal chart, glass bottle, car).
inline void applyPreset(FitConfig& cfg) {
    const std::string& p = cfg.preset;
    auto setModel = [&](const char* path) {
        if (!cfg.modelFromCli) cfg.modelPath = path;
    };
    auto setEnv = [&](const char* path) {
        if (!cfg.envFromCli) cfg.envPath = path;
    };
    auto setTruth = [&](float r, float g, float b, float rough, float metal) {
        cfg.truthR = r; cfg.truthG = g; cfg.truthB = b;
        cfg.truthRough = rough; cfg.truthMetal = metal;
    };
    auto setInit = [&](float r, float g, float b, float rough, float metal) {
        cfg.initR = r; cfg.initG = g; cfg.initB = b;
        cfg.initRough = rough; cfg.initMetal = metal;
    };

    if (p == "cornell") {
        cfg.scene = "cornell";
        cfg.presetNote = "classic cornell box";
        return;
    }
    cfg.scene = "studio";

    if (p == "lantern" || p == "default" || p.empty()) {
        setModel("assets/showcase_objects/Lantern.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        setTruth(0.72f, 0.55f, 0.42f, 0.30f, 0.12f);
        setInit(0.20f, 0.45f, 0.70f, 0.88f, 0.70f);
        cfg.presetNote = "Lantern product studio (baseline)";
    } else if (p == "helmet") {
        setModel("assets/test_models/DamagedHelmet.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        setTruth(0.55f, 0.48f, 0.40f, 0.45f, 0.25f);
        setInit(0.15f, 0.55f, 0.75f, 0.90f, 0.05f);
        cfg.heroScaleMul = 1.1f;
        cfg.presetNote = "DamagedHelmet (textured metal-ish hero)";
    } else if (p == "bottle") {
        setModel("assets/complex_scenes/WaterBottle.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        setTruth(0.65f, 0.62f, 0.58f, 0.20f, 0.05f);
        setInit(0.25f, 0.35f, 0.80f, 0.85f, 0.55f);
        cfg.heroScaleMul = 0.95f;
        cfg.camDistMul = 0.95f;
        cfg.presetNote = "WaterBottle (glass/plastic refraction-ish; TRICKY)";
    } else if (p == "spheres") {
        setModel("assets/showcase_objects/MetalRoughSpheres.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        // Darker glossy floor — metal chart reflects it hard
        setTruth(0.35f, 0.35f, 0.38f, 0.18f, 0.55f);
        setInit(0.70f, 0.25f, 0.15f, 0.75f, 0.10f);
        cfg.heroScaleMul = 1.15f;
        cfg.camDistMul = 1.15f;
        cfg.specularWeight = 0.70;
        cfg.brdfSppMul = 2.5f;
        cfg.presetNote = "MetalRoughSpheres (metal/rough chart; TRICKY)";
    } else if (p == "toycar") {
        setModel("assets/showcase_objects/ToyCar.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        setTruth(0.68f, 0.52f, 0.38f, 0.35f, 0.08f);
        setInit(0.15f, 0.50f, 0.75f, 0.90f, 0.65f);
        cfg.heroScaleMul = 1.0f;
        cfg.camDistMul = 1.05f;
        cfg.presetNote = "ToyCar (complex mesh + fabrics; TRICKY)";
    } else if (p == "boombox") {
        setModel("assets/showcase_objects/BoomBox.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        setTruth(0.50f, 0.50f, 0.52f, 0.40f, 0.15f);
        setInit(0.80f, 0.30f, 0.20f, 0.20f, 0.80f);
        cfg.heroScaleMul = 1.05f;
        cfg.presetNote = "BoomBox (dense mesh + mixed materials)";
    } else if (p == "outdoor") {
        setModel("assets/showcase_objects/Lantern.glb");
        setEnv("assets/hdri/kloofendal_43d_clear_puresky_2k.hdr");
        setTruth(0.45f, 0.42f, 0.38f, 0.55f, 0.05f);
        setInit(0.20f, 0.55f, 0.85f, 0.15f, 0.70f);
        cfg.truthR = 0.45f; // outdoor concrete
        cfg.presetNote = "Lantern under outdoor HDRI (strong dir light; TRICKY)";
    } else if (p == "mirror") {
        setModel("assets/showcase_objects/Lantern.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        // Near-mirror floor — very sensitive to lights/env
        setTruth(0.85f, 0.85f, 0.88f, 0.08f, 0.92f);
        setInit(0.30f, 0.40f, 0.55f, 0.70f, 0.15f);
        cfg.specularWeight = 0.75;
        cfg.brdfSppMul = 2.5f;
        cfg.presetNote = "Mirror floor (high metal/low rough; TRICKY)";
    } else if (p == "chess") {
        setModel("assets/showcase_objects/ABeautifulGame.glb");
        setEnv("assets/hdri/brown_photostudio_02_2k.hdr");
        setTruth(0.60f, 0.50f, 0.40f, 0.50f, 0.02f);
        setInit(0.20f, 0.25f, 0.70f, 0.25f, 0.60f);
        cfg.heroScaleMul = 1.2f;
        cfg.camDistMul = 1.2f;
        cfg.presetNote = "ABeautifulGame chess set (large scene; TRICKY)";
    } else {
        std::cerr << "unknown --preset '" << p << "', using lantern\n";
        cfg.preset = "lantern";
        applyPreset(cfg);
        return;
    }
}

struct CliArgs {
    FitConfig cfg;
};

inline CliArgs parseArgs(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view s{argv[i]};
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "--selftest") {
            continue;
        } else if (s == "--quality") {
            a.cfg.quality = qualityFromName(need("--quality"));
            a.cfg.fit = a.cfg.quality.fit;
            a.cfg.show = a.cfg.quality.show;
        } else if (s == "--scene") {
            a.cfg.scene = need("--scene");
            if (a.cfg.scene == "cornell") {
                a.cfg.maskX = 0.40;
                a.cfg.maskYMin = 0.0;
            }
        } else if (s == "--preset") {
            a.cfg.preset = need("--preset");
        } else if (s == "--model") {
            a.cfg.modelPath = need("--model");
            a.cfg.modelFromCli = true;
        } else if (s == "--env") {
            a.cfg.envPath = need("--env");
            a.cfg.envFromCli = true;
        } else if (s == "--relight-env") {
            a.cfg.relightEnvPath = need("--relight-env");
        } else if (s == "--views") {
            a.cfg.numViews = std::clamp(std::atoi(need("--views")), 1, 8);
        } else if (s == "--no-pedestal") {
            a.cfg.fitPedestal = false;
        } else if (s == "--no-light") {
            a.cfg.fitKeyLight = false;
            a.cfg.fitFillLight = false;
            a.cfg.fitRimLight = false;
        } else if (s == "--no-fill") {
            a.cfg.fitFillLight = false;
        } else if (s == "--no-rim") {
            a.cfg.fitRimLight = false;
        } else if (s == "--no-env") {
            a.cfg.fitEnvScale = false;
        } else if (s == "--export-dataset") {
            a.cfg.exportDataset = std::max(1, std::atoi(need("--export-dataset")));
        } else if (s == "--multi-start") {
            a.cfg.multiStart = std::max(1, std::atoi(need("--multi-start")));
        } else if (s == "--no-multi-start") {
            a.cfg.multiStart = 1;
        } else if (s == "--light-reg") {
            a.cfg.lightReg = std::max(0.0, std::atof(need("--light-reg")));
        } else if (s == "--specular-weight") {
            a.cfg.specularWeight = std::clamp(std::atof(need("--specular-weight")), 0.0, 1.0);
        } else if (s == "--brdf-spp-mul") {
            a.cfg.brdfSppMul = std::max(1.0f, static_cast<float>(std::atof(need("--brdf-spp-mul"))));
        } else if (s == "--map-ground") {
            a.cfg.mapGround = true;
            if (a.cfg.mapRes < 2) a.cfg.mapRes = 2;
        } else if (s == "--map-res") {
            a.cfg.mapRes = std::clamp(std::atoi(need("--map-res")), 1, 8);
            a.cfg.mapGround = a.cfg.mapRes >= 2;
        } else if (s == "--target-image") {
            a.cfg.targetImage = need("--target-image");
        } else if (s == "--exposure") {
            a.cfg.exposure = static_cast<float>(std::atof(need("--exposure")));
        } else if (s == "--fit-exposure") {
            a.cfg.fitExposure = true;
        } else if (s == "--theta-init") {
            a.cfg.thetaInitPath = need("--theta-init");
        } else if (s == "--nn-model") {
            a.cfg.nnModelPath = need("--nn-model");
        } else if (s == "--nn-python") {
            a.cfg.nnPython = need("--nn-python");
        } else if (s == "--nn-keep-multi-start") {
            a.cfg.nnSkipMultiStart = false;
        } else if (s == "--domain-rand") {
            a.cfg.domainRand = true;
        } else if (s == "--export-exposure-jitter") {
            a.cfg.exportExposureJitter = true;
        } else if (s == "--export-views") {
            const std::string v = need("--export-views");
            if (v == "0" || v == "primary") {
                a.cfg.exportViewMode = 0;
            } else if (v == "random") {
                a.cfg.exportViewMode = 1;
            } else if (v == "all") {
                a.cfg.exportViewMode = 2;
            } else {
                std::cerr << "--export-views expects primary|random|all\n";
                std::exit(2);
            }
        } else if (s == "--visual-polish") {
            a.cfg.visualPolish = true;
        } else if (s == "--no-visual-polish") {
            a.cfg.visualPolish = false;
        } else if (s == "--polish-spp-mul") {
            a.cfg.polishSppMul =
                std::max(1.0f, static_cast<float>(std::atof(need("--polish-spp-mul"))));
        } else if (s == "--polish-iters") {
            a.cfg.polishIters = std::max(1, std::atoi(need("--polish-iters")));
        } else if (s == "--polish-crop") {
            a.cfg.polishFullFrame = false; // use normal mask crop
        } else if (s == "--fit-width") {
            a.cfg.fit.width = static_cast<uint32_t>(std::max(16, std::atoi(need("--fit-width"))));
        } else if (s == "--fit-height") {
            a.cfg.fit.height = static_cast<uint32_t>(std::max(16, std::atoi(need("--fit-height"))));
        } else if (s == "--fit-spp") {
            a.cfg.fit.spp = std::max(1, std::atoi(need("--fit-spp")));
        } else if (s == "--show-width") {
            a.cfg.show.width = static_cast<uint32_t>(std::max(16, std::atoi(need("--show-width"))));
        } else if (s == "--show-height") {
            a.cfg.show.height = static_cast<uint32_t>(std::max(16, std::atoi(need("--show-height"))));
        } else if (s == "--show-spp") {
            a.cfg.show.spp = std::max(1, std::atoi(need("--show-spp")));
        } else if (s == "--iters") {
            a.cfg.iters = std::max(1, std::atoi(need("--iters")));
        } else if (s == "--lr") {
            a.cfg.lr = std::atof(need("--lr"));
        } else if (s == "--eps") {
            a.cfg.eps = std::atof(need("--eps"));
        } else if (s == "--mask-x") {
            a.cfg.maskX = std::atof(need("--mask-x"));
        } else if (s == "--mask-y-min") {
            a.cfg.maskYMin = std::atof(need("--mask-y-min"));
        } else if (s == "--seed") {
            a.cfg.seed = static_cast<uint32_t>(std::atoi(need("--seed")));
        } else if (s == "--gd") {
            a.cfg.useAdam = false;
        } else if (s.starts_with("--show-denoise=")) {
            a.cfg.showDenoise = parseDenoiseMode(s.substr(std::string_view("--show-denoise=").size()));
            if (a.cfg.showDenoise != DenoiseMode::None && a.cfg.showDenoise != DenoiseMode::OIDN) {
                std::cerr << "SHOW denoise: use none or oidn\n";
                std::exit(2);
            }
        } else if (s == "--out-dir") {
            a.cfg.outDir = need("--out-dir");
        } else if (s == "--export-capture") {
            a.cfg.exportCapture = true;
            // Lab default: spatial ground albedo maps (2×2 tiles; override with --map-res).
            if (!a.cfg.mapGround) {
                a.cfg.mapGround = true;
                a.cfg.mapRes = 2;
            }
        } else if (s == "--lab-bundle") {
            a.cfg.labBundle = need("--lab-bundle");
        } else if (s == "--backend") {
            a.cfg.backend = parseInverseBackend(need("--backend"));
            if (!a.cfg.mapGround) {
                a.cfg.mapGround = true;
                a.cfg.mapRes = 2;
            }
        } else if (s == "--help" || s == "-h") {
            std::cout
                << "Usage: inverse_fit [--selftest] [--scene studio|cornell]\n"
                << "  Studio θ: ground PBR[5] + pedestal[3] + key/fill/rim I + env scale\n"
                << "  B6 stages: multi-start → env → lights → albedo → brdf → pedestal → lights2 → refine\n"
                << "  --preset lantern|helmet|bottle|spheres|toycar|boombox|outdoor|mirror|chess|cornell\n"
                << "  --export-dataset N   ML data factory\n"
                << "  --export-capture     lab multi-view capture bundle (train/holdout/relight)\n"
                << "  --lab-bundle PATH    fit from lab capture/ directory\n"
                << "  --backend pt|diff|hybrid  pt=path tracer; diff=Deferred map SoT;\n"
                << "                            hybrid=Diff fit + PT capture-gated eval\n"
                << "  --multi-start N | --no-multi-start   (default 5 candidates)\n"
                << "  --light-reg W  --specular-weight W  --brdf-spp-mul M\n"
                << "  --map-ground           N×N ground albedo tiles (default N=2)\n"
                << "  --map-res N            tile grid size 1..8 (implies map-ground if N>=2)\n"
                << "  --export-capture       lab multi-view capture (default map-res 4)\n"
                << "  --lab-bundle PATH      fit from lab capture/ (default map-res 4)\n"
                << "  --target-image PATH    external LDR photo target\n"
                << "  --exposure E  --fit-exposure\n"
                << "  --theta-init PATH.json C1 NN prior (from tools/inverse_c1/infer.py)\n"
                << "  --nn-model PATH.pt     auto-run C1 infer on FIT target → θ prior\n"
                << "  --nn-python BIN        python for --nn-model (default python3)\n"
                << "  --nn-keep-multi-start   still probe multi-start after NN prior\n"
                << "  --export-dataset N     ML data factory (θ, FIT image) pairs\n"
                << "  --domain-rand          L2: randomize cam/lights/hero per export sample\n"
                << "  --export-exposure-jitter  L4-lite exposure gain on export PNGs\n"
                << "  --export-views primary|random|all\n"
                << "  --visual-polish / --no-visual-polish  high-spp pure-image final FD (default on)\n"
                << "  --polish-spp-mul M  --polish-iters N  --polish-crop\n"
                << "  --no-pedestal --no-light --no-fill --no-rim --no-env\n"
                << "  --quality draft|high|ultra|cinema  --views N\n";
            std::exit(0);
        } else {
            std::cerr << "unknown arg: " << s << "\n";
            std::exit(2);
        }
    }
    return a;
}


} // namespace ohao::inverse
