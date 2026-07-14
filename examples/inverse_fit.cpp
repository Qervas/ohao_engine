// inverse_fit — Phase A inverse renderer (dual-budget, grain-free SHOW).
//
// FIT  = raw MC (noise is a feature for FD / inverse) — denoise NEVER on
// SHOW = high-res stills, OIDN by default so outputs are grain-free
//
// Usage:
//   ./inverse_fit --selftest --quality high
//   ./inverse_fit --selftest --show-denoise=none   # raw SHOW
//   ./inverse_fit --selftest --show-denoise=oidn   # default

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include "gpu/vulkan/renderer.hpp"
#include "inverse/inverse_module.hpp"
#include "render/camera/camera.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "scene/actor/actor.hpp"
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

void addQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
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
    inds = {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3};
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
    mat->getMaterial().metallic = 0.0f;
}

struct FitConfig {
    QualityPreset quality{kQualityHigh};
    RenderBudget fit{kQualityHigh.fit};
    RenderBudget show{kQualityHigh.show};
    int iters{25};
    double lr{0.08};     // Adam learning rate
    double eps{0.05};    // FD step
    double maskX{0.40};
    uint32_t seed{1};
    std::string outDir{"renders/inverse"};
    bool useAdam{true};
    // SHOW: grain-free for humans. FIT always forces DenoiseMode::None.
    DenoiseMode showDenoise{DenoiseMode::OIDN};
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
        } else if (s == "--seed") {
            a.cfg.seed = static_cast<uint32_t>(std::atoi(need("--seed")));
        } else if (s == "--gd") {
            a.cfg.useAdam = false;
        } else if (s.starts_with("--show-denoise=")) {
            a.cfg.showDenoise = parseDenoiseMode(s.substr(std::string_view("--show-denoise=").size()));
            // Restrict SHOW denoisers to static offline backends
            if (a.cfg.showDenoise != DenoiseMode::None && a.cfg.showDenoise != DenoiseMode::OIDN) {
                std::cerr << "SHOW denoise: use none or oidn (got "
                          << denoiseModeName(a.cfg.showDenoise) << ")\n";
                std::exit(2);
            }
        } else if (s == "--out-dir") {
            a.cfg.outDir = need("--out-dir");
        } else if (s == "--help" || s == "-h") {
            std::cout
                << "Usage: inverse_fit [--selftest] [--quality draft|high|ultra|cinema]\n"
                << "  --fit-width/height/spp   internal FD budget (always raw MC)\n"
                << "  --show-width/height/spp  presentation stills (default high=1080p@1024)\n"
                << "  --show-denoise=oidn|none  grain-free SHOW (default oidn); FIT never denoises\n"
                << "  --iters --lr --eps --mask-x --seed --gd --out-dir\n";
            std::exit(0);
        } else {
            std::cerr << "unknown arg: " << s << "\n";
            std::exit(2);
        }
    }
    return a;
}

struct InverseScene {
    std::unique_ptr<Scene> scene;
    Actor* leftWall{nullptr};
    MaterialComponent* leftMat{nullptr};

    static InverseScene build() {
        InverseScene s;
        s.scene = std::make_unique<Scene>("Inverse Cornell");
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
                s.leftWall = &a;
                s.leftMat = a.getComponent<MaterialComponent>().get();
            }
        });
        return s;
    }

    void setLeftAlbedo(glm::vec3 rgb) {
        if (!leftMat) return;
        leftMat->getMaterial().baseColor = rgb;
        if (leftWall) {
            if (auto mesh = leftWall->getComponent<MeshComponent>()) {
                if (auto model = mesh->getModel()) {
                    for (auto& v : model->vertices) v.color = rgb;
                }
            }
        }
    }
};

bool savePNG(const ImageRGBA8& img, const std::filesystem::path& path) {
    if (img.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    return stbi_write_png(path.string().c_str(), static_cast<int>(img.width),
                          static_cast<int>(img.height), 4, img.rgba.data(),
                          static_cast<int>(img.width * 4)) != 0;
}

ImageRGBA8 renderBudget(VulkanRenderer& renderer, InverseScene& inv,
                        const RenderBudget& budget, uint32_t seed,
                        DenoiseMode denoise) {
    if (renderer.getWidth() != budget.width || renderer.getHeight() != budget.height) {
        renderer.resize(budget.width, budget.height);
    }
    // FIT: always raw (white noise is a feature for inverse / FD).
    // SHOW: OIDN (or none) for grain-free stills humans actually open.
    renderer.setDenoiseMode(denoise);
    renderer.setRenderSeed(seed);
    renderer.setScene(inv.scene.get());
    renderer.resetAccumulation();
    const int frames = budget.spp + 3;
    for (int i = 0; i < frames; ++i) renderer.render();
    return ImageRGBA8::fromSpan(renderer.getWidth(), renderer.getHeight(),
                                renderer.getPixelSpan());
}

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);
    const FitConfig& cfg = args.cfg;

    std::cout << "OHAO inverse_fit — dual-budget material recovery\n";
    std::cout << "  quality=" << cfg.quality.name
              << "  FIT " << cfg.fit.width << "x" << cfg.fit.height << " @" << cfg.fit.spp << " spp"
              << "  SHOW " << cfg.show.width << "x" << cfg.show.height << " @" << cfg.show.spp
              << " spp\n";
    std::cout << "  iters=" << cfg.iters << " lr=" << cfg.lr << " seed=" << cfg.seed
              << " optimizer=" << (cfg.useAdam ? "adam" : "gd")
              << "  SHOW denoise=" << denoiseModeName(cfg.showDenoise)
              << "  FIT denoise=none (raw MC)\n";

    // Create at SHOW size so first still is correct; FIT will resize down.
    VulkanRenderer renderer(cfg.show.width, cfg.show.height);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    InverseScene inv = InverseScene::build();
    if (!inv.leftMat) {
        std::cerr << "FATAL: left wall missing\n";
        return 1;
    }

    auto& camera = renderer.getCamera();
    camera.setPosition({0.0f, 0.0f, 13.0f});
    camera.setFov(38.0f);
    camera.setRotation(0.0f, -90.0f);

    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.setRenderSeed(cfg.seed);

    const glm::vec3 truth{0.65f, 0.05f, 0.05f};
    const glm::vec3 initGuess{0.2f, 0.5f, 0.7f};
    const auto outDir = std::filesystem::path(cfg.outDir);

    // ── SHOW: target (grain-free if OIDN) ─────────────────────────────
    std::cout << "SHOW target (truth) " << cfg.show.width << "x" << cfg.show.height
              << " @" << cfg.show.spp << " spp  denoise="
              << denoiseModeName(cfg.showDenoise) << "...\n";
    inv.setLeftAlbedo(truth);
    auto t0 = std::chrono::steady_clock::now();
    const ImageRGBA8 targetShow =
        renderBudget(renderer, inv, cfg.show, cfg.seed, cfg.showDenoise);
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  done in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms → target_show.png\n";
    savePNG(targetShow, outDir / "target_show.png");

    // ── FIT: target buffer — always raw MC (noise helps inverse/FD) ───
    std::cout << "FIT target " << cfg.fit.width << "x" << cfg.fit.height << " @"
              << cfg.fit.spp << " spp (raw)...\n";
    inv.setLeftAlbedo(truth);
    const ImageRGBA8 targetFit =
        renderBudget(renderer, inv, cfg.fit, cfg.seed, DenoiseMode::None);

    ParamSpace space;
    space.add("left.R", initGuess.r, 0.0, 1.0);
    space.add("left.G", initGuess.g, 0.0, 1.0);
    space.add("left.B", initGuess.b, 0.0, 1.0);

    auto applyTheta = [&](const std::vector<double>& th) {
        inv.setLeftAlbedo({static_cast<float>(th[0]), static_cast<float>(th[1]),
                           static_cast<float>(th[2])});
    };

    auto lossAt = [&](const std::vector<double>& th) -> double {
        applyTheta(th);
        const ImageRGBA8 img =
            renderBudget(renderer, inv, cfg.fit, cfg.seed, DenoiseMode::None);
        return mseRGB(img, targetFit, cfg.maskX);
    };

    // ── SHOW: init ────────────────────────────────────────────────────
    std::cout << "SHOW init (wrong guess)...\n";
    applyTheta(space.values);
    const ImageRGBA8 initShow =
        renderBudget(renderer, inv, cfg.show, cfg.seed, cfg.showDenoise);
    savePNG(initShow, outDir / "init_show.png");

    double loss = lossAt(space.values);
    std::cout << "  init FIT loss (masked MSE)=" << loss << "  θ=["
              << space.values[0] << "," << space.values[1] << "," << space.values[2] << "]\n";

    AdamState adam;
    adam.resize(space.size());
    std::ofstream traj(outDir / "trajectory.json");
    traj << "{\n  \"quality\": \"" << cfg.quality.name << "\",\n  \"iters\": [\n";

    const auto fitStart = std::chrono::steady_clock::now();
    for (int it = 0; it < cfg.iters; ++it) {
        const auto g = finiteDiffGradient(space, cfg.eps, lossAt);
        if (cfg.useAdam) {
            adam.step(space, g, cfg.lr);
        } else {
            gdStep(space, g, cfg.lr * 50.0); // GD needs larger scale
        }
        loss = lossAt(space.values);

        std::cout << "  iter " << (it + 1) << "/" << cfg.iters
                  << "  loss=" << loss
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

    // ── SHOW: recovered (grain-free) ──────────────────────────────────
    std::cout << "SHOW recovered...\n";
    applyTheta(space.values);
    const ImageRGBA8 recoveredShow =
        renderBudget(renderer, inv, cfg.show, cfg.seed, cfg.showDenoise);
    savePNG(recoveredShow, outDir / "recovered_show.png");

    const std::vector<double> truthV{truth.r, truth.g, truth.b};
    const double paramErr = space.l2To(truthV);
    const double showRmse = rmseRGB(recoveredShow, targetShow);

    std::cout << "\n=== inverse_fit result ===\n";
    std::cout << "  truth     RGB = [" << truth.r << ", " << truth.g << ", " << truth.b << "]\n";
    std::cout << "  recovered RGB = [" << space.values[0] << ", " << space.values[1] << ", "
              << space.values[2] << "]\n";
    std::cout << "  param L2 error = " << paramErr << "\n";
    std::cout << "  final FIT loss = " << loss << "\n";
    std::cout << "  SHOW RMSE (full frame) = " << showRmse << "\n";
    std::cout << "  fit wall time = "
              << std::chrono::duration_cast<std::chrono::seconds>(fitEnd - fitStart).count()
              << " s\n";
    std::cout << "  wrote " << outDir
              << "/{target_show,init_show,recovered_show}.png + trajectory.json\n";

    constexpr double kParamTol = 0.12;
    const bool ok = paramErr < kParamTol;
    std::cout << (ok ? "SELFTEST PASS" : "SELFTEST FAIL") << " (param L2 "
              << paramErr << (ok ? " < " : " >= ") << kParamTol << ")\n";

    inv.scene.reset();
    return ok ? 0 : 1;
}
