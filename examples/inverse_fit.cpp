// inverse_fit — physical inverse renderer (no ML).
//
// Gradual roadmap (no ML yet):
//   A) dual-budget + grain-free SHOW (OIDN)
//   B) product-studio scene + multi-view + relight
//   B1/B2) scalar full PBR: albedo RGB + roughness + metallic
//   B3) multi-surface + key light
//   B4) fill light + 3-stage schedule + dataset-dataset (ML bridge)
//   C) ML priors later (not this binary)
//
// Studio θ (default):
//   ground[5]  = albedo.rgb, roughness, metallic
//   pedestal[3] = albedo.rgb
//   key_I[1], fill_I[1]
// Schedule: primary → pedestal → lights
// --export-dataset N  writes (θ, image) pairs for future ML
// FIT  = raw MC (noise is a feature for FD) — denoise NEVER on
// SHOW = grain-free stills (OIDN default)
//
// Usage:
//   ./inverse_fit --selftest --scene studio --quality draft
//   ./inverse_fit --selftest --scene studio --quality high
//   ./inverse_fit --selftest --scene cornell          # legacy fast box

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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    int iters{35}; // multi-surface + light needs more Adam steps
    double lr{0.055};
    double eps{0.05};
    double maskX{1.0};     // studio: full width
    double maskYMin{0.25}; // floor + pedestal in loss band
    uint32_t seed{1};
    std::string outDir{"renders/inverse"};
    bool useAdam{true};
    DenoiseMode showDenoise{DenoiseMode::OIDN};
    std::string scene{"studio"}; // studio | cornell
    int numViews{3};
    bool fitPedestal{true};  // multi-surface: pedestal albedo
    bool fitKeyLight{true};  // key (+ fill) intensities
    bool fitFillLight{true};
    int exportDataset{0}; // >0: write N (θ, FIT image) pairs then exit (ML data factory)
    // Prefer showcase Lantern when present; fall back to tracked test assets.
    std::string modelPath{"assets/showcase_objects/Lantern.glb"};
    std::string envPath{"assets/hdri/brown_photostudio_02_2k.hdr"};
    std::string relightEnvPath{"assets/hdri/kloofendal_43d_clear_puresky_2k.hdr"};
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
        } else if (s == "--model") {
            a.cfg.modelPath = need("--model");
        } else if (s == "--env") {
            a.cfg.envPath = need("--env");
        } else if (s == "--relight-env") {
            a.cfg.relightEnvPath = need("--relight-env");
        } else if (s == "--views") {
            a.cfg.numViews = std::clamp(std::atoi(need("--views")), 1, 8);
        } else if (s == "--no-pedestal") {
            a.cfg.fitPedestal = false;
        } else if (s == "--no-light") {
            a.cfg.fitKeyLight = false;
            a.cfg.fitFillLight = false;
        } else if (s == "--no-fill") {
            a.cfg.fitFillLight = false;
        } else if (s == "--export-dataset") {
            a.cfg.exportDataset = std::max(1, std::atoi(need("--export-dataset")));
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
                << "  Studio θ: ground PBR[5] + pedestal[3] + key I + fill I\n"
                << "  Stages: primary → pedestal → lights\n"
                << "  --export-dataset N   write N (θ,image) pairs for ML (no optim)\n"
                << "  --no-pedestal --no-light --no-fill\n"
                << "  --quality draft|high|ultra|cinema  --views N\n"
                << "  --show-denoise=oidn|none  --out-dir DIR\n";
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
    // Primary PBR surface (ground / left wall)
    Actor* primaryActor{nullptr};
    MaterialComponent* primaryMat{nullptr};
    // Secondary surface (pedestal top) — albedo only in θ
    Actor* pedestalActor{nullptr};
    MaterialComponent* pedestalMat{nullptr};
    LightComponent* keyLight{nullptr};
    LightComponent* fillLight{nullptr};

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
    // Light intensities stored as scale of kKeyIScale (Adam conditioning).
    static constexpr float kKeyIScale = 40.0f;
    bool fitPedestal{false};
    bool fitKeyLight{false};
    bool fitFillLight{false};

    // Layout: primary[5] | pedestal[3]? | key_scale[1]? | fill_scale[1]?
    [[nodiscard]] size_t thetaDims() const {
        size_t n = 5;
        if (fitPedestal && pedestalMat) n += 3;
        if (fitKeyLight && keyLight) n += 1;
        if (fitFillLight && fillLight) n += 1;
        return n;
    }

    [[nodiscard]] bool needsLightUpdate() const {
        return (fitKeyLight && keyLight) || (fitFillLight && fillLight);
    }

    [[nodiscard]] std::vector<double> truthTheta() const {
        std::vector<double> t = truthPrimary.asTheta();
        if (fitPedestal && pedestalMat) {
            t.push_back(truthPedestal.albedo.r);
            t.push_back(truthPedestal.albedo.g);
            t.push_back(truthPedestal.albedo.b);
        }
        if (fitKeyLight && keyLight) t.push_back(truthKeyI / kKeyIScale);
        if (fitFillLight && fillLight) t.push_back(truthFillI / kKeyIScale);
        return t;
    }

    [[nodiscard]] std::vector<double> initTheta() const {
        std::vector<double> t = initPrimary.asTheta();
        if (fitPedestal && pedestalMat) {
            t.push_back(initPedestal.albedo.r);
            t.push_back(initPedestal.albedo.g);
            t.push_back(initPedestal.albedo.b);
        }
        if (fitKeyLight && keyLight) t.push_back(initKeyI / kKeyIScale);
        if (fitFillLight && fillLight) t.push_back(initFillI / kKeyIScale);
        return t;
    }

    /// Random θ in box bounds (for dataset export / ML).
    [[nodiscard]] std::vector<double> sampleRandomTheta(uint32_t rng) const {
        auto u01 = [&]() -> double {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<double>(rng >> 8) / static_cast<double>(1u << 24);
        };
        std::vector<double> t(5);
        for (int i = 0; i < 3; ++i) t[static_cast<size_t>(i)] = u01();
        t[3] = 0.04 + 0.96 * u01(); // roughness
        t[4] = u01();               // metallic
        if (fitPedestal && pedestalMat) {
            t.push_back(u01());
            t.push_back(u01());
            t.push_back(u01());
        }
        if (fitKeyLight && keyLight) t.push_back(0.05 + 1.2 * u01());
        if (fitFillLight && fillLight) t.push_back(0.05 + 0.8 * u01());
        return t;
    }

    void applyTheta(const std::vector<double>& th) {
        if (th.size() < 5) return;
        writeMatPbr(primaryMat, primaryActor,
                    PbrParams{.albedo = {static_cast<float>(th[0]), static_cast<float>(th[1]),
                                         static_cast<float>(th[2])},
                              .roughness = static_cast<float>(th[3]),
                              .metallic = static_cast<float>(th[4])});
        size_t i = 5;
        if (fitPedestal && pedestalMat && th.size() >= i + 3) {
            PbrParams pp = truthPedestal; // keep rough/metal of pedestal fixed
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
        s.fitFillLight = false; // cornell has single key
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
        // Glossy warm floor: mid roughness + slight metal so all 5 PBR dims matter.
        s.truthPrimary = {.albedo = {0.72f, 0.55f, 0.42f}, .roughness = 0.30f, .metallic = 0.12f};
        s.initPrimary = {.albedo = {0.20f, 0.45f, 0.70f}, .roughness = 0.88f, .metallic = 0.70f};
        s.truthPedestal = {.albedo = {0.18f, 0.19f, 0.21f}, .roughness = 0.40f, .metallic = 0.05f};
        s.initPedestal = {.albedo = {0.55f, 0.12f, 0.10f}, .roughness = 0.40f, .metallic = 0.05f};
        s.truthKeyI = 22.0f;
        s.initKeyI = 7.0f;
        s.truthFillI = 9.0f;
        s.initFillI = 3.0f;

        const float groundY = 0.0f;
        const float pedestalH = 0.35f;
        const float pedestalHalf = 1.35f;

        // ── Optimizable ground (large cyclorama floor) ─────────────────
        {
            const float half = 14.0f;
            auto ground = s.scene->createActor("Ground");
            auto gm = std::make_shared<Model>();
            addQuad(gm->vertices, gm->indices,
                    {-half, groundY, -half}, {half, groundY, -half},
                    {half, groundY, half}, {-half, groundY, half},
                    {0, 1, 0}, s.truthPrimary.albedo);
            auto gmesh = ground->addComponent<MeshComponent>();
            gmesh->setModel(gm);
            gmesh->setVisible(true);
            auto gmat = ground->addComponent<MaterialComponent>();
            gmat->getMaterial().baseColor = s.truthPrimary.albedo;
            gmat->getMaterial().roughness = s.truthPrimary.roughness;
            gmat->getMaterial().metallic = s.truthPrimary.metallic;
            s.primaryActor = ground.get();
            s.primaryMat = gmat.get();
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
        // Bake model so local origin is bottom-center (Y-up glTF), then scale
        // and sit on the pedestal. Avoids float-above-plinth from bad bmin math.
        auto model = ModelLoader::load(cfg.modelPath);
        if (model && !model->vertices.empty()) {
            glm::vec3 bmin(1e30f), bmax(-1e30f);
            for (const auto& v : model->vertices) {
                bmin = glm::min(bmin, v.position);
                bmax = glm::max(bmax, v.position);
            }
            const glm::vec3 extent = bmax - bmin;
            const glm::vec3 centerXZ{(bmin.x + bmax.x) * 0.5f, 0.0f, (bmin.z + bmax.z) * 0.5f};
            // Shift vertices: bottom on y=0, centered in XZ.
            for (auto& v : model->vertices) {
                v.position.x -= centerXZ.x;
                v.position.y -= bmin.y;
                v.position.z -= centerXZ.z;
            }
            const float maxExtent = std::max({extent.x, extent.y, extent.z});
            // Fit hero height to ~2.0 world units (product shot).
            const float scale = 2.0f / std::max(extent.y > 0.01f ? extent.y : maxExtent, 0.001f);

            auto actor = s.scene->createActor("Hero");
            actor->getTransform()->setRotation(
                glm::quat(glm::radians(glm::vec3(0.0f, 22.0f, 0.0f))));
            actor->getTransform()->setScale(glm::vec3(scale));
            // Local bottom is y=0 → world bottom = pos.y (yaw keeps Y).
            actor->getTransform()->setPosition({0.0f, groundY + pedestalH + 0.002f, 0.0f});

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

        // Soft key + fill + rim (works with or without env for relight demos)
        auto addLight = [&](const char* name, glm::vec3 pos, glm::vec3 col, float I, float r) {
            auto a = s.scene->createActor(name);
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor(col);
            lc->setIntensity(I);
            lc->setRadius(r);
            a->getTransform()->setPosition(pos);
        };
        {
            auto a = s.scene->createActor("Key");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({1.0f, 0.97f, 0.92f});
            lc->setIntensity(s.truthKeyI);
            lc->setRadius(1.0f);
            a->getTransform()->setPosition({4.0f, 5.0f, 4.5f});
            s.keyLight = lc.get();
        }
        {
            auto a = s.scene->createActor("Fill");
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor({0.75f, 0.85f, 1.0f});
            lc->setIntensity(s.truthFillI);
            lc->setRadius(1.4f);
            a->getTransform()->setPosition({-3.5f, 3.0f, 3.5f});
            s.fillLight = lc.get();
        }
        addLight("Rim", {-0.5f, 3.5f, -4.5f}, {1.0f, 0.95f, 0.9f}, 10.0f, 0.7f);

        // Multi-view product orbit — pulled back so full hero + pedestal are in frame.
        s.views = {
            {"front", {0.0f, 1.35f, 7.2f}, -8.0f, -90.0f},
            {"three_quarter", {5.0f, 1.55f, 5.6f}, -10.0f, -48.0f},
            {"opposite", {-4.8f, 1.35f, 5.4f}, -8.0f, -128.0f},
        };
        return s;
    }
};

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
        if (!bound) {
            renderer.setScene(inv.scene.get());
            bound = true;
        } else {
            // Material + light edits only — never rebuild BLAS / reload env HDR.
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

    std::cout << "OHAO inverse_fit — multi-surface + multi-light IR (no ML)\n";
    std::cout << "  θ dims=" << inv.thetaDims() << "  primary PBR[5]";
    if (inv.fitPedestal && inv.pedestalMat) std::cout << " + pedestal albedo[3]";
    if (inv.fitKeyLight && inv.keyLight) std::cout << " + key I";
    if (inv.fitFillLight && inv.fillLight) std::cout << " + fill I";
    std::cout << "\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << cfg.numViews
              << "  quality=" << cfg.quality.name << "\n";
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

    const int nViews = std::min(cfg.numViews, static_cast<int>(inv.views.size()));
    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.setRenderSeed(cfg.seed);
    if (studio) applyEnv(renderer, inv.envPath);

    RenderSession session{renderer, inv, false};

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir);

    // ── Optional: ML data factory (θ → FIT image pairs) ───────────────
    if (cfg.exportDataset > 0) {
        const auto dataDir = outDir / "dataset";
        std::filesystem::create_directories(dataDir);
        std::ofstream meta(dataDir / "meta.jsonl");
        meta << "{\"type\":\"header\",\"dims\":" << inv.thetaDims()
             << ",\"fit\":[" << cfg.fit.width << "," << cfg.fit.height << "," << cfg.fit.spp
             << "],\"scene\":\"" << cfg.scene << "\"}\n";
        std::cout << "Exporting " << cfg.exportDataset << " (θ, image) pairs to " << dataDir
                  << " ...\n";
        for (int i = 0; i < cfg.exportDataset; ++i) {
            const uint32_t rng = cfg.seed + static_cast<uint32_t>(i) * 2654435761u;
            const auto th = inv.sampleRandomTheta(rng);
            inv.applyTheta(th);
            const ImageRGBA8 img =
                session.render(0, cfg.fit, cfg.seed + static_cast<uint32_t>(i), DenoiseMode::None);
            char name[32];
            std::snprintf(name, sizeof(name), "%05d.png", i);
            const auto path = dataDir / name;
            savePNG(img, path);
            meta << "{\"i\":" << i << ",\"file\":\"" << name << "\",\"theta\":[";
            for (size_t k = 0; k < th.size(); ++k) {
                if (k) meta << ",";
                meta << th[k];
            }
            meta << "]}\n";
            if ((i + 1) % 10 == 0 || i + 1 == cfg.exportDataset) {
                std::cout << "  " << (i + 1) << "/" << cfg.exportDataset << "\n";
            }
        }
        meta.close();
        std::cout << "Dataset export done. (physical renderer = ML data factory)\n";
        inv.scene.reset();
        return 0;
    }

    // ── Truth multi-view targets ──────────────────────────────────────
    inv.applyTruth();
    std::vector<ImageRGBA8> targetsFit(static_cast<size_t>(nViews));
    std::vector<ImageRGBA8> targetsShow(static_cast<size_t>(nViews));

    const auto truthV = inv.truthTheta();
    std::cout << "Rendering multi-view TARGETS (truth θ=" << formatTheta(truthV) << ")...\n";
    auto t0 = std::chrono::steady_clock::now();
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
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  targets done in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms\n";

    // Build ParamSpace from init θ
    ParamSpace space;
    const auto initV = inv.initTheta();
    space.add("primary.R", initV[0], 0.0, 1.0);
    space.add("primary.G", initV[1], 0.0, 1.0);
    space.add("primary.B", initV[2], 0.0, 1.0);
    space.add("primary.rough", initV[3], 0.04, 1.0);
    space.add("primary.metal", initV[4], 0.0, 1.0);
    size_t off = 5;
    if (inv.fitPedestal && inv.pedestalMat) {
        space.add("pedestal.R", initV[off], 0.0, 1.0);
        space.add("pedestal.G", initV[off + 1], 0.0, 1.0);
        space.add("pedestal.B", initV[off + 2], 0.0, 1.0);
        off += 3;
    }
    if (inv.fitKeyLight && inv.keyLight) {
        space.add("key.I_scale", initV[off], 0.05, 1.5);
        ++off;
    }
    if (inv.fitFillLight && inv.fillLight) {
        space.add("fill.I_scale", initV[off], 0.05, 1.2);
    }

    auto applyTheta = [&](const std::vector<double>& th) { inv.applyTheta(th); };

    // FD loss on FIT budget (small res). Multi-view average when sizes match.
    auto lossAt = [&](const std::vector<double>& th) -> double {
        applyTheta(th);
        double L = 0.0;
        int used = 0;
        for (int v = 0; v < nViews; ++v) {
            const ImageRGBA8 img =
                session.render(v, cfg.fit, cfg.seed, DenoiseMode::None);
            if (img.empty() || targetsFit[static_cast<size_t>(v)].empty()) continue;
            const double m =
                mseRGB(img, targetsFit[static_cast<size_t>(v)], cfg.maskX, cfg.maskYMin);
            if (std::isfinite(m)) {
                L += m;
                ++used;
            }
        }
        return used > 0 ? L / static_cast<double>(used) : 1e6;
    };

    std::cout << "SHOW init (wrong multi-param guess)...\n";
    applyTheta(space.values);
    const ImageRGBA8 initShow =
        session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
    savePNG(initShow, outDir / "init_show.png");

    double loss = lossAt(space.values);
    const double showInitRmse = rmseRGB(initShow, targetsShow[0]);
    std::cout << "  init multi-view FIT loss=" << loss << "  SHOW RMSE vs target=" << showInitRmse
              << "\n  θ=" << formatTheta(space.values) << "\n";

    std::vector<double> bestTheta = space.values;
    double bestLoss = loss;
    int bestIter = 0;
    int worseStreak = 0;

    // Staged FD: joint albedo↔light is ill-conditioned under HDRI.
    // Stage A — materials (primary ± pedestal), key held at current value.
    // Stage B — key intensity only, materials held.
    auto runStage = [&](const char* name, const std::vector<size_t>& activeIdx, int iters,
                        std::ofstream& traj, bool& firstTraj) {
        if (activeIdx.empty() || iters <= 0) return;
        std::cout << "── stage " << name << " (" << activeIdx.size() << " params, " << iters
                  << " iters) ──\n";
        AdamState adam;
        adam.resize(space.size());
        int stageBestIter = 0;
        int stageWorse = 0;
        double stageBest = lossAt(space.values);
        std::vector<double> stageBestTh = space.values;
        for (int it = 0; it < iters; ++it) {
            // Masked FD: only perturb active indices.
            std::vector<double> g(space.size(), 0.0);
            std::vector<double> theta = space.values;
            for (size_t ai : activeIdx) {
                const double v0 = theta[ai];
                const double hi = space.project(ai, v0 + cfg.eps);
                const double lo = space.project(ai, v0 - cfg.eps);
                const double denom = hi - lo;
                if (denom < 1e-12) continue;
                theta[ai] = hi;
                const double Lh = lossAt(theta);
                theta[ai] = lo;
                const double Ll = lossAt(theta);
                theta[ai] = v0;
                g[ai] = (Lh - Ll) / denom;
            }
            const std::vector<double> before = space.values;
            if (cfg.useAdam) adam.step(space, g, cfg.lr);
            else {
                for (size_t ai : activeIdx) {
                    space.values[ai] =
                        space.project(ai, space.values[ai] - cfg.lr * 50.0 * g[ai]);
                }
            }
            // Freeze inactive dims (Adam can nudge zero-grad slots via bias correction).
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
            loss = lossAt(space.values);
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

    // 3-stage schedule: primary PBR → pedestal → lights (key+fill)
    std::vector<size_t> primaryIdx = {0, 1, 2, 3, 4};
    std::vector<size_t> pedestalIdx;
    if (inv.fitPedestal && inv.pedestalMat) {
        pedestalIdx = {5, 6, 7};
    }
    std::vector<size_t> lightIdx;
    size_t lightBase = 5 + (pedestalIdx.empty() ? 0 : 3);
    if (inv.fitKeyLight && inv.keyLight) {
        lightIdx.push_back(lightBase++);
    }
    if (inv.fitFillLight && inv.fillLight) {
        lightIdx.push_back(lightBase);
    }

    std::ofstream traj(outDir / "trajectory.json");
    traj << "{\n  \"scene\": \"" << cfg.scene << "\",\n  \"quality\": \"" << cfg.quality.name
         << "\",\n  \"views\": " << nViews << ",\n  \"dims\": " << space.size()
         << ",\n  \"fit_pedestal\": " << (inv.fitPedestal ? "true" : "false")
         << ",\n  \"fit_key\": " << (inv.fitKeyLight ? "true" : "false")
         << ",\n  \"fit_fill\": " << (inv.fitFillLight ? "true" : "false")
         << ",\n  \"schedule\": \"primary_then_pedestal_then_lights\",\n  \"iters\": [\n";
    bool firstTraj = true;

    const auto fitStart = std::chrono::steady_clock::now();
    const bool anyLight = !lightIdx.empty();
    const int primaryIters =
        anyLight || !pedestalIdx.empty() ? std::max(10, (cfg.iters * 5) / 10) : cfg.iters;
    const int pedestalIters =
        pedestalIdx.empty() ? 0 : std::max(4, (cfg.iters * 2) / 10);
    const int lightIters = anyLight ? std::max(4, (cfg.iters * 3) / 10) : 0;

    runStage("primary", primaryIdx, primaryIters, traj, firstTraj);
    bestTheta = space.values;
    bestLoss = loss;
    runStage("pedestal", pedestalIdx, pedestalIters, traj, firstTraj);
    bestTheta = space.values;
    bestLoss = loss;
    runStage("lights", lightIdx, lightIters, traj, firstTraj);
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
        savePNG(img, outDir / (std::string("recovered_") + inv.views[static_cast<size_t>(v)].name +
                               ".png"));
        if (v == 0) recoveredPrimary = std::move(img);
    }
    savePNG(recoveredPrimary, outDir / "recovered_show.png");

    // Relight showcase: keep recovered materials, push key hotter (not inverse).
    if (studio && inv.keyLight) {
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

    const double paramErr = space.l2To(truthV);
    const double paramRmse = paramErr / std::sqrt(static_cast<double>(space.size()));
    const double showRmse = rmseRGB(recoveredPrimary, targetsShow[0]);

    std::cout << "\n=== inverse_fit result (multi-surface + light) ===\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << nViews << "  dims=" << space.size()
              << "\n";
    std::cout << "  truth     θ = " << formatTheta(truthV) << "\n";
    std::cout << "  recovered θ = " << formatTheta(space.values) << "\n";
    std::cout << "  primary |Δ| RGB=(" << std::abs(space.values[0] - truthV[0]) << ","
              << std::abs(space.values[1] - truthV[1]) << ","
              << std::abs(space.values[2] - truthV[2]) << ") rough="
              << std::abs(space.values[3] - truthV[3])
              << " metal=" << std::abs(space.values[4] - truthV[4]) << "\n";
    if (inv.fitPedestal && inv.pedestalMat && space.size() >= 8) {
        std::cout << "  pedestal |Δ| RGB=(" << std::abs(space.values[5] - truthV[5]) << ","
                  << std::abs(space.values[6] - truthV[6]) << ","
                  << std::abs(space.values[7] - truthV[7]) << ")\n";
    }
    const size_t lightOff = 5 + ((inv.fitPedestal && inv.pedestalMat) ? 3 : 0);
    double keyErr = 0.0;
    double fillErr = 0.0;
    if (inv.fitKeyLight && inv.keyLight && truthV.size() > lightOff) {
        const double tI = truthV[lightOff] * InverseScene::kKeyIScale;
        const double rI = space.values[lightOff] * InverseScene::kKeyIScale;
        keyErr = std::abs(rI - tI);
        std::cout << "  key |Δ|I = " << keyErr << "  (truth " << tI << " recovered " << rI
                  << ")\n";
    }
    if (inv.fitFillLight && inv.fillLight && truthV.size() > lightOff + 1) {
        const double tI = truthV[lightOff + 1] * InverseScene::kKeyIScale;
        const double rI = space.values[lightOff + 1] * InverseScene::kKeyIScale;
        fillErr = std::abs(rI - tI);
        std::cout << "  fill |Δ|I = " << fillErr << "  (truth " << tI << " recovered " << rI
                  << ")\n";
    }
    (void)fillErr;
    std::cout << "  param L2 = " << paramErr << "  param RMSE = " << paramRmse << "\n";
    std::cout << "  final multi-view FIT loss = " << loss << "\n";
    std::cout << "  SHOW RMSE (primary) = " << showRmse << "\n";
    std::cout << "  fit wall time = "
              << std::chrono::duration_cast<std::chrono::seconds>(fitEnd - fitStart).count()
              << " s\n";
    std::cout << "  wrote " << outDir << "/\n";

    // Composite selftest: image match + key recovery (fill is softer).
    constexpr double kShowRmseTol = 0.12;
    constexpr double kKeyITol = 8.0;
    constexpr double kParamRmseSoft = 0.50;
    const bool keyOk = !inv.fitKeyLight || keyErr < kKeyITol;
    const bool showOk = showRmse < kShowRmseTol;
    const bool paramSoftOk = paramRmse < kParamRmseSoft;
    const bool ok = showOk && keyOk && paramSoftOk;
    std::cout << (ok ? "SELFTEST PASS" : "SELFTEST FAIL")
              << " (SHOW RMSE " << showRmse << (showOk ? " < " : " >= ") << kShowRmseTol
              << ", key|ΔI| " << keyErr << (keyOk ? " < " : " >= ") << kKeyITol
              << ", param RMSE " << paramRmse << (paramSoftOk ? " < " : " >= ") << kParamRmseSoft
              << ")\n";

    inv.scene.reset();
    return ok ? 0 : 1;
}
