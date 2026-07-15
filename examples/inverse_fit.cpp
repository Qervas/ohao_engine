// inverse_fit — physical inverse renderer (no ML).
//
// Gradual roadmap (no ML yet):
//   A) dual-budget + grain-free SHOW (OIDN)
//   B) product-studio scene + multi-view + relight
//   B1–B5) scalar full PBR + multi-light + env scale + staged FD
//   B6) gap-close: multi-start, specular loss, light regularizer,
//       external --target-image, optional 2×2 ground albedo map
//   C) ML priors later (not this binary)
//
// Studio θ (default 12D):
//   ground[5]  = albedo.rgb, roughness, metallic
//   pedestal[3] = albedo.rgb
//   key_I, fill_I, rim_I, env_scale
// With --map-ground: ground becomes 4×RGB tiles + shared rough/metal (14D primary).
// Schedule: multi-start → env → lights → albedo → brdf(hi-spp) → pedestal → lights2 → refine
// --export-dataset N  writes (θ, image) pairs for future ML
// FIT  = raw MC (noise is a feature for FD) — denoise NEVER on
// SHOW = grain-free stills (OIDN default)
//
// Usage:
//   ./inverse_fit --selftest --preset lantern --quality draft
//   ./inverse_fit --selftest --preset mirror --quality draft
//   ./inverse_fit --target-image photo.png --fit-exposure --quality high

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "stb_image.h"
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "inverse/inverse_module.hpp"
#include "render/camera/camera.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "scene/actor/actor.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/component/light_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace ohao;
using namespace ohao::inverse;

namespace {

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
    bool mapGround{false};      // 2×2 ground albedo tiles (shared rough/metal)
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
};

// Resolve missing optional showcase assets to tracked test_models copies.
void resolveAssetFallbacks(FitConfig& cfg) {
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
void applyPreset(FitConfig& cfg) {
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

CliArgs parseArgs(int argc, char** argv) {
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
        } else if (s == "--help" || s == "-h") {
            std::cout
                << "Usage: inverse_fit [--selftest] [--scene studio|cornell]\n"
                << "  Studio θ: ground PBR[5] + pedestal[3] + key/fill/rim I + env scale\n"
                << "  B6 stages: multi-start → env → lights → albedo → brdf → pedestal → lights2 → refine\n"
                << "  --preset lantern|helmet|bottle|spheres|toycar|boombox|outdoor|mirror|chess|cornell\n"
                << "  --export-dataset N   ML data factory\n"
                << "  --multi-start N | --no-multi-start   (default 5 candidates)\n"
                << "  --light-reg W  --specular-weight W  --brdf-spp-mul M\n"
                << "  --map-ground           2×2 ground albedo tiles\n"
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

void addQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color) {
    auto mk = [&](glm::vec3 pos) {
        Vertex v{};
        v.position = pos;
        v.normal = normal;
        v.color = color;
        v.texCoord = {0, 0};
        v.tangent = {1, 0, 0, 1};
        v.boneIndices = glm::ivec4(0);
        v.boneWeights = {1, 0, 0, 0};
        return v;
    };
    verts = {mk(a), mk(b), mk(c), mk(d)};
    inds = {0, 1, 2, 0, 2, 3};
}

void addWall(Scene* scene, std::string_view name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color, float rough = 0.95f, float metal = 0.0f) {
    auto actor = scene->createActor(name);
    auto model = std::make_shared<Model>();
    addQuad(model->vertices, model->indices, a, b, c, d, normal, color);
    auto mesh = actor->addComponent<MeshComponent>();
    mesh->setModel(model);
    mesh->setVisible(true);
    auto mat = actor->addComponent<MaterialComponent>();
    mat->getMaterial().baseColor = color;
    mat->getMaterial().roughness = rough;
    mat->getMaterial().metallic = metal;
}

// Uniform metal-rough PBR on one surface (not textures).
struct PbrParams {
    glm::vec3 albedo{0.7f, 0.7f, 0.7f};
    float roughness{0.5f};
    float metallic{0.0f};

    [[nodiscard]] std::vector<double> asTheta() const {
        return {albedo.r, albedo.g, albedo.b, roughness, metallic};
    }
};

std::string formatTheta(const std::vector<double>& th) {
    if (th.empty()) return "[]";
    std::string s = "[";
    for (size_t i = 0; i < th.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(th[i]);
    }
    s += "]";
    return s;
}

void writeMatPbr(MaterialComponent* mat, Actor* actor, const PbrParams& p) {
    if (!mat) return;
    mat->getMaterial().baseColor = p.albedo;
    mat->getMaterial().roughness = p.roughness;
    mat->getMaterial().metallic = p.metallic;
    if (!actor) return;
    if (auto mesh = actor->getComponent<MeshComponent>()) {
        if (auto model = mesh->getModel()) {
            for (auto& v : model->vertices) v.color = p.albedo;
        }
    }
}

// Multi-surface + light inverse scene.
struct InverseScene {
    std::unique_ptr<Scene> scene;
    // Primary PBR surface (ground / left wall) — uniform mode
    Actor* primaryActor{nullptr};
    MaterialComponent* primaryMat{nullptr};
    // Optional 2×2 ground albedo tiles (mapGround): each tile is RGB in θ
    std::vector<Actor*> groundTiles;
    std::vector<MaterialComponent*> groundMats;
    bool mapGround{false};
    glm::vec3 truthTiles[4]{};
    glm::vec3 initTiles[4]{};
    // Secondary surface (pedestal top) — albedo only in θ
    Actor* pedestalActor{nullptr};
    MaterialComponent* pedestalMat{nullptr};
    LightComponent* keyLight{nullptr};
    LightComponent* fillLight{nullptr};
    LightComponent* rimLight{nullptr};
    Actor* keyLightActor{nullptr};
    Actor* fillLightActor{nullptr};
    Actor* rimLightActor{nullptr};
    Actor* heroActor{nullptr};
    glm::vec3 baseKeyPos{4.0f, 5.0f, 4.5f};
    glm::vec3 baseFillPos{-3.5f, 3.0f, 3.5f};
    glm::vec3 baseRimPos{-0.5f, 3.5f, -4.5f};
    std::vector<CameraView> baseViews;

    std::string envPath;
    std::string relightEnvPath;
    std::vector<CameraView> views;

    PbrParams truthPrimary{};
    PbrParams initPrimary{};
    PbrParams truthPedestal{.albedo = {0.18f, 0.19f, 0.21f}, .roughness = 0.4f, .metallic = 0.05f};
    PbrParams initPedestal{.albedo = {0.55f, 0.12f, 0.10f}, .roughness = 0.4f, .metallic = 0.05f};
    float truthKeyI{22.0f};
    float initKeyI{7.0f};
    float truthFillI{9.0f};
    float initFillI{3.0f};
    float truthRimI{10.0f};
    float initRimI{4.0f};
    float truthEnvScale{1.0f};
    float initEnvScale{0.45f};
    float currentEnvScale{1.0f};
    float currentExposure{1.0f};
    float truthExposure{1.0f};
    float initExposure{1.0f};
    // Light intensities stored as scale of kKeyIScale (Adam conditioning).
    static constexpr float kKeyIScale = 40.0f;
    bool fitPedestal{false};
    bool fitKeyLight{false};
    bool fitFillLight{false};
    bool fitRimLight{false};
    bool fitEnvScale{false};
    bool fitExposure{false};

    /// Primary block: 5 (uniform) or 14 (4×RGB + shared rough/metal).
    [[nodiscard]] size_t primaryDims() const noexcept { return mapGround ? 14u : 5u; }

    // Layout: primary | pedestal[3]? | key? fill? rim? | env? | exposure?
    [[nodiscard]] size_t thetaDims() const {
        size_t n = primaryDims();
        if (fitPedestal && pedestalMat) n += 3;
        if (fitKeyLight && keyLight) n += 1;
        if (fitFillLight && fillLight) n += 1;
        if (fitRimLight && rimLight) n += 1;
        if (fitEnvScale) n += 1;
        if (fitExposure) n += 1;
        return n;
    }

    [[nodiscard]] bool needsLightUpdate() const {
        return (fitKeyLight && keyLight) || (fitFillLight && fillLight) ||
               (fitRimLight && rimLight) || fitEnvScale;
    }

    /// Index of first light/env/exposure param (for regularizer).
    [[nodiscard]] size_t lightBlockStart() const {
        size_t i = primaryDims();
        if (fitPedestal && pedestalMat) i += 3;
        return i;
    }

    void appendLightTheta(std::vector<double>& t, float key, float fill, float rim,
                          float env, float exposure) const {
        if (fitKeyLight && keyLight) t.push_back(key / kKeyIScale);
        if (fitFillLight && fillLight) t.push_back(fill / kKeyIScale);
        if (fitRimLight && rimLight) t.push_back(rim / kKeyIScale);
        if (fitEnvScale) t.push_back(env);
        if (fitExposure) t.push_back(exposure);
    }

    void appendPrimaryTruth(std::vector<double>& t) const {
        if (mapGround) {
            for (int k = 0; k < 4; ++k) {
                t.push_back(truthTiles[k].r);
                t.push_back(truthTiles[k].g);
                t.push_back(truthTiles[k].b);
            }
            t.push_back(truthPrimary.roughness);
            t.push_back(truthPrimary.metallic);
        } else {
            auto p = truthPrimary.asTheta();
            t.insert(t.end(), p.begin(), p.end());
        }
    }

    void appendPrimaryInit(std::vector<double>& t) const {
        if (mapGround) {
            for (int k = 0; k < 4; ++k) {
                t.push_back(initTiles[k].r);
                t.push_back(initTiles[k].g);
                t.push_back(initTiles[k].b);
            }
            t.push_back(initPrimary.roughness);
            t.push_back(initPrimary.metallic);
        } else {
            auto p = initPrimary.asTheta();
            t.insert(t.end(), p.begin(), p.end());
        }
    }

    [[nodiscard]] std::vector<double> truthTheta() const {
        std::vector<double> t;
        t.reserve(thetaDims());
        appendPrimaryTruth(t);
        if (fitPedestal && pedestalMat) {
            t.push_back(truthPedestal.albedo.r);
            t.push_back(truthPedestal.albedo.g);
            t.push_back(truthPedestal.albedo.b);
        }
        appendLightTheta(t, truthKeyI, truthFillI, truthRimI, truthEnvScale, truthExposure);
        return t;
    }

    [[nodiscard]] std::vector<double> initTheta() const {
        std::vector<double> t;
        t.reserve(thetaDims());
        appendPrimaryInit(t);
        if (fitPedestal && pedestalMat) {
            t.push_back(initPedestal.albedo.r);
            t.push_back(initPedestal.albedo.g);
            t.push_back(initPedestal.albedo.b);
        }
        appendLightTheta(t, initKeyI, initFillI, initRimI, initEnvScale, initExposure);
        return t;
    }

    /// Random θ in box bounds (for dataset export / multi-start / ML).
    [[nodiscard]] std::vector<double> sampleRandomTheta(uint32_t rng) const {
        auto u01 = [&]() -> double {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<double>(rng >> 8) / static_cast<double>(1u << 24);
        };
        std::vector<double> t;
        t.reserve(thetaDims());
        if (mapGround) {
            for (int k = 0; k < 12; ++k) t.push_back(u01());
            t.push_back(0.04 + 0.96 * u01());
            t.push_back(u01());
        } else {
            for (int i = 0; i < 3; ++i) t.push_back(u01());
            t.push_back(0.04 + 0.96 * u01());
            t.push_back(u01());
        }
        if (fitPedestal && pedestalMat) {
            t.push_back(u01());
            t.push_back(u01());
            t.push_back(u01());
        }
        // Tighter light ranges match ParamSpace bounds (reduce multi-light blowout).
        if (fitKeyLight && keyLight) t.push_back(0.15 + 0.75 * u01());
        if (fitFillLight && fillLight) t.push_back(0.08 + 0.45 * u01());
        if (fitRimLight && rimLight) t.push_back(0.08 + 0.45 * u01());
        if (fitEnvScale) t.push_back(0.35 + 1.0 * u01());
        if (fitExposure) t.push_back(0.4 + 1.6 * u01());
        return t;
    }

    void applyPrimaryFromTheta(const std::vector<double>& th) {
        const size_t pd = primaryDims();
        if (th.size() < pd) return;
        if (mapGround && groundMats.size() == 4) {
            const float rough = static_cast<float>(th[12]);
            const float metal = static_cast<float>(th[13]);
            for (int k = 0; k < 4; ++k) {
                const size_t o = static_cast<size_t>(k) * 3;
                writeMatPbr(groundMats[static_cast<size_t>(k)], groundTiles[static_cast<size_t>(k)],
                            PbrParams{.albedo = {static_cast<float>(th[o]), static_cast<float>(th[o + 1]),
                                                 static_cast<float>(th[o + 2])},
                                      .roughness = rough,
                                      .metallic = metal});
            }
            // Keep primaryMat in sync with tile 0 for diagnostics.
            if (primaryMat) {
                writeMatPbr(primaryMat, primaryActor,
                            PbrParams{.albedo = {static_cast<float>(th[0]), static_cast<float>(th[1]),
                                                 static_cast<float>(th[2])},
                                      .roughness = rough,
                                      .metallic = metal});
            }
        } else {
            writeMatPbr(primaryMat, primaryActor,
                        PbrParams{.albedo = {static_cast<float>(th[0]), static_cast<float>(th[1]),
                                             static_cast<float>(th[2])},
                                  .roughness = static_cast<float>(th[3]),
                                  .metallic = static_cast<float>(th[4])});
        }
    }

    void applyTheta(const std::vector<double>& th) {
        if (th.size() < primaryDims()) return;
        applyPrimaryFromTheta(th);
        size_t i = primaryDims();
        if (fitPedestal && pedestalMat && th.size() >= i + 3) {
            PbrParams pp = truthPedestal;
            pp.albedo = {static_cast<float>(th[i]), static_cast<float>(th[i + 1]),
                         static_cast<float>(th[i + 2])};
            writeMatPbr(pedestalMat, pedestalActor, pp);
            i += 3;
        }
        if (fitKeyLight && keyLight && th.size() > i) {
            keyLight->setIntensity(static_cast<float>(th[i] * kKeyIScale));
            ++i;
        }
        if (fitFillLight && fillLight && th.size() > i) {
            fillLight->setIntensity(static_cast<float>(th[i] * kKeyIScale));
            ++i;
        }
        if (fitRimLight && rimLight && th.size() > i) {
            rimLight->setIntensity(static_cast<float>(th[i] * kKeyIScale));
            ++i;
        }
        if (fitEnvScale && th.size() > i) {
            currentEnvScale = static_cast<float>(th[i]);
            ++i;
        }
        if (fitExposure && th.size() > i) {
            currentExposure = static_cast<float>(th[i]);
        }
    }

    void applyTruth() { applyTheta(truthTheta()); }

    void applyCamera(Camera& cam, int viewIndex) const {
        const CameraView& v = views[static_cast<size_t>(viewIndex) % views.size()];
        cam.setPosition(v.position);
        cam.setRotation(v.pitchDeg, v.yawDeg);
        cam.setFov(40.0f);
    }

    static InverseScene buildCornell(const FitConfig& cfg) {
        InverseScene s;
        s.scene = std::make_unique<Scene>("Inverse Cornell");
        s.fitPedestal = false;
        s.fitKeyLight = cfg.fitKeyLight;
        s.fitFillLight = false;
        s.fitRimLight = false;
        s.fitEnvScale = false;
        s.fitExposure = false;
        s.mapGround = false;
        s.truthPrimary = {.albedo = {0.65f, 0.05f, 0.05f}, .roughness = 0.35f, .metallic = 0.0f};
        s.initPrimary = {.albedo = {0.2f, 0.5f, 0.7f}, .roughness = 0.85f, .metallic = 0.55f};
        s.truthKeyI = 40.0f;
        s.initKeyI = 12.0f;
        const float S = 5.0f;
        const glm::vec3 white(0.73f), red = s.truthPrimary.albedo, green(0.12f, 0.45f, 0.15f);
        glm::vec3 LBB(-S, -S, -S), RBB(S, -S, -S), LTB(-S, S, -S), RTB(S, S, -S);
        glm::vec3 LBF(-S, -S, S), RBF(S, -S, S), LTF(-S, S, S), RTF(S, S, S);
        addWall(s.scene.get(), "Back", LBB, RBB, RTB, LTB, {0, 0, 1}, white);
        addWall(s.scene.get(), "Left", LBB, LTB, LTF, LBF, {1, 0, 0}, red, s.truthPrimary.roughness,
                s.truthPrimary.metallic);
        addWall(s.scene.get(), "Right", RBB, RBF, RTF, RTB, {-1, 0, 0}, green);
        addWall(s.scene.get(), "Floor", LBB, LBF, RBF, RBB, {0, 1, 0}, white, 0.7f);
        addWall(s.scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0, -1, 0}, white);

        auto sphere = s.scene->createActorWithComponents("Sphere", PrimitiveType::Sphere);
        sphere->getTransform()->setPosition({0.0f, -S + 1.5f, 0.0f});
        sphere->getTransform()->setScale(glm::vec3(1.5f));
        auto sm = sphere->getComponent<MaterialComponent>();
        sm->getMaterial().baseColor = {0.8f, 0.8f, 0.8f};
        sm->getMaterial().roughness = 0.4f;
        sm->getMaterial().metallic = 0.0f;

        auto light = s.scene->createActor("KeyLight");
        auto lc = light->addComponent<LightComponent>();
        lc->setLightType(LightType::Sphere);
        lc->setColor({1.0f, 0.98f, 0.95f});
        lc->setIntensity(s.truthKeyI);
        lc->setRadius(0.6f);
        light->getTransform()->setPosition({0.0f, S - 0.5f, 0.0f});
        s.keyLight = lc.get();

        s.scene->forEachActor([&](Actor& a) {
            if (a.getName() == "Left") {
                s.primaryActor = &a;
                s.primaryMat = a.getComponent<MaterialComponent>().get();
            }
        });
        s.views = {
            {"front", {0.0f, 0.0f, 13.0f}, 0.0f, -90.0f},
            {"leftish", {-4.0f, 0.5f, 12.0f}, -2.0f, -110.0f},
            {"rightish", {4.0f, 0.5f, 12.0f}, -2.0f, -70.0f},
        };
        return s;
    }

    static InverseScene buildStudio(const FitConfig& cfg) {
        InverseScene s;
        s.scene = std::make_unique<Scene>("Inverse Product Studio");
        s.envPath = cfg.envPath;
        s.relightEnvPath = cfg.relightEnvPath;
        s.fitPedestal = cfg.fitPedestal;
        s.fitKeyLight = cfg.fitKeyLight;
        s.fitFillLight = cfg.fitFillLight && cfg.fitKeyLight;
        s.fitRimLight = cfg.fitRimLight && cfg.fitKeyLight;
        s.fitEnvScale = cfg.fitEnvScale;
        s.fitExposure = cfg.fitExposure && !cfg.targetImage.empty();
        s.mapGround = cfg.mapGround;
        // Floor PBR from preset / CLI defaults (tricky presets override metal/rough).
        s.truthPrimary = {.albedo = {cfg.truthR, cfg.truthG, cfg.truthB},
                          .roughness = cfg.truthRough,
                          .metallic = cfg.truthMetal};
        s.initPrimary = {.albedo = {cfg.initR, cfg.initG, cfg.initB},
                         .roughness = cfg.initRough,
                         .metallic = cfg.initMetal};
        // Distinct 2×2 tile truth (checker-ish) so map-ground selftest is meaningful.
        s.truthTiles[0] = {cfg.truthR, cfg.truthG, cfg.truthB};
        s.truthTiles[1] = {std::clamp(cfg.truthR * 0.75f, 0.05f, 1.0f),
                           std::clamp(cfg.truthG * 0.85f + 0.08f, 0.05f, 1.0f),
                           std::clamp(cfg.truthB * 1.15f, 0.05f, 1.0f)};
        s.truthTiles[2] = {std::clamp(cfg.truthR * 1.1f, 0.05f, 1.0f),
                           std::clamp(cfg.truthG * 0.7f, 0.05f, 1.0f),
                           std::clamp(cfg.truthB * 0.8f, 0.05f, 1.0f)};
        s.truthTiles[3] = {std::clamp(cfg.truthR * 0.55f + 0.1f, 0.05f, 1.0f),
                           std::clamp(cfg.truthG * 0.55f + 0.1f, 0.05f, 1.0f),
                           std::clamp(cfg.truthB * 0.55f + 0.1f, 0.05f, 1.0f)};
        s.initTiles[0] = {cfg.initR, cfg.initG, cfg.initB};
        s.initTiles[1] = {cfg.initB, cfg.initR, cfg.initG};
        s.initTiles[2] = {cfg.initG, cfg.initB, cfg.initR};
        s.initTiles[3] = {0.5f * (cfg.initR + cfg.initG), cfg.initB, cfg.initR};
        s.truthPedestal = {.albedo = {0.18f, 0.19f, 0.21f}, .roughness = 0.40f, .metallic = 0.05f};
        s.initPedestal = {.albedo = {0.55f, 0.12f, 0.10f}, .roughness = 0.40f, .metallic = 0.05f};
        s.truthKeyI = 22.0f;
        s.initKeyI = 7.0f;
        s.truthFillI = 9.0f;
        s.initFillI = 3.0f;
        s.truthRimI = 10.0f;
        s.initRimI = 4.0f;
        s.truthEnvScale = 1.0f;
        s.initEnvScale = 0.45f;
        s.currentEnvScale = 1.0f;
        s.truthExposure = 1.0f;
        s.initExposure = cfg.exposure > 0.0f ? cfg.exposure : 1.0f;
        s.currentExposure = s.initExposure;

        const float groundY = 0.0f;
        const float pedestalH = 0.35f;
        const float pedestalHalf = 1.35f;

        // ── Optimizable ground (uniform or 2×2 albedo tiles) ──────────
        {
            const float half = 14.0f;
            auto makeGroundTile = [&](const char* name, float x0, float z0, float x1, float z1,
                                      const glm::vec3& col) {
                auto ground = s.scene->createActor(name);
                auto gm = std::make_shared<Model>();
                addQuad(gm->vertices, gm->indices,
                        {x0, groundY, z0}, {x1, groundY, z0},
                        {x1, groundY, z1}, {x0, groundY, z1},
                        {0, 1, 0}, col);
                auto gmesh = ground->addComponent<MeshComponent>();
                gmesh->setModel(gm);
                gmesh->setVisible(true);
                auto gmat = ground->addComponent<MaterialComponent>();
                gmat->getMaterial().baseColor = col;
                gmat->getMaterial().roughness = s.truthPrimary.roughness;
                gmat->getMaterial().metallic = s.truthPrimary.metallic;
                return std::pair{ground.get(), gmat.get()};
            };

            if (s.mapGround) {
                const float m = 0.0f;
                const std::array<std::tuple<const char*, float, float, float, float, int>, 4> tiles{{
                    {"GroundNW", -half, -half, m, m, 0},
                    {"GroundNE", m, -half, half, m, 1},
                    {"GroundSW", -half, m, m, half, 2},
                    {"GroundSE", m, m, half, half, 3},
                }};
                for (const auto& [name, x0, z0, x1, z1, idx] : tiles) {
                    auto [actor, mat] = makeGroundTile(name, x0, z0, x1, z1, s.truthTiles[idx]);
                    s.groundTiles.push_back(actor);
                    s.groundMats.push_back(mat);
                }
                s.primaryActor = s.groundTiles[0];
                s.primaryMat = s.groundMats[0];
            } else {
                auto [actor, mat] =
                    makeGroundTile("Ground", -half, -half, half, half, s.truthPrimary.albedo);
                s.primaryActor = actor;
                s.primaryMat = mat;
            }
        }

        // ── Soft vertical backdrop (product-shot cyclorama) ────────────
        {
            const float halfW = 14.0f;
            const float h = 8.0f;
            const float z = -6.5f;
            const glm::vec3 backdropCol(0.82f, 0.84f, 0.88f);
            auto wall = s.scene->createActor("Backdrop");
            auto wm = std::make_shared<Model>();
            addQuad(wm->vertices, wm->indices,
                    {-halfW, groundY, z}, {halfW, groundY, z},
                    {halfW, groundY + h, z}, {-halfW, groundY + h, z},
                    {0, 0, 1}, backdropCol);
            auto mesh = wall->addComponent<MeshComponent>();
            mesh->setModel(wm);
            mesh->setVisible(true);
            auto mat = wall->addComponent<MaterialComponent>();
            mat->getMaterial().baseColor = backdropCol;
            mat->getMaterial().roughness = 0.85f;
            mat->getMaterial().metallic = 0.0f;
        }

        // ── Pedestal (top albedo is multi-surface θ; sides fixed slate) ─
        {
            const float y0 = groundY + 0.002f;
            const float y1 = groundY + pedestalH;
            const float h = pedestalHalf;
            const glm::vec3 slate = s.truthPedestal.albedo;
            auto makeFace = [&](const char* name, glm::vec3 a, glm::vec3 b, glm::vec3 c,
                                glm::vec3 d, glm::vec3 n, bool isTop) {
                auto actor = s.scene->createActor(name);
                auto m = std::make_shared<Model>();
                addQuad(m->vertices, m->indices, a, b, c, d, n, slate);
                auto mesh = actor->addComponent<MeshComponent>();
                mesh->setModel(m);
                mesh->setVisible(true);
                auto mat = actor->addComponent<MaterialComponent>();
                mat->getMaterial().baseColor = slate;
                mat->getMaterial().roughness = s.truthPedestal.roughness;
                mat->getMaterial().metallic = s.truthPedestal.metallic;
                if (isTop) {
                    s.pedestalActor = actor.get();
                    s.pedestalMat = mat.get();
                }
            };
            makeFace("PedestalTop", {-h, y1, -h}, {h, y1, -h}, {h, y1, h}, {-h, y1, h},
                     {0, 1, 0}, true);
            makeFace("PedestalFront", {-h, y0, h}, {h, y0, h}, {h, y1, h}, {-h, y1, h},
                     {0, 0, 1}, false);
            makeFace("PedestalBack", {h, y0, -h}, {-h, y0, -h}, {-h, y1, -h}, {h, y1, -h},
                     {0, 0, -1}, false);
            makeFace("PedestalLeft", {-h, y0, -h}, {-h, y0, h}, {-h, y1, h}, {-h, y1, -h},
                     {-1, 0, 0}, false);
            makeFace("PedestalRight", {h, y0, h}, {h, y0, -h}, {h, y1, -h}, {h, y1, h},
                     {1, 0, 0}, false);
        }

        // ── Hero model on pedestal ─────────────────────────────────────
        auto model = ModelLoader::load(cfg.modelPath);
        if (model && !model->vertices.empty()) {
            glm::vec3 bmin(1e30f), bmax(-1e30f);
            for (const auto& v : model->vertices) {
                bmin = glm::min(bmin, v.position);
                bmax = glm::max(bmax, v.position);
            }
            const glm::vec3 extent = bmax - bmin;
            const glm::vec3 centerXZ{(bmin.x + bmax.x) * 0.5f, 0.0f, (bmin.z + bmax.z) * 0.5f};
            for (auto& v : model->vertices) {
                v.position.x -= centerXZ.x;
                v.position.y -= bmin.y;
                v.position.z -= centerXZ.z;
            }
            const float maxExtent = std::max({extent.x, extent.y, extent.z});
            const float scale =
                (2.0f * cfg.heroScaleMul) / std::max(extent.y > 0.01f ? extent.y : maxExtent, 0.001f);

            auto actor = s.scene->createActor("Hero");
            actor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, 22.0f, 0.0f))));
            actor->getTransform()->setScale(glm::vec3(scale));
            actor->getTransform()->setPosition({0.0f, groundY + pedestalH + 0.002f, 0.0f});
            s.heroActor = actor.get();

            auto mesh = actor->addComponent<MeshComponent>();
            mesh->setModel(model);
            mesh->setVisible(true);
            auto mat = actor->addComponent<MaterialComponent>();
            mat->getMaterial().roughness = 0.35f;
            std::cout << "  loaded hero: " << cfg.modelPath << " (" << model->vertices.size()
                      << " verts, scale=" << scale << ", h=" << (extent.y * scale) << ")\n";
        } else {
            std::cerr << "  WARN: could not load " << cfg.modelPath
                      << " — product studio without hero\n";
        }

        {
            auto a = s.scene->createActor("Key");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({1.0f, 0.97f, 0.92f});
            lc->setIntensity(s.truthKeyI);
            lc->setRadius(1.0f);
            a->getTransform()->setPosition(s.baseKeyPos);
            s.keyLight = lc.get();
            s.keyLightActor = a.get();
        }
        {
            auto a = s.scene->createActor("Fill");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({0.75f, 0.85f, 1.0f});
            lc->setIntensity(s.truthFillI);
            lc->setRadius(1.4f);
            a->getTransform()->setPosition(s.baseFillPos);
            s.fillLight = lc.get();
            s.fillLightActor = a.get();
        }
        {
            auto a = s.scene->createActor("Rim");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({1.0f, 0.95f, 0.9f});
            lc->setIntensity(s.truthRimI);
            lc->setRadius(0.7f);
            a->getTransform()->setPosition(s.baseRimPos);
            s.rimLight = lc.get();
            s.rimLightActor = a.get();
        }

        const float d = cfg.camDistMul;
        s.views = {
            {"front", {0.0f, 1.35f * d, 7.2f * d}, -8.0f, -90.0f},
            {"three_quarter", {5.0f * d, 1.55f * d, 5.6f * d}, -10.0f, -48.0f},
            {"opposite", {-4.8f * d, 1.35f * d, 5.4f * d}, -8.0f, -128.0f},
        };
        s.baseViews = s.views;
        return s;
    }

    /// L2 domain randomization for export (θ dims unchanged).
    int applyDomainRandomization(uint32_t& rng) {
        auto u01 = [&]() -> float {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) / static_cast<float>(1u << 24);
        };
        auto n11 = [&]() -> float { return u01() * 2.0f - 1.0f; };

        auto jitterPos = [&](Actor* a, const glm::vec3& base, float amp) {
            if (!a) return;
            a->getTransform()->setPosition(
                base + glm::vec3(n11() * amp, n11() * amp * 0.45f, n11() * amp));
        };
        jitterPos(keyLightActor, baseKeyPos, 1.25f);
        jitterPos(fillLightActor, baseFillPos, 1.05f);
        jitterPos(rimLightActor, baseRimPos, 1.05f);

        if (heroActor) {
            const float yaw = 8.0f + u01() * 55.0f;
            heroActor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, yaw, 0.0f))));
        }

        if (baseViews.empty()) baseViews = views;
        const float distScale = 1.0f + n11() * 0.14f;
        const float yaw = -90.0f + n11() * 58.0f;
        const float pitch = -9.0f + n11() * 7.0f;
        const float elev = 1.25f + n11() * 0.4f;
        const float dist = 6.9f * distScale;
        const float yawRad = glm::radians(yaw + 90.0f);
        CameraView orbit{"orbit",
                         {std::cos(yawRad) * dist, elev, std::sin(yawRad) * dist},
                         pitch,
                         yaw};
        views.clear();
        views.push_back(orbit);
        for (const auto& b : baseViews) views.push_back(b);
        return 0;
    }

    void resetDomainDefaults() {
        if (keyLightActor) keyLightActor->getTransform()->setPosition(baseKeyPos);
        if (fillLightActor) fillLightActor->getTransform()->setPosition(baseFillPos);
        if (rimLightActor) rimLightActor->getTransform()->setPosition(baseRimPos);
        if (heroActor) {
            heroActor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, 22.0f, 0.0f))));
        }
        if (!baseViews.empty()) views = baseViews;
    }
};


/// Specular proxy for target image (floor/wall crop).
/// Combines bright-pixel fraction with max/mean luma contrast — mirror floors under
/// soft studio HDRI often lack crushed whites but still show high peak contrast.
[[nodiscard]] double targetHighlightScore(const ImageRGBA8& img, double xMaxFrac, double yMinFrac) {
    if (img.empty()) return 0.0;
    const uint32_t xLim = (xMaxFrac >= 1.0)
                              ? img.width
                              : static_cast<uint32_t>(std::ceil(xMaxFrac * img.width));
    const uint32_t y0 = (yMinFrac <= 0.0)
                            ? 0u
                            : static_cast<uint32_t>(std::floor(yMinFrac * img.height));
    size_t bright = 0, count = 0;
    double sumL = 0.0, maxL = 0.0, sumMax = 0.0;
    for (uint32_t y = y0; y < img.height; ++y) {
        for (uint32_t x = 0; x < xLim; ++x) {
            const size_t o = (static_cast<size_t>(y) * img.width + x) * 4;
            const double r = img.rgba[o] / 255.0;
            const double g = img.rgba[o + 1] / 255.0;
            const double b = img.rgba[o + 2] / 255.0;
            const double mx = std::max({r, g, b});
            const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            if (mx > 0.55 || luma > 0.50) ++bright;
            sumL += luma;
            sumMax += mx;
            maxL = std::max(maxL, luma);
            ++count;
        }
    }
    if (count == 0) return 0.0;
    const double frac = static_cast<double>(bright) / static_cast<double>(count);
    const double meanL = sumL / static_cast<double>(count);
    const double meanMax = sumMax / static_cast<double>(count);
    const double contrast = maxL / (meanL + 1e-3);
    // Soft blend: bright fraction + normalized peak contrast + mean-max lift.
    const double cScore = std::clamp((contrast - 1.4) / 2.5, 0.0, 1.0);
    const double mScore = std::clamp((meanMax - 0.25) / 0.55, 0.0, 1.0);
    return std::clamp(0.40 * frac + 0.35 * cScore + 0.25 * mScore, 0.0, 1.0);
}

ImageRGBA8 loadPNG(const std::filesystem::path& path) {
    ImageRGBA8 img;
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return img;
    }
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    img.rgba.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    return img;
}

/// Load C1 θ prior JSON: `{"theta":[...]}` or bare `[...]`.
/// Prefer the array after `"theta"` so names/metadata arrays are ignored.
bool loadThetaInit(const std::filesystem::path& path, std::vector<double>& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string s = buf.str();
    size_t lb = std::string::npos;
    const auto key = s.find("\"theta\"");
    if (key != std::string::npos) {
        lb = s.find('[', key);
    }
    if (lb == std::string::npos) {
        lb = s.find('['); // bare array fallback
    }
    if (lb == std::string::npos) return false;
    // Match the bracket for this array only (not rfind of whole file).
    int depth = 0;
    size_t rb = std::string::npos;
    for (size_t i = lb; i < s.size(); ++i) {
        if (s[i] == '[') ++depth;
        else if (s[i] == ']') {
            --depth;
            if (depth == 0) {
                rb = i;
                break;
            }
        }
    }
    if (rb == std::string::npos || rb <= lb) return false;
    out.clear();
    std::stringstream arr(s.substr(lb + 1, rb - lb - 1));
    std::string tok;
    while (std::getline(arr, tok, ',')) {
        size_t a = 0, b = tok.size();
        while (a < b && std::isspace(static_cast<unsigned char>(tok[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1]))) --b;
        if (a >= b) continue;
        // Skip non-numeric tokens (e.g. leftover strings)
        const char c0 = tok[a];
        if (!(c0 == '-' || c0 == '+' || c0 == '.' || (c0 >= '0' && c0 <= '9'))) continue;
        try {
            out.push_back(std::stod(tok.substr(a, b - a)));
        } catch (...) {
            return false;
        }
    }
    return !out.empty();
}

bool savePNG(const ImageRGBA8& img, const std::filesystem::path& path) {
    if (img.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    return stbi_write_png(path.string().c_str(), static_cast<int>(img.width),
                          static_cast<int>(img.height), 4, img.rgba.data(),
                          static_cast<int>(img.width * 4)) != 0;
}

void applyEnv(VulkanRenderer& renderer, const std::string& path) {
    if (!path.empty() && std::filesystem::exists(path)) {
        renderer.setEnvironmentMap(path);
    }
}

// Avoid re-uploading the full scene/HDR every eval (was OOM/segfault under FD).
struct RenderSession {
    VulkanRenderer& renderer;
    InverseScene& inv;
    bool bound{false};

    ImageRGBA8 render(int viewIndex, const RenderBudget& budget, uint32_t seed,
                      DenoiseMode denoise) {
        // Prefer fixed size (set at construct). Resize only if explicitly needed
        // and sizes differ (cornell dual-res); studio keeps one size.
        const bool needResize =
            (renderer.getWidth() != budget.width || renderer.getHeight() != budget.height) &&
            budget.width > 0 && budget.height > 0;
        if (needResize) {
            renderer.resize(budget.width, budget.height);
            // RT images/pipelines recreated — force full scene rebind once.
            bound = false;
        }
        inv.applyCamera(renderer.getCamera(), viewIndex);
        renderer.setDenoiseMode(denoise);
        renderer.setRenderSeed(seed + static_cast<uint32_t>(viewIndex) * 9973u);
        renderer.setEnvIntensityScale(inv.currentEnvScale);
        if (!bound) {
            renderer.setScene(inv.scene.get());
            bound = true;
        } else {
            // Material + light/env-scale edits only — never rebuild BLAS / reload HDR.
            const bool matsOk = renderer.updateRTMaterialParams();
            const bool lightsOk =
                inv.needsLightUpdate() ? renderer.updateRTLightParams() : true;
            if (!matsOk || (inv.needsLightUpdate() && !lightsOk)) {
                (void)renderer.updateSceneBuffers();
            }
        }
        renderer.resetAccumulation();
        const int frames = budget.spp + 3;
        for (int i = 0; i < frames; ++i) renderer.render();
        return ImageRGBA8::fromSpan(renderer.getWidth(), renderer.getHeight(),
                                    renderer.getPixelSpan());
    }

    void rebindEnv(const std::string& path) {
        applyEnv(renderer, path);
        bound = false; // next render will setScene and pick up new env once
    }
};

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);
    FitConfig cfg = args.cfg;
    // --scene cornell must survive applyPreset (which defaults other presets to studio).
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
    // Dual-res FIT/SHOW is OK now (mat/light-only GPU updates, no BLAS thrash).
    // Forcing FIT=SHOW size made multi-view draft FIT loss unresponsive (loss≈0).
    if (!studio) cfg.fitPedestal = false;

    InverseScene inv =
        studio ? InverseScene::buildStudio(cfg) : InverseScene::buildCornell(cfg);
    if (!inv.primaryMat) {
        std::cerr << "FATAL: optimizable primary material missing\n";
        return 1;
    }

    std::cout << "OHAO inverse_fit — B6 physical multi-param IR (no ML)\n";
    std::cout << "  preset=" << cfg.preset << "  (" << cfg.presetNote << ")\n";
    std::cout << "  model=" << cfg.modelPath << "\n";
    std::cout << "  θ dims=" << inv.thetaDims() << "  primary PBR[5]";
    if (inv.fitPedestal && inv.pedestalMat) std::cout << " + pedestal[3]";
    if (inv.fitKeyLight && inv.keyLight) std::cout << " + key";
    if (inv.fitFillLight && inv.fillLight) std::cout << " + fill";
    if (inv.fitRimLight && inv.rimLight) std::cout << " + rim";
    if (inv.fitEnvScale) std::cout << " + env";
    std::cout << "\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << cfg.numViews
              << "  quality=" << cfg.quality.name
              << (cfg.mapGround ? "  map-ground" : "")
              << (cfg.multiStart > 1 ? ("  multi-start=" + std::to_string(cfg.multiStart)) : "")
              << (!cfg.targetImage.empty() ? "  target-image" : "") << "\n";
    std::cout << "  FIT " << cfg.fit.width << "x" << cfg.fit.height << " @" << cfg.fit.spp
              << "  SHOW " << cfg.show.width << "x" << cfg.show.height << " @" << cfg.show.spp
              << "  denoise SHOW=" << denoiseModeName(cfg.showDenoise)
              << " FIT=none\n";
    if (cfg.exportDataset > 0) {
        std::cout << "  mode=export-dataset N=" << cfg.exportDataset << "\n";
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

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir);

    // ── Optional: ML data factory (θ → FIT image pairs) for C1 ────────
    if (cfg.exportDataset > 0) {
        const auto dataDir = outDir / "dataset";
        std::filesystem::create_directories(dataDir);

        // Param names match ParamSpace layout (for trainer column weighting).
        std::vector<std::string> dimNames;
        if (inv.mapGround) {
            for (int t = 0; t < 4; ++t) {
                dimNames.push_back("tile" + std::to_string(t) + ".R");
                dimNames.push_back("tile" + std::to_string(t) + ".G");
                dimNames.push_back("tile" + std::to_string(t) + ".B");
            }
            dimNames.push_back("primary.rough");
            dimNames.push_back("primary.metal");
        } else {
            dimNames = {"primary.R", "primary.G", "primary.B", "primary.rough", "primary.metal"};
        }
        if (inv.fitPedestal && inv.pedestalMat) {
            dimNames.push_back("pedestal.R");
            dimNames.push_back("pedestal.G");
            dimNames.push_back("pedestal.B");
        }
        if (inv.fitKeyLight && inv.keyLight) dimNames.push_back("key.I_scale");
        if (inv.fitFillLight && inv.fillLight) dimNames.push_back("fill.I_scale");
        if (inv.fitRimLight && inv.rimLight) dimNames.push_back("rim.I_scale");
        if (inv.fitEnvScale) dimNames.push_back("env.scale");
        if (inv.fitExposure) dimNames.push_back("exposure");

        {
            std::ofstream cfgOut(dataDir / "config.json");
            cfgOut << "{\n"
                   << "  \"format\": \"ohao_inverse_c1\",\n"
                   << "  \"version\": 2,\n"
                   << "  \"dims\": " << inv.thetaDims() << ",\n"
                   << "  \"preset\": \"" << cfg.preset << "\",\n"
                   << "  \"scene\": \"" << cfg.scene << "\",\n"
                   << "  \"map_ground\": " << (inv.mapGround ? "true" : "false") << ",\n"
                   << "  \"domain_rand\": " << (cfg.domainRand ? "true" : "false") << ",\n"
                   << "  \"exposure_jitter\": " << (cfg.exportExposureJitter ? "true" : "false")
                   << ",\n"
                   << "  \"export_view_mode\": " << cfg.exportViewMode << ",\n"
                   << "  \"fit\": {\"width\": " << cfg.fit.width << ", \"height\": " << cfg.fit.height
                   << ", \"spp\": " << cfg.fit.spp << "},\n"
                   << "  \"names\": [";
            for (size_t i = 0; i < dimNames.size(); ++i) {
                if (i) cfgOut << ", ";
                cfgOut << "\"" << dimNames[i] << "\"";
            }
            cfgOut << "]\n}\n";
        }

        std::ofstream meta(dataDir / "meta.jsonl");
        meta << "{\"type\":\"header\",\"dims\":" << inv.thetaDims()
             << ",\"fit\":[" << cfg.fit.width << "," << cfg.fit.height << "," << cfg.fit.spp
             << "],\"scene\":\"" << cfg.scene << "\",\"preset\":\"" << cfg.preset
             << "\",\"map_ground\":" << (inv.mapGround ? "true" : "false")
             << ",\"domain_rand\":" << (cfg.domainRand ? "true" : "false")
             << ",\"version\":2}\n";
        std::cout << "Exporting " << cfg.exportDataset << " (θ, image) pairs to " << dataDir
                  << " (C1 factory"
                  << (cfg.domainRand ? ", domain-rand" : "")
                  << (cfg.exportExposureJitter ? ", exp-jitter" : "") << ")...\n";

        int written = 0;
        for (int i = 0; i < cfg.exportDataset; ++i) {
            uint32_t rng = cfg.seed + static_cast<uint32_t>(i) * 2654435761u;
            const auto th = inv.sampleRandomTheta(rng);
            inv.applyTheta(th);

            int viewIdx = 0;
            std::string viewName = "front";
            if (cfg.domainRand && studio) {
                viewIdx = inv.applyDomainRandomization(rng);
                viewName = inv.views.empty() ? "orbit" : inv.views[0].name;
                session.bound = false; // transforms changed — full rebind
            } else if (cfg.exportViewMode == 1 && !inv.views.empty()) {
                viewIdx = static_cast<int>(rng % static_cast<uint32_t>(inv.views.size()));
                viewName = inv.views[static_cast<size_t>(viewIdx)].name;
            }

            auto emitSample = [&](int vIdx, const std::string& vName, int sampleId) {
                ImageRGBA8 img =
                    session.render(vIdx, cfg.fit, cfg.seed + static_cast<uint32_t>(sampleId),
                                   DenoiseMode::None);
                float expGain = 1.0f;
                if (cfg.exportExposureJitter) {
                    // Mild LDR exposure domain gap (C2/L4-lite)
                    uint32_t er = cfg.seed + static_cast<uint32_t>(sampleId) * 97u + 13u;
                    er = er * 1664525u + 1013904223u;
                    const float u = static_cast<float>(er >> 8) / static_cast<float>(1u << 24);
                    expGain = 0.75f + u * 0.55f; // [0.75, 1.30]
                    img = applyExposure(img, expGain);
                }
                char name[40];
                std::snprintf(name, sizeof(name), "%05d.png", sampleId);
                savePNG(img, dataDir / name);
                meta << "{\"i\":" << sampleId << ",\"file\":\"" << name << "\",\"preset\":\""
                     << cfg.preset << "\",\"scene\":\"" << cfg.scene << "\",\"view\":\"" << vName
                     << "\",\"domain_rand\":" << (cfg.domainRand ? "true" : "false")
                     << ",\"exposure\":" << expGain << ",\"theta\":[";
                for (size_t k = 0; k < th.size(); ++k) {
                    if (k) meta << ",";
                    meta << th[k];
                }
                meta << "]}\n";
            };

            if (cfg.exportViewMode == 2 && !inv.views.empty() && !cfg.domainRand) {
                for (int v = 0; v < static_cast<int>(inv.views.size()); ++v) {
                    emitSample(v, inv.views[static_cast<size_t>(v)].name, written);
                    ++written;
                }
            } else {
                emitSample(viewIdx, viewName, written);
                ++written;
            }

            if (cfg.domainRand && studio) inv.resetDomainDefaults();

            if ((i + 1) % 10 == 0 || i + 1 == cfg.exportDataset) {
                std::cout << "  " << (i + 1) << "/" << cfg.exportDataset << " (files=" << written
                          << ")\n";
            }
        }
        meta.close();
        std::cout << "Dataset export done → " << dataDir << " (" << written << " images)\n"
                  << "  Train: python3 tools/inverse_c1/train.py --data " << dataDir << "\n";
        inv.scene.reset();
        return 0;
    }

    // ── Targets: synthetic truth renders OR external photo ───────────
    inv.applyTruth();
    std::vector<ImageRGBA8> targetsFit(static_cast<size_t>(nViews));
    std::vector<ImageRGBA8> targetsShow(static_cast<size_t>(nViews));

    const auto truthV = inv.truthTheta();
    const bool externalTarget = !cfg.targetImage.empty();
    auto t0 = std::chrono::steady_clock::now();

    if (externalTarget) {
        if (!std::filesystem::exists(cfg.targetImage)) {
            std::cerr << "FATAL: --target-image not found: " << cfg.targetImage << "\n";
            return 1;
        }
        ImageRGBA8 loaded = loadPNG(cfg.targetImage);
        if (loaded.empty()) {
            std::cerr << "FATAL: failed to load --target-image: " << cfg.targetImage << "\n";
            return 1;
        }
        if (!cfg.fitExposure) {
            loaded = applyExposure(loaded, cfg.exposure);
        }
        std::cout << "External target " << cfg.targetImage << " (" << loaded.width << "x"
                  << loaded.height << ") exposure="
                  << (cfg.fitExposure ? "fit" : std::to_string(cfg.exposure)) << "\n";
        // Photo path: primary view only (multi-view needs multi-shot capture).
        nViews = 1;
        targetsShow.resize(1);
        targetsFit.resize(1);
        targetsShow[0] = resizeNearest(loaded, cfg.show.width, cfg.show.height);
        targetsFit[0] = resizeNearest(loaded, cfg.fit.width, cfg.fit.height);
        savePNG(targetsShow[0], outDir / "target_show.png");
        savePNG(targetsFit[0], outDir / "target_fit.png");
        savePNG(targetsShow[0], outDir / "target_front.png");
    } else {
        std::cout << "Rendering multi-view TARGETS (truth θ=" << formatTheta(truthV) << ")...\n";
        for (int v = 0; v < nViews; ++v) {
            std::cout << "  SHOW " << inv.views[static_cast<size_t>(v)].name << "...\n";
            targetsShow[static_cast<size_t>(v)] =
                session.render(v, cfg.show, cfg.seed, cfg.showDenoise);
            savePNG(targetsShow[static_cast<size_t>(v)],
                    outDir / (std::string("target_") + inv.views[static_cast<size_t>(v)].name + ".png"));
            targetsFit[static_cast<size_t>(v)] =
                session.render(v, cfg.fit, cfg.seed, DenoiseMode::None);
        }
        savePNG(targetsShow[0], outDir / "target_show.png");
    }
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  targets done in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms\n";

    // C1: optional auto-infer θ prior from FIT target via Python trainer.
    if (!cfg.nnModelPath.empty() && cfg.thetaInitPath.empty()) {
        if (!std::filesystem::exists(cfg.nnModelPath)) {
            std::cerr << "FATAL: --nn-model not found: " << cfg.nnModelPath << "\n";
            return 1;
        }
        const auto fitTargetPath = outDir / "target_fit.png";
        if (targetsFit[0].empty() || !savePNG(targetsFit[0], fitTargetPath)) {
            // Fall back to SHOW if FIT empty
            if (!savePNG(targetsShow[0], fitTargetPath)) {
                std::cerr << "FATAL: could not write FIT target for NN infer\n";
                return 1;
            }
        }
        const auto thetaPath = outDir / "theta_prior.json";
        // Prefer repo-relative infer script
        std::filesystem::path inferScript = "tools/inverse_c1/infer.py";
        if (!std::filesystem::exists(inferScript)) {
            inferScript = std::filesystem::path("..") / inferScript;
        }
        std::ostringstream cmd;
        cmd << cfg.nnPython << " " << inferScript.string() << " --model "
            << std::filesystem::absolute(cfg.nnModelPath).string() << " --image "
            << std::filesystem::absolute(fitTargetPath).string() << " --out "
            << std::filesystem::absolute(thetaPath).string();
        std::cout << "C1 NN infer: " << cmd.str() << "\n";
        const int rc = std::system(cmd.str().c_str());
        if (rc != 0 || !std::filesystem::exists(thetaPath)) {
            std::cerr << "FATAL: NN infer failed (rc=" << rc << ")\n";
            return 1;
        }
        cfg.thetaInitPath = thetaPath.string();
    }

    // Build ParamSpace from init θ — tighter light bounds reduce multi-light ambiguity.
    ParamSpace space;
    const auto initV = inv.initTheta();
    if (inv.mapGround) {
        for (int t = 0; t < 4; ++t) {
            const size_t o = static_cast<size_t>(t) * 3;
            const std::string pref = "tile" + std::to_string(t) + ".";
            space.add(pref + "R", initV[o], 0.0, 1.0);
            space.add(pref + "G", initV[o + 1], 0.0, 1.0);
            space.add(pref + "B", initV[o + 2], 0.0, 1.0);
        }
        space.add("primary.rough", initV[12], 0.04, 1.0);
        space.add("primary.metal", initV[13], 0.0, 1.0);
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

    auto applyTheta = [&](const std::vector<double>& th) { inv.applyTheta(th); };

    // Always capture the *wrong* multi-param guess first (true BEFORE for compare sheets).
    std::cout << "SHOW init (wrong multi-param guess)...\n";
    applyTheta(space.values);
    ImageRGBA8 wrongInitShow = session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
    if (inv.fitExposure) wrongInitShow = applyExposure(wrongInitShow, inv.currentExposure);
    savePNG(wrongInitShow, outDir / "init_show.png"); // BEFORE
    savePNG(wrongInitShow, outDir / "init_wrong_show.png");
    const double showWrongRmse = rmseRGB(wrongInitShow, targetsShow[0]);
    std::cout << "  wrong-init SHOW RMSE vs target=" << showWrongRmse << "\n";

    // C1: override with neural θ prior (tools/inverse_c1/infer.py).
    bool usedNnPrior = false;
    if (!cfg.thetaInitPath.empty()) {
        std::vector<double> nnTh;
        if (!loadThetaInit(cfg.thetaInitPath, nnTh)) {
            std::cerr << "FATAL: failed to parse --theta-init " << cfg.thetaInitPath << "\n";
            return 1;
        }
        if (nnTh.size() != space.size()) {
            std::cerr << "FATAL: --theta-init dims " << nnTh.size() << " != space " << space.size()
                      << "\n";
            return 1;
        }
        for (size_t i = 0; i < space.size(); ++i) {
            space.values[i] = space.project(i, nnTh[i]);
        }
        usedNnPrior = true;
        if (cfg.nnSkipMultiStart) cfg.multiStart = 1;
        std::cout << "C1 NN prior loaded from " << cfg.thetaInitPath
                  << "  θ=" << formatTheta(space.values) << "\n";
        applyTheta(space.values);
        ImageRGBA8 nnShow = session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        if (inv.fitExposure) nnShow = applyExposure(nnShow, inv.currentExposure);
        savePNG(nnShow, outDir / "nn_prior_show.png");
        std::cout << "  NN-prior SHOW RMSE vs target=" << rmseRGB(nnShow, targetsShow[0]) << "\n";
    }

    const size_t roughIdx = inv.mapGround ? 12u : 3u;
    const size_t metalIdx = inv.mapGround ? 13u : 4u;
    double highlightScore =
        targetHighlightScore(targetsFit[0], cfg.maskX, cfg.maskYMin);
    // Presets with known high-metal floors get a floor on the score (image metric alone is soft under LDR).
    if (cfg.preset == "mirror" || cfg.preset == "spheres") {
        highlightScore = std::max(highlightScore, 0.28);
    }
    std::cout << "  target highlight score=" << highlightScore
              << (highlightScore > 0.18 ? " (specular-ish)" : (highlightScore > 0.12 ? " (mixed)" : " (diffuse-ish)")) << "\n";

    // Soft multi-light regularizer: key-dominant hierarchy + mild mid-prior.
    // Breaks pure intensity trade-offs under HDRI without hardcoding truth.
    auto lightRegularizer = [&](const std::vector<double>& th) -> double {
        if (cfg.lightReg <= 0.0) return 0.0;
        size_t i = inv.lightBlockStart();
        double reg = 0.0;
        double key = 0.0, fill = 0.0, rim = 0.0, env = 0.0;
        bool hasKey = false, hasFill = false, hasRim = false, hasEnv = false;
        if (inv.fitKeyLight && inv.keyLight && th.size() > i) {
            key = th[i++];
            hasKey = true;
            // Soft prior toward mid of studio range (~0.55 = truth-ish scale, mild).
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
        // Hierarchy: key should dominate fill/rim (product key light).
        if (hasKey && hasFill && fill > key) reg += 2.0 * (fill - key) * (fill - key);
        if (hasKey && hasRim && rim > key) reg += 2.0 * (rim - key) * (rim - key);
        // Soft total energy ball (prevents all lights cranking together).
        if (hasKey || hasFill || hasRim) {
            const double total = (hasKey ? key : 0.0) + (hasFill ? fill : 0.0) + (hasRim ? rim : 0.0);
            const double targetTotal = 0.50 + (hasFill ? 0.22 : 0.0) + (hasRim ? 0.25 : 0.0);
            reg += 0.35 * (total - targetTotal) * (total - targetTotal);
        }
        (void)hasEnv;
        (void)env;
        return cfg.lightReg * reg;
    };

    // Multi-view hybrid + specular FIT loss. sppScale for high-spp BRDF stages.
    // Exposure (if fitted) is applied to the render before comparison.
    auto lossAt = [&](const std::vector<double>& th, float sppScale = 1.0f,
                      double specularW = -1.0) -> double {
        applyTheta(th);
        RenderBudget budget = cfg.fit;
        if (sppScale > 1.001f) {
            budget.spp = std::max(1, static_cast<int>(std::lround(budget.spp * sppScale)));
        }
        const double sw = (specularW >= 0.0) ? specularW : cfg.specularWeight;
        double L = 0.0;
        double wSum = 0.0;
        for (int v = 0; v < nViews; ++v) {
            ImageRGBA8 img =
                session.render(v, budget, cfg.seed, DenoiseMode::None);
            if (img.empty() || targetsFit[static_cast<size_t>(v)].empty()) continue;
            if (inv.fitExposure) {
                img = applyExposure(img, inv.currentExposure);
            }
            const double m =
                hybridSpecularRGB(img, targetsFit[static_cast<size_t>(v)], cfg.maskX,
                                  cfg.maskYMin, 0.35, sw);
            if (!std::isfinite(m)) continue;
            const double w = (v == 0) ? 1.0 : 0.5;
            L += w * m;
            wSum += w;
        }
        const double imgLoss = wSum > 0.0 ? L / wSum : 1e6;
        // Target-driven metal prior — only for clearly specular targets (mirror/spheres).
        // Soft floors under studio HDRI often look "bright" without being metal; don't push them.
        double metalPrior = 0.0;
        if (th.size() > metalIdx) {
            const double metal = th[metalIdx];
            const double rough = th[roughIdx];
            if (highlightScore > 0.26) {
                const double targetM = 0.70 + 0.22 * std::min(1.0, (highlightScore - 0.26) * 2.0);
                const double w = 0.06 + 0.08 * highlightScore;
                metalPrior += w * (metal - targetM) * (metal - targetM);
                metalPrior += 0.4 * w * (rough - 0.12) * (rough - 0.12);
            } else if (highlightScore < 0.12) {
                // Mild diffuse preference — don't fight image loss hard.
                metalPrior += 0.015 * (metal - 0.12) * (metal - 0.12);
            }
        }
        return imgLoss + lightRegularizer(th) + metalPrior;
    };

    applyTheta(space.values);
    ImageRGBA8 initShow = session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
    if (inv.fitExposure) initShow = applyExposure(initShow, inv.currentExposure);
    // Keep init_show.png as the *optimization* start (NN prior if used, else wrong guess).
    // Wrong guess is always also at init_wrong_show.png for before/after sheets.
    if (usedNnPrior) {
        savePNG(initShow, outDir / "init_opt_start_show.png");
    } else {
        savePNG(initShow, outDir / "init_show.png");
    }

    double loss = lossAt(space.values);
    const double showInitRmse = rmseRGB(initShow, targetsShow[0]);
    std::cout << "  fit-start FIT loss=" << loss << "  SHOW RMSE vs target=" << showInitRmse
              << "\n  θ=" << formatTheta(space.values) << "\n";

    // ── Multi-start: probe candidates, keep lowest loss ──────────────
    // Critical for mirror/metal and multi-light: bad init traps are common.
    if (cfg.multiStart > 1) {
        std::cout << "── multi-start probe (" << cfg.multiStart << " candidates) ──\n";
        std::vector<double> bestStart = space.values;
        double bestStartLoss = loss;
        std::vector<std::vector<double>> candidates;
        candidates.push_back(initV);

        // Mid-gray materials + mid lights (safe basin under HDRI).
        {
            auto mid = initV;
            if (inv.mapGround) {
                for (int k = 0; k < 12; ++k) mid[static_cast<size_t>(k)] = 0.45;
                mid[12] = 0.45;
                mid[13] = 0.25;
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

        // High-metal / low-rough seed (mirror / spheres friendly).
        {
            auto m = initV;
            if (inv.mapGround) {
                m[12] = 0.12;
                m[13] = 0.85;
            } else {
                m[3] = 0.12;
                m[4] = 0.85;
            }
            candidates.push_back(m);
        }
        // Low-metal / high-rough seed (diffuse floor).
        {
            auto m = initV;
            if (inv.mapGround) {
                m[12] = 0.80;
                m[13] = 0.05;
            } else {
                m[3] = 0.80;
                m[4] = 0.05;
            }
            candidates.push_back(m);
        }

        // Fill remaining with random samples.
        for (int c = static_cast<int>(candidates.size()); c < cfg.multiStart; ++c) {
            candidates.push_back(inv.sampleRandomTheta(cfg.seed + 17u * static_cast<uint32_t>(c + 3)));
        }

        const int nProbe = std::min(cfg.multiStart, static_cast<int>(candidates.size()));
        for (int c = 0; c < nProbe; ++c) {
            // Project into box.
            std::vector<double> th = candidates[static_cast<size_t>(c)];
            if (th.size() != space.size()) continue;
            for (size_t i = 0; i < th.size(); ++i) th[i] = space.project(i, th[i]);
            // Specular targets: evaluate candidates with higher spp + full specular weight.
            const float probeSpp = (highlightScore > 0.16) ? 2.0f : 1.25f;
            const double probeSw =
                (highlightScore > 0.16) ? std::min(1.0, cfg.specularWeight + 0.2)
                                        : cfg.specularWeight * 0.6;
            double Lc = lossAt(th, probeSpp, probeSw);
            // Bonus: when target is specular, prefer high-metal / low-rough seeds.
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
        // If target looks specular but winner is diffuse, reinject conductor seed for metal/rough.
        if (highlightScore > 0.18 && bestStart.size() > metalIdx && bestStart[metalIdx] < 0.40) {
            bestStart[metalIdx] = 0.85;
            bestStart[roughIdx] = 0.12;
            for (size_t i = 0; i < bestStart.size(); ++i)
                bestStart[i] = space.project(i, bestStart[i]);
            bestStartLoss = lossAt(bestStart, 2.0f, std::min(1.0, cfg.specularWeight + 0.2));
            std::cout << "  specular reinject metal/rough → loss=" << bestStartLoss << "\n";
        }
        space.values = bestStart;
        loss = bestStartLoss;
        std::cout << "  multi-start winner loss=" << loss << "  θ=" << formatTheta(space.values)
                  << "\n";
    }

    std::vector<double> bestTheta = space.values;
    double bestLoss = loss;
    int bestIter = 0;

    // Staged FD: joint albedo↔light is ill-conditioned under HDRI.
    auto runStage = [&](const char* name, const std::vector<size_t>& activeIdx, int iters,
                        std::ofstream& traj, bool& firstTraj, double lrMul = 1.0,
                        double epsMul = 1.0, float sppScale = 1.0f, double specularW = -1.0) {
        if (activeIdx.empty() || iters <= 0) return;
        std::cout << "── stage " << name << " (" << activeIdx.size() << " params, " << iters
                  << " iters, lr×" << lrMul;
        if (sppScale > 1.001f) std::cout << ", spp×" << sppScale;
        std::cout << ") ──\n";
        AdamState adam;
        adam.resize(space.size());
        int stageBestIter = 0;
        int stageWorse = 0;
        double stageBest = lossAt(space.values, sppScale, specularW);
        std::vector<double> stageBestTh = space.values;
        const double stageLr = cfg.lr * lrMul;
        const double stageEps = cfg.eps * epsMul;
        for (int it = 0; it < iters; ++it) {
            std::vector<double> g(space.size(), 0.0);
            std::vector<double> theta = space.values;
            for (size_t ai : activeIdx) {
                const double v0 = theta[ai];
                const double span = std::max(1e-3, space.hi[ai] - space.lo[ai]);
                const double epsI = std::max(stageEps, 0.02 * span);
                const double hi = space.project(ai, v0 + epsI);
                const double lo = space.project(ai, v0 - epsI);
                const double denom = hi - lo;
                if (denom < 1e-12) continue;
                theta[ai] = hi;
                const double Lh = lossAt(theta, sppScale, specularW);
                theta[ai] = lo;
                const double Ll = lossAt(theta, sppScale, specularW);
                theta[ai] = v0;
                g[ai] = (Lh - Ll) / denom;
            }
            const std::vector<double> before = space.values;
            if (cfg.useAdam) adam.step(space, g, stageLr);
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
            loss = lossAt(space.values, sppScale, specularW);
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
            if (stageWorse >= 5 && stageBest < 2e-3) {
                std::cout << "  early stop (best @ " << stageBestIter << ")\n";
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
    };

    // B6 schedule: multi-start done → brightness → materials → hi-spp BRDF → polish.
    std::vector<size_t> albedoIdx;
    std::vector<size_t> brdfIdx;
    if (inv.mapGround) {
        for (size_t i = 0; i < 12; ++i) albedoIdx.push_back(i);
        brdfIdx = {12, 13};
    } else {
        albedoIdx = {0, 1, 2};
        brdfIdx = {3, 4};
    }
    std::vector<size_t> pedestalIdx;
    if (inv.fitPedestal && inv.pedestalMat) {
        const size_t base = inv.primaryDims();
        pedestalIdx = {base, base + 1, base + 2};
    }
    std::vector<size_t> lightIdx;
    std::vector<size_t> envIdx;
    std::vector<size_t> exposureIdx;
    size_t cursor = inv.lightBlockStart();
    if (inv.fitKeyLight && inv.keyLight) lightIdx.push_back(cursor++);
    if (inv.fitFillLight && inv.fillLight) lightIdx.push_back(cursor++);
    if (inv.fitRimLight && inv.rimLight) lightIdx.push_back(cursor++);
    if (inv.fitEnvScale) envIdx.push_back(cursor++);
    if (inv.fitExposure) exposureIdx.push_back(cursor);

    // C1: if NN prior already matches well, only soft-polish (full FD walks off good color).
    const bool nnSoftRefine = usedNnPrior && (showInitRmse < 0.08 || loss < 0.08);
    const char* schedName =
        nnSoftRefine ? "nn_soft_refine"
                     : (usedNnPrior ? "nn_seeded_staged"
                                    : "multistart_env_lights_brdfpre_albedo_brdf_pedestal_refine");

    std::ofstream traj(outDir / "trajectory.json");
    traj << "{\n  \"scene\": \"" << cfg.scene << "\",\n  \"quality\": \"" << cfg.quality.name
         << "\",\n  \"views\": " << nViews << ",\n  \"dims\": " << space.size()
         << ",\n  \"schedule\": \"" << schedName << "\",\n"
         << "  \"nn_prior\": " << (usedNnPrior ? "true" : "false")
         << ",\n  \"map_ground\": " << (inv.mapGround ? "true" : "false")
         << ",\n  \"external_target\": " << (externalTarget ? "true" : "false")
         << ",\n  \"iters\": [\n";
    bool firstTraj = true;

    const auto fitStart = std::chrono::steady_clock::now();
    const int envIters = envIdx.empty() ? 0 : std::max(6, (cfg.iters * 15) / 100);
    const int lightIters = lightIdx.empty() ? 0 : std::max(8, (cfg.iters * 22) / 100);
    const int albedoIters = std::max(10, (cfg.iters * 22) / 100);
    const int brdfIters = std::max(10, (cfg.iters * 20) / 100);
    const int pedestalIters = pedestalIdx.empty() ? 0 : std::max(5, (cfg.iters * 12) / 100);
    const int refineIters = std::max(5, (cfg.iters * 12) / 100);
    const int exposureIters = exposureIdx.empty() ? 0 : std::max(4, (cfg.iters * 8) / 100);

    bestTheta = space.values;
    bestLoss = loss;

    if (nnSoftRefine) {
        // Keep NN albedo — only gentle light/BRDF/refine. Prevents color blowout.
        std::cout << "C1 soft refine (init SHOW RMSE=" << showInitRmse << " loss=" << loss
                  << ") — skipping full staged FD\n";
        runStage("lights_soft", lightIdx, std::max(3, lightIters / 3), traj, firstTraj, 0.35, 0.7);
        runStage("brdf_soft", brdfIdx, std::max(4, brdfIters / 3), traj, firstTraj, 0.35, 0.55,
                 cfg.brdfSppMul, cfg.specularWeight * 0.5);
        std::vector<size_t> refineIdx = brdfIdx;
        if (!envIdx.empty()) refineIdx.insert(refineIdx.end(), envIdx.begin(), envIdx.end());
        if (!lightIdx.empty()) refineIdx.push_back(lightIdx.front());
        // Tiny albedo nudge only (preserve NN color)
        refineIdx.insert(refineIdx.end(), albedoIdx.begin(), albedoIdx.end());
        runStage("refine", refineIdx, std::max(4, refineIters), traj, firstTraj, 0.20, 0.45, 1.25f,
                 cfg.specularWeight * 0.4);
    } else {
        // Brightness first, then materials (prevents white-albedo blowout from dark init).
        runStage("env", envIdx, envIters, traj, firstTraj, 1.0, 1.0);
        if (!exposureIdx.empty()) {
            runStage("exposure", exposureIdx, exposureIters, traj, firstTraj, 1.0, 1.0);
        }
        runStage("lights", lightIdx, lightIters, traj, firstTraj, 0.85, 1.0);
        if (highlightScore > 0.24) {
            const int brdfPreIters = std::max(6, brdfIters / 2);
            const float preSpp = std::max(cfg.brdfSppMul, 2.0f);
            const double preSw = std::min(1.0, cfg.specularWeight + 0.30);
            runStage("brdf_pre", brdfIdx, brdfPreIters, traj, firstTraj, 0.80, 0.65, preSpp, preSw);
        }
        // With NN seed, use softer albedo steps so prior color is not destroyed.
        const double albedoLr = usedNnPrior ? 0.45 : 0.8;
        runStage("albedo", albedoIdx, albedoIters, traj, firstTraj, albedoLr, 0.9);
        runStage("brdf", brdfIdx, brdfIters, traj, firstTraj, 0.75, 0.7, cfg.brdfSppMul,
                 std::min(1.0, cfg.specularWeight + 0.1));
        runStage("brdf2", brdfIdx, std::max(4, brdfIters / 2), traj, firstTraj, 0.45, 0.55,
                 cfg.brdfSppMul, std::min(1.0, cfg.specularWeight + 0.15));
        runStage("pedestal", pedestalIdx, pedestalIters, traj, firstTraj, 0.65, 1.0);
        runStage("lights2", lightIdx, std::max(3, lightIters / 2), traj, firstTraj, 0.50, 0.75);
        std::vector<size_t> refineIdx = albedoIdx;
        refineIdx.insert(refineIdx.end(), brdfIdx.begin(), brdfIdx.end());
        if (!envIdx.empty()) refineIdx.insert(refineIdx.end(), envIdx.begin(), envIdx.end());
        if (!lightIdx.empty()) refineIdx.push_back(lightIdx.front());
        if (!exposureIdx.empty())
            refineIdx.insert(refineIdx.end(), exposureIdx.begin(), exposureIdx.end());
        runStage("refine", refineIdx, refineIters, traj, firstTraj, 0.35, 0.55, 1.25f,
                 cfg.specularWeight * 0.7);
    }
    // Always restore global best across stages (MC noise can make a stage "worse").
    space.values = bestTheta;
    loss = bestLoss;
    traj << "\n  ],\n  \"best_loss\": " << bestLoss << "\n}\n";
    traj.close();
    space.values = bestTheta;
    loss = bestLoss;
    const auto fitEnd = std::chrono::steady_clock::now();

    std::cout << "SHOW recovered (all views, best staged θ)...\n";
    applyTheta(space.values);
    ImageRGBA8 recoveredPrimary;
    for (int v = 0; v < nViews; ++v) {
        auto img = session.render(v, cfg.show, cfg.seed, cfg.showDenoise);
        if (inv.fitExposure) img = applyExposure(img, inv.currentExposure);
        savePNG(img, outDir / (std::string("recovered_") + inv.views[static_cast<size_t>(v)].name +
                               ".png"));
        if (v == 0) recoveredPrimary = std::move(img);
    }
    savePNG(recoveredPrimary, outDir / "recovered_show.png");

    // Relight showcase: keep recovered materials, push key hotter (not inverse).
    if (studio && inv.keyLight && !externalTarget) {
        std::cout << "SHOW relight (recovered materials + hot key)...\n";
        const float savedKey = inv.keyLight->getIntensity();
        inv.keyLight->setIntensity(savedKey * 2.5f);
        if (auto fill = inv.scene->findActor("Fill")) {
            if (auto lc = fill->getComponent<LightComponent>())
                lc->setIntensity(lc->getIntensity() * 0.35f);
        }
        (void)renderer.updateRTLightParams();
        const ImageRGBA8 relightRec =
            session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        savePNG(relightRec, outDir / "recovered_relight.png");

        inv.applyTruth();
        inv.keyLight->setIntensity(inv.truthKeyI * 2.5f);
        (void)renderer.updateRTLightParams();
        const ImageRGBA8 relightTruth =
            session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        savePNG(relightTruth, outDir / "truth_relight.png");
    }

    const double paramErr = externalTarget ? 0.0 : space.l2To(truthV);
    const double paramRmse =
        externalTarget ? 0.0 : paramErr / std::sqrt(static_cast<double>(space.size()));
    const double showRmse = rmseRGB(recoveredPrimary, targetsShow[0]);

    std::cout << "\n=== inverse_fit result (B6 multi-param) ===\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << nViews << "  dims=" << space.size()
              << (inv.mapGround ? "  map-ground" : "")
              << (externalTarget ? "  external-target" : "") << "\n";
    if (!externalTarget) {
        std::cout << "  truth     θ = " << formatTheta(truthV) << "\n";
    }
    std::cout << "  recovered θ = " << formatTheta(space.values) << "\n";
    if (!externalTarget && space.size() > metalIdx && truthV.size() > metalIdx) {
        if (!inv.mapGround) {
            std::cout << "  primary |Δ| RGB=(" << std::abs(space.values[0] - truthV[0]) << ","
                      << std::abs(space.values[1] - truthV[1]) << ","
                      << std::abs(space.values[2] - truthV[2]) << ") rough="
                      << std::abs(space.values[roughIdx] - truthV[roughIdx])
                      << " metal=" << std::abs(space.values[metalIdx] - truthV[metalIdx]) << "\n";
        } else {
            std::cout << "  tiles |Δ|albedo L2=";
            double tileL2 = 0.0;
            for (size_t i = 0; i < 12; ++i) {
                const double d = space.values[i] - truthV[i];
                tileL2 += d * d;
            }
            std::cout << std::sqrt(tileL2) << " rough="
                      << std::abs(space.values[roughIdx] - truthV[roughIdx])
                      << " metal=" << std::abs(space.values[metalIdx] - truthV[metalIdx]) << "\n";
        }
    }
    if (inv.fitPedestal && inv.pedestalMat) {
        const size_t pb = inv.primaryDims();
        if (!externalTarget && space.size() >= pb + 3 && truthV.size() >= pb + 3) {
            std::cout << "  pedestal |Δ| RGB=(" << std::abs(space.values[pb] - truthV[pb]) << ","
                      << std::abs(space.values[pb + 1] - truthV[pb + 1]) << ","
                      << std::abs(space.values[pb + 2] - truthV[pb + 2]) << ")\n";
        }
    }
    size_t lo = inv.lightBlockStart();
    double keyErr = 0.0;
    if (inv.fitKeyLight && inv.keyLight && truthV.size() > lo && space.size() > lo) {
        const double tI = truthV[lo] * InverseScene::kKeyIScale;
        const double rI = space.values[lo] * InverseScene::kKeyIScale;
        keyErr = std::abs(rI - tI);
        std::cout << "  key |Δ|I = " << keyErr << "  (truth " << tI << " recovered " << rI
                  << ")\n";
        ++lo;
    }
    if (inv.fitFillLight && inv.fillLight && truthV.size() > lo && space.size() > lo) {
        const double tI = truthV[lo] * InverseScene::kKeyIScale;
        const double rI = space.values[lo] * InverseScene::kKeyIScale;
        std::cout << "  fill |Δ|I = " << std::abs(rI - tI) << "  (truth " << tI << " recovered "
                  << rI << ")\n";
        ++lo;
    }
    if (inv.fitRimLight && inv.rimLight && truthV.size() > lo && space.size() > lo) {
        const double tI = truthV[lo] * InverseScene::kKeyIScale;
        const double rI = space.values[lo] * InverseScene::kKeyIScale;
        std::cout << "  rim |Δ|I = " << std::abs(rI - tI) << "  (truth " << tI << " recovered "
                  << rI << ")\n";
        ++lo;
    }
    if (inv.fitEnvScale && truthV.size() > lo && space.size() > lo) {
        std::cout << "  env |Δ|scale = " << std::abs(space.values[lo] - truthV[lo])
                  << "  (truth " << truthV[lo] << " recovered " << space.values[lo] << ")\n";
        ++lo;
    }
    if (inv.fitExposure && space.size() > lo) {
        std::cout << "  exposure = " << space.values[lo] << "\n";
    }
    if (!externalTarget) {
        std::cout << "  param L2 = " << paramErr << "  param RMSE = " << paramRmse << "\n";
    }
    std::cout << "  final multi-view FIT loss = " << loss << "\n";
    std::cout << "  SHOW RMSE (primary) = " << showRmse << "\n";
    std::cout << "  fit wall time = "
              << std::chrono::duration_cast<std::chrono::seconds>(fitEnd - fitStart).count()
              << " s\n";
    std::cout << "  wrote " << outDir << "/\n";

    // Before/after comparison sheet (target | init | recovered | diff).
    {
        std::filesystem::path cmpScript = "tools/inverse_c1/make_compare.py";
        if (!std::filesystem::exists(cmpScript)) {
            cmpScript = std::filesystem::path("..") / cmpScript;
        }
        if (std::filesystem::exists(cmpScript)) {
            std::ostringstream cmd;
            cmd << cfg.nnPython << " " << cmpScript.string() << " "
                << std::filesystem::absolute(outDir).string();
            const int crc = std::system(cmd.str().c_str());
            if (crc == 0) {
                std::cout << "  compare sheets → " << outDir
                          << "/compare_before_after.png  (+ target_recovered, multiview)\n";
            } else {
                std::cout << "  (compare sheet skipped, rc=" << crc << ")\n";
            }
        }
    }

    // Selftest gates. External photo: image match only.
    // Draft multi-param (12–14D + MC noise): SHOW ~0.15 is grain-limited; high quality is tighter.
    const double kShowRmseTol = (std::string_view(cfg.quality.name) == "draft") ? 0.155 : 0.12;
    constexpr double kKeyITol = 12.0;
    constexpr double kParamRmseSoft = 0.48;
    // Mirror/metal: also gate rough+metal when synthetic (tighter for BRDF gap).
    double roughErr = 0.0, metalErr = 0.0;
    if (!externalTarget && truthV.size() > metalIdx && space.size() > metalIdx) {
        roughErr = std::abs(space.values[roughIdx] - truthV[roughIdx]);
        metalErr = std::abs(space.values[metalIdx] - truthV[metalIdx]);
    }
    // Specular presets get a slightly softer metal gate under draft MC; rough stays tight.
    const double metalTol =
        (cfg.preset == "mirror" || cfg.preset == "spheres") ? 0.35 : 0.55;
    const double roughTol = 0.40;
    const bool brdfOk = externalTarget || (roughErr < roughTol && metalErr < metalTol);
    const bool keyOk = externalTarget || !inv.fitKeyLight || keyErr < kKeyITol;
    const bool showOk = showRmse < kShowRmseTol;
    const bool paramSoftOk = externalTarget || paramRmse < kParamRmseSoft;
    const bool ok = showOk && keyOk && paramSoftOk && brdfOk;
    std::cout << (ok ? "SELFTEST PASS" : "SELFTEST FAIL")
              << " (SHOW RMSE " << showRmse << (showOk ? " < " : " >= ") << kShowRmseTol;
    if (!externalTarget) {
        std::cout << ", key|ΔI| " << keyErr << (keyOk ? " < " : " >= ") << kKeyITol
                  << ", param RMSE " << paramRmse << (paramSoftOk ? " < " : " >= ") << kParamRmseSoft
                  << ", |Δ|rough " << roughErr << " |Δ|metal " << metalErr
                  << (brdfOk ? " ok" : " FAIL");
    }
    std::cout << ")\n";

    inv.scene.reset();
    return ok ? 0 : 1;
}
