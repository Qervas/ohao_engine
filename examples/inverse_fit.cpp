// inverse_fit — physical inverse renderer (no ML).
//
// Gradual roadmap (no ML yet):
//   A) dual-budget + grain-free SHOW (OIDN)
//   B) better scene (studio helmet + HDRI), multi-view loss, relight showcase
//
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
    int iters{25};
    double lr{0.08};
    double eps{0.05};
    double maskX{1.0};     // studio: full width
    double maskYMin{0.45}; // studio: bottom ground band (image-space)
    uint32_t seed{1};
    std::string outDir{"renders/inverse"};
    bool useAdam{true};
    DenoiseMode showDenoise{DenoiseMode::OIDN};
    std::string scene{"studio"}; // studio | cornell
    int numViews{3};
    std::string modelPath{"assets/test_models/DamagedHelmet.glb"};
    std::string envPath{"assets/hdri/brown_photostudio_02_2k.hdr"};
    std::string relightEnvPath{"assets/hdri/kloofendal_43d_clear_puresky_2k.hdr"};
};

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
                << "  --quality draft|high|ultra|cinema\n"
                << "  --model PATH --env PATH --relight-env PATH --views N\n"
                << "  --show-denoise=oidn|none  (FIT always raw MC)\n"
                << "  --fit-* / --show-* / --iters --lr --seed --out-dir\n";
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
             glm::vec3 normal, glm::vec3 color) {
    auto actor = scene->createActor(name);
    auto model = std::make_shared<Model>();
    addQuad(model->vertices, model->indices, a, b, c, d, normal, color);
    auto mesh = actor->addComponent<MeshComponent>();
    mesh->setModel(model);
    mesh->setVisible(true);
    auto mat = actor->addComponent<MaterialComponent>();
    mat->getMaterial().baseColor = color;
    mat->getMaterial().roughness = 0.95f;
}

// Optimizable surface: ground plane (studio) or left wall (cornell).
struct InverseScene {
    std::unique_ptr<Scene> scene;
    Actor* targetActor{nullptr};
    MaterialComponent* targetMat{nullptr};
    std::string envPath;
    std::string relightEnvPath;
    std::vector<CameraView> views;
    glm::vec3 truthAlbedo{0.65f, 0.12f, 0.08f}; // warm studio floor (not the old box red)
    glm::vec3 initGuess{0.15f, 0.35f, 0.75f};   // cool blue wrong guess

    void setTargetAlbedo(glm::vec3 rgb) {
        if (!targetMat) return;
        targetMat->getMaterial().baseColor = rgb;
        if (targetActor) {
            if (auto mesh = targetActor->getComponent<MeshComponent>()) {
                if (auto model = mesh->getModel()) {
                    for (auto& v : model->vertices) v.color = rgb;
                }
            }
        }
    }

    void applyCamera(Camera& cam, int viewIndex) const {
        const CameraView& v = views[static_cast<size_t>(viewIndex) % views.size()];
        cam.setPosition(v.position);
        cam.setRotation(v.pitchDeg, v.yawDeg);
        cam.setFov(40.0f);
    }

    static InverseScene buildCornell() {
        InverseScene s;
        s.scene = std::make_unique<Scene>("Inverse Cornell");
        s.truthAlbedo = {0.65f, 0.05f, 0.05f};
        s.initGuess = {0.2f, 0.5f, 0.7f};
        const float S = 5.0f;
        const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
        glm::vec3 LBB(-S, -S, -S), RBB(S, -S, -S), LTB(-S, S, -S), RTB(S, S, -S);
        glm::vec3 LBF(-S, -S, S), RBF(S, -S, S), LTF(-S, S, S), RTF(S, S, S);
        addWall(s.scene.get(), "Back", LBB, RBB, RTB, LTB, {0, 0, 1}, white);
        addWall(s.scene.get(), "Left", LBB, LTB, LTF, LBF, {1, 0, 0}, red);
        addWall(s.scene.get(), "Right", RBB, RBF, RTF, RTB, {-1, 0, 0}, green);
        addWall(s.scene.get(), "Floor", LBB, LBF, RBF, RBB, {0, 1, 0}, white);
        addWall(s.scene.get(), "Ceiling", LTB, RTB, RTF, LTF, {0, -1, 0}, white);

        auto sphere = s.scene->createActorWithComponents("Sphere", PrimitiveType::Sphere);
        sphere->getTransform()->setPosition({0.0f, -S + 1.5f, 0.0f});
        sphere->getTransform()->setScale(glm::vec3(1.5f));
        auto sm = sphere->getComponent<MaterialComponent>();
        sm->getMaterial().baseColor = {0.8f, 0.8f, 0.8f};
        sm->getMaterial().roughness = 0.4f;

        auto light = s.scene->createActor("KeyLight");
        auto lc = light->addComponent<LightComponent>();
        lc->setLightType(LightType::Sphere);
        lc->setColor({1.0f, 0.98f, 0.95f});
        lc->setIntensity(40.0f);
        lc->setRadius(0.6f);
        light->getTransform()->setPosition({0.0f, S - 0.5f, 0.0f});

        s.scene->forEachActor([&](Actor& a) {
            if (a.getName() == "Left") {
                s.targetActor = &a;
                s.targetMat = a.getComponent<MaterialComponent>().get();
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
        s.scene = std::make_unique<Scene>("Inverse Studio");
        s.envPath = cfg.envPath;
        s.relightEnvPath = cfg.relightEnvPath;
        s.truthAlbedo = {0.72f, 0.55f, 0.42f}; // warm concrete / clay floor
        s.initGuess = {0.20f, 0.45f, 0.70f};   // cool wrong floor

        // Load hero model
        auto model = ModelLoader::load(cfg.modelPath);
        float groundY = -2.0f;
        if (model && !model->vertices.empty()) {
            glm::vec3 bmin(1e30f), bmax(-1e30f);
            for (const auto& v : model->vertices) {
                bmin = glm::min(bmin, v.position);
                bmax = glm::max(bmax, v.position);
            }
            const glm::vec3 extent = bmax - bmin;
            const glm::vec3 center = (bmin + bmax) * 0.5f;
            const float maxExtent = std::max({extent.x, extent.y, extent.z});
            const float scale = 3.2f / std::max(maxExtent, 0.001f);
            const bool isYUp = (extent.y >= extent.z);

            auto actor = s.scene->createActor("Hero");
            if (!isYUp) {
                const bool posZIsUp = (bmax.z > 0.01f);
                actor->getTransform()->setRotation(
                    glm::quat(glm::radians(glm::vec3(posZIsUp ? -90.0f : 90.0f, 0, 0))));
            }
            actor->getTransform()->setScale(glm::vec3(scale));
            actor->getTransform()->setPosition(
                {-center.x * scale, -center.y * scale, -center.z * scale});
            auto mesh = actor->addComponent<MeshComponent>();
            mesh->setModel(model);
            mesh->setVisible(true);
            auto mat = actor->addComponent<MaterialComponent>();
            mat->getMaterial().roughness = 0.45f;
            groundY = -3.2f * 0.5f - 0.002f;
            std::cout << "  loaded hero: " << cfg.modelPath << " (" << model->vertices.size()
                      << " verts)\n";
        } else {
            std::cerr << "  WARN: could not load " << cfg.modelPath
                      << " — studio ground-only scene\n";
        }

        // Large studio ground (optimizable)
        {
            const float half = 12.0f;
            auto ground = s.scene->createActor("Ground");
            auto gm = std::make_shared<Model>();
            addQuad(gm->vertices, gm->indices,
                    {-half, groundY, -half}, {half, groundY, -half},
                    {half, groundY, half}, {-half, groundY, half},
                    {0, 1, 0}, s.truthAlbedo);
            auto gmesh = ground->addComponent<MeshComponent>();
            gmesh->setModel(gm);
            gmesh->setVisible(true);
            auto gmat = ground->addComponent<MaterialComponent>();
            gmat->getMaterial().baseColor = s.truthAlbedo;
            gmat->getMaterial().roughness = 0.65f;
            gmat->getMaterial().metallic = 0.0f;
            s.targetActor = ground.get();
            s.targetMat = gmat.get();
        }

        // Soft key + fill (studio) so indoor relight still works without env
        auto addLight = [&](const char* name, glm::vec3 pos, glm::vec3 col, float I, float r) {
            auto a = s.scene->createActor(name);
            auto lc = a->addComponent<LightComponent>();
            lc->setLightType(LightType::Sphere);
            lc->setColor(col);
            lc->setIntensity(I);
            lc->setRadius(r);
            a->getTransform()->setPosition(pos);
        };
        addLight("Key", {3.5f, 4.0f, 4.0f}, {1.0f, 0.98f, 0.95f}, 18.0f, 0.8f);
        addLight("Fill", {-3.0f, 2.5f, 3.0f}, {0.85f, 0.90f, 1.0f}, 8.0f, 1.2f);
        addLight("Rim", {0.0f, 3.0f, -4.0f}, {1.0f, 1.0f, 1.0f}, 6.0f, 0.6f);

        // Multi-view orbit around origin (hero + ground)
        s.views = {
            {"front", {0.0f, 1.6f, 7.5f}, -8.0f, -90.0f},
            {"three_quarter", {5.2f, 1.8f, 5.8f}, -10.0f, -50.0f},
            {"opposite", {-5.0f, 1.5f, 5.5f}, -8.0f, -130.0f},
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
        if ((renderer.getWidth() != budget.width || renderer.getHeight() != budget.height) &&
            budget.width > 0 && budget.height > 0) {
            // Skip resize when only spp differs at same dims (common path).
            if (renderer.getWidth() != budget.width ||
                renderer.getHeight() != budget.height) {
                renderer.resize(budget.width, budget.height);
            }
        }
        inv.applyCamera(renderer.getCamera(), viewIndex);
        renderer.setDenoiseMode(denoise);
        renderer.setRenderSeed(seed + static_cast<uint32_t>(viewIndex) * 9973u);
        if (!bound) {
            renderer.setScene(inv.scene.get());
            bound = true;
        } else {
            // Material edits only — still rebuilds RT mats, but not env HDR from disk.
            (void)renderer.updateSceneBuffers();
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

    const bool studio = (cfg.scene != "cornell");
    if (studio && cfg.maskYMin <= 0.0) cfg.maskYMin = 0.45;
    // Heavy studio (textured GLB + HDRI): keep ONE resolution. Dual-res resize
    // was OOM/segfaulting under multi-view FD. Budget differs by spp only.
    if (studio) {
        cfg.fit.width = cfg.show.width;
        cfg.fit.height = cfg.show.height;
        // Keep FIT spp from quality preset (cheaper than SHOW spp).
    }

    std::cout << "OHAO inverse_fit — multi-view physical IR (no ML)\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << cfg.numViews
              << "  quality=" << cfg.quality.name << "\n";
    std::cout << "  FIT " << cfg.fit.width << "x" << cfg.fit.height << " @" << cfg.fit.spp
              << "  SHOW " << cfg.show.width << "x" << cfg.show.height << " @" << cfg.show.spp
              << "  denoise SHOW=" << denoiseModeName(cfg.showDenoise)
              << " FIT=none\n";

    VulkanRenderer renderer(cfg.show.width, cfg.show.height);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    InverseScene inv =
        studio ? InverseScene::buildStudio(cfg) : InverseScene::buildCornell();
    if (!inv.targetMat) {
        std::cerr << "FATAL: optimizable material missing\n";
        return 1;
    }

    const int nViews = std::min(cfg.numViews, static_cast<int>(inv.views.size()));
    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.setRenderSeed(cfg.seed);
    if (studio) applyEnv(renderer, inv.envPath);

    RenderSession session{renderer, inv, false};

    const auto outDir = std::filesystem::path(cfg.outDir);
    std::filesystem::create_directories(outDir);

    // ── Truth multi-view targets ──────────────────────────────────────
    inv.setTargetAlbedo(inv.truthAlbedo);
    std::vector<ImageRGBA8> targetsFit(static_cast<size_t>(nViews));
    std::vector<ImageRGBA8> targetsShow(static_cast<size_t>(nViews));

    std::cout << "Rendering multi-view TARGETS (truth albedo "
              << inv.truthAlbedo.r << "," << inv.truthAlbedo.g << "," << inv.truthAlbedo.b
              << ")...\n";
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

    ParamSpace space;
    space.add("ground.R", inv.initGuess.r, 0.0, 1.0);
    space.add("ground.G", inv.initGuess.g, 0.0, 1.0);
    space.add("ground.B", inv.initGuess.b, 0.0, 1.0);

    auto applyTheta = [&](const std::vector<double>& th) {
        inv.setTargetAlbedo({static_cast<float>(th[0]), static_cast<float>(th[1]),
                             static_cast<float>(th[2])});
    };

    auto lossAt = [&](const std::vector<double>& th) -> double {
        applyTheta(th);
        double L = 0.0;
        for (int v = 0; v < nViews; ++v) {
            const ImageRGBA8 img =
                session.render(v, cfg.fit, cfg.seed, DenoiseMode::None);
            L += mseRGB(img, targetsFit[static_cast<size_t>(v)], cfg.maskX, cfg.maskYMin);
        }
        return L / static_cast<double>(nViews);
    };

    // ── Init SHOW (wrong guess), primary view ─────────────────────────
    std::cout << "SHOW init (wrong guess)...\n";
    applyTheta(space.values);
    const ImageRGBA8 initShow =
        session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
    savePNG(initShow, outDir / "init_show.png");

    double loss = lossAt(space.values);
    std::cout << "  init multi-view FIT loss=" << loss << "  θ=["
              << space.values[0] << "," << space.values[1] << "," << space.values[2] << "]\n";

    AdamState adam;
    adam.resize(space.size());
    std::ofstream traj(outDir / "trajectory.json");
    traj << "{\n  \"scene\": \"" << cfg.scene << "\",\n  \"quality\": \"" << cfg.quality.name
         << "\",\n  \"views\": " << nViews << ",\n  \"iters\": [\n";

    const auto fitStart = std::chrono::steady_clock::now();
    for (int it = 0; it < cfg.iters; ++it) {
        const auto g = finiteDiffGradient(space, cfg.eps, lossAt);
        if (cfg.useAdam) adam.step(space, g, cfg.lr);
        else gdStep(space, g, cfg.lr * 50.0);
        loss = lossAt(space.values);

        std::cout << "  iter " << (it + 1) << "/" << cfg.iters << "  loss=" << loss
                  << "  θ=[" << space.values[0] << "," << space.values[1] << ","
                  << space.values[2] << "]\n";
        if (it > 0) traj << ",\n";
        traj << "    {\"i\":" << (it + 1) << ",\"loss\":" << loss
             << ",\"r\":" << space.values[0] << ",\"g\":" << space.values[1]
             << ",\"b\":" << space.values[2] << "}";
        if (loss < 5e-5) {
            std::cout << "  early stop\n";
            break;
        }
    }
    traj << "\n  ]\n}\n";
    traj.close();
    const auto fitEnd = std::chrono::steady_clock::now();

    // ── Recovered SHOW multi-view ─────────────────────────────────────
    std::cout << "SHOW recovered (all views)...\n";
    applyTheta(space.values);
    ImageRGBA8 recoveredPrimary;
    for (int v = 0; v < nViews; ++v) {
        auto img = session.render(v, cfg.show, cfg.seed, cfg.showDenoise);
        savePNG(img, outDir / (std::string("recovered_") + inv.views[static_cast<size_t>(v)].name +
                               ".png"));
        if (v == 0) recoveredPrimary = std::move(img);
    }
    savePNG(recoveredPrimary, outDir / "recovered_show.png");

    // ── Relight showcase: same recovered ground, different lighting ───
    // (Scale key/fill intensities — safer than swapping multi-GB HDR mid-run.)
    if (studio) {
        std::cout << "SHOW relight (recovered materials + hot key light)...\n";
        inv.scene->forEachActor([&](Actor& a) {
            if (auto lc = a.getComponent<LightComponent>()) {
                if (a.getName() == "Key") lc->setIntensity(lc->getIntensity() * 2.5f);
                if (a.getName() == "Fill") lc->setIntensity(lc->getIntensity() * 0.3f);
                if (a.getName() == "Rim") lc->setIntensity(lc->getIntensity() * 1.5f);
            }
        });
        applyTheta(space.values);
        session.bound = false; // re-upload lights
        const ImageRGBA8 relightRec =
            session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        savePNG(relightRec, outDir / "recovered_relight.png");

        inv.setTargetAlbedo(inv.truthAlbedo);
        const ImageRGBA8 relightTruth =
            session.render(0, cfg.show, cfg.seed, cfg.showDenoise);
        savePNG(relightTruth, outDir / "truth_relight.png");
    }

    const std::vector<double> truthV{inv.truthAlbedo.r, inv.truthAlbedo.g, inv.truthAlbedo.b};
    const double paramErr = space.l2To(truthV);
    const double showRmse = rmseRGB(recoveredPrimary, targetsShow[0]);

    std::cout << "\n=== inverse_fit result ===\n";
    std::cout << "  scene=" << cfg.scene << "  views=" << nViews << "\n";
    std::cout << "  truth     RGB = [" << inv.truthAlbedo.r << ", " << inv.truthAlbedo.g << ", "
              << inv.truthAlbedo.b << "]\n";
    std::cout << "  recovered RGB = [" << space.values[0] << ", " << space.values[1] << ", "
              << space.values[2] << "]\n";
    std::cout << "  param L2 error = " << paramErr << "\n";
    std::cout << "  final multi-view FIT loss = " << loss << "\n";
    std::cout << "  SHOW RMSE (primary) = " << showRmse << "\n";
    std::cout << "  fit wall time = "
              << std::chrono::duration_cast<std::chrono::seconds>(fitEnd - fitStart).count()
              << " s\n";
    std::cout << "  wrote " << outDir << "/ (target_*, init_show, recovered_*, *relight*)\n";

    constexpr double kParamTol = 0.15;
    const bool ok = paramErr < kParamTol;
    std::cout << (ok ? "SELFTEST PASS" : "SELFTEST FAIL") << " (param L2 " << paramErr
              << (ok ? " < " : " >= ") << kParamTol << ")\n";

    inv.scene.reset();
    return ok ? 0 : 1;
}
