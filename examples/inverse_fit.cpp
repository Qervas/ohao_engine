// inverse_fit — Phase 1 inverse renderer: recover material albedo by image match.
//
// Self-test (default):
//   1. Render Cornell-like scene with known left-wall RGB (target I*).
//   2. Start from a wrong initial albedo.
//   3. Finite-difference GD on MSE(R(θ), I*) until recovered or max iters.
//
// Usage:
//   ./inverse_fit [--selftest] [--spp N] [--iters N] [--width W] [--height H]
//                 [--lr F] [--eps F] [--out-dir DIR]

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
    verts.push_back(mk(a));
    verts.push_back(mk(b));
    verts.push_back(mk(c));
    verts.push_back(mk(d));
    inds.push_back(base + 0);
    inds.push_back(base + 1);
    inds.push_back(base + 2);
    inds.push_back(base + 0);
    inds.push_back(base + 2);
    inds.push_back(base + 3);
}

void addWall(Scene* scene, std::string_view name,
             glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
             glm::vec3 normal, glm::vec3 color, float roughness = 0.95f) {
    auto actor = scene->createActor(name);
    auto model = std::make_shared<Model>();
    addQuad(model->vertices, model->indices, a, b, c, d, normal, color);
    auto mesh = actor->addComponent<MeshComponent>();
    mesh->setModel(model);
    mesh->setVisible(true);
    auto mat = actor->addComponent<MaterialComponent>();
    mat->getMaterial().baseColor = color;
    mat->getMaterial().roughness = roughness;
    mat->getMaterial().metallic = 0.0f;
}

struct FitConfig {
    uint32_t width{320};
    uint32_t height{180};
    int spp{8};
    int iters{25};
    double lr{8.0};      // gradients on masked wall are small; need large step
    double eps{0.06};
    double maskX{0.38};  // left-wall image fraction for loss
    std::string outDir{"renders/inverse"};
};

struct CliArgs {
    FitConfig cfg;
    bool selftest{true};
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
            a.selftest = true;
        } else if (s == "--spp") {
            a.cfg.spp = std::max(1, std::atoi(need("--spp")));
        } else if (s == "--iters") {
            a.cfg.iters = std::max(1, std::atoi(need("--iters")));
        } else if (s == "--width") {
            a.cfg.width = static_cast<uint32_t>(std::max(16, std::atoi(need("--width"))));
        } else if (s == "--height") {
            a.cfg.height = static_cast<uint32_t>(std::max(16, std::atoi(need("--height"))));
        } else if (s == "--lr") {
            a.cfg.lr = std::atof(need("--lr"));
        } else if (s == "--eps") {
            a.cfg.eps = std::atof(need("--eps"));
        } else if (s == "--mask-x") {
            a.cfg.maskX = std::atof(need("--mask-x"));
        } else if (s == "--out-dir") {
            a.cfg.outDir = need("--out-dir");
        } else if (s == "--help" || s == "-h") {
            std::cout
                << "Usage: inverse_fit [--selftest] [--spp N] [--iters N]\n"
                << "                   [--width W] [--height H] [--lr F] [--eps F]\n"
                << "                   [--mask-x F] [--out-dir DIR]\n"
                << "Phase 1 inverse renderer: recover left-wall albedo via FD + masked MSE.\n";
            std::exit(0);
        } else {
            std::cerr << "unknown arg: " << s << "\n";
            std::exit(2);
        }
    }
    return a;
}

// Minimal Cornell box: 5 walls + 1 sphere + 1 ceiling light (stable FD).
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
        sm->getMaterial().metallic = 0.0f;

        auto light = s.scene->createActor("KeyLight");
        auto lc = light->addComponent<LightComponent>();
        lc->setLightType(LightType::Sphere);
        lc->setColor({1.0f, 0.98f, 0.95f});
        lc->setIntensity(40.0f);
        lc->setRadius(0.6f);
        light->getTransform()->setPosition({0.0f, S - 0.5f, 0.0f});

        s.leftWall = nullptr;
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
        // Keep vertex colors in sync (some paths sample mesh color).
        if (auto mesh = leftWall->getComponent<MeshComponent>()) {
            if (auto model = mesh->getModel()) {
                for (auto& v : model->vertices) {
                    v.color = rgb;
                }
            }
        }
    }

    [[nodiscard]] glm::vec3 leftAlbedo() const {
        return leftMat ? leftMat->getMaterial().baseColor : glm::vec3(0);
    }
};

ImageRGBA8 renderOnce(VulkanRenderer& renderer, InverseScene& inv, int spp) {
    // Material / mesh edits need a full scene re-upload into RT buffers.
    renderer.setScene(inv.scene.get());
    renderer.resetAccumulation();
    const int frames = spp + 3;
    for (int i = 0; i < frames; ++i) {
        renderer.render();
    }
    const auto px = renderer.getPixelSpan();
    return ImageRGBA8::fromSpan(renderer.getWidth(), renderer.getHeight(), px);
}

bool savePNG(const ImageRGBA8& img, const std::filesystem::path& path) {
    if (img.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    return stbi_write_png(path.string().c_str(), static_cast<int>(img.width),
                          static_cast<int>(img.height), 4, img.rgba.data(),
                          static_cast<int>(img.width * 4)) != 0;
}

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);
    const FitConfig& cfg = args.cfg;

    std::cout << "OHAO inverse_fit — Phase 1 material recovery (FD + image MSE)\n";
    std::cout << "  resolution " << cfg.width << "x" << cfg.height
              << "  spp=" << cfg.spp << "  iters=" << cfg.iters
              << "  lr=" << cfg.lr << "  eps=" << cfg.eps << "\n";

    VulkanRenderer renderer(cfg.width, cfg.height);
    if (!renderer.initialize()) {
        std::cerr << "FATAL: renderer init failed\n";
        return 1;
    }

    InverseScene inv = InverseScene::build();
    if (!inv.leftMat) {
        std::cerr << "FATAL: left wall material missing\n";
        return 1;
    }

    auto& camera = renderer.getCamera();
    camera.setPosition({0.0f, 0.0f, 13.0f});
    camera.setFov(38.0f);
    camera.setRotation(0.0f, -90.0f);

    renderer.setRenderMode(RenderMode::RTOffline);
    renderer.setDenoiseMode(DenoiseMode::None);

    // Ground-truth parameters (what we want to recover).
    const glm::vec3 truth{0.65f, 0.05f, 0.05f};
    const glm::vec3 initGuess{0.2f, 0.5f, 0.7f};  // deliberately wrong (blue-ish)

    inv.setLeftAlbedo(truth);
    std::cout << "Rendering target (truth albedo "
              << truth.r << "," << truth.g << "," << truth.b << ")...\n";
    auto t0 = std::chrono::steady_clock::now();
    const ImageRGBA8 target = renderOnce(renderer, inv, cfg.spp);
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  target done in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms\n";

    const auto outDir = std::filesystem::path(cfg.outDir);
    savePNG(target, outDir / "target.png");

    // Optimizable θ = left wall albedo RGB.
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
        const ImageRGBA8 img = renderOnce(renderer, inv, cfg.spp);
        return mseRGB(img, target, cfg.maskX);
    };

    applyTheta(space.values);
    ImageRGBA8 current = renderOnce(renderer, inv, cfg.spp);
    savePNG(current, outDir / "init.png");
    double loss = mseRGB(current, target, cfg.maskX);
    std::cout << "  init loss (MSE)=" << loss << "  θ=["
              << space.values[0] << "," << space.values[1] << "," << space.values[2] << "]\n";

    const auto fitStart = std::chrono::steady_clock::now();
    for (int it = 0; it < cfg.iters; ++it) {
        const auto g = finiteDiffGradient(space, cfg.eps, lossAt);
        gdStep(space, g, cfg.lr);
        applyTheta(space.values);
        current = renderOnce(renderer, inv, cfg.spp);
        loss = mseRGB(current, target, cfg.maskX);

        std::cout << "  iter " << (it + 1) << "/" << cfg.iters
                  << "  loss=" << loss
                  << "  θ=[" << space.values[0] << "," << space.values[1] << ","
                  << space.values[2] << "]"
                  << "  ∇=[" << g[0] << "," << g[1] << "," << g[2] << "]\n";

        if (loss < 1e-4) {
            std::cout << "  early stop (loss < 1e-4)\n";
            break;
        }
    }
    const auto fitEnd = std::chrono::steady_clock::now();

    savePNG(current, outDir / "recovered.png");

    const std::vector<double> truthV{truth.r, truth.g, truth.b};
    const double paramErr = space.l2To(truthV);
    const double finalLoss = loss;
    const double maxPix = maxAbsRGB(current, target);

    std::cout << "\n=== inverse_fit result ===\n";
    std::cout << "  truth     RGB = [" << truth.r << ", " << truth.g << ", " << truth.b << "]\n";
    std::cout << "  recovered RGB = [" << space.values[0] << ", " << space.values[1] << ", "
              << space.values[2] << "]\n";
    std::cout << "  param L2 error = " << paramErr << "\n";
    std::cout << "  final image MSE = " << finalLoss << "  max|Δ|px = " << maxPix << "\n";
    std::cout << "  fit wall time = "
              << std::chrono::duration_cast<std::chrono::seconds>(fitEnd - fitStart).count()
              << " s\n";
    std::cout << "  wrote " << (outDir / "target.png") << ", init.png, recovered.png\n";

    // Success criteria for Phase 1 self-test: albedo within ~0.20 L2 (noisy FD).
    constexpr double kParamTol = 0.20;
    const bool ok = paramErr < kParamTol;
    if (ok) {
        std::cout << "SELFTEST PASS (param L2 < " << kParamTol << ")\n";
    } else {
        std::cout << "SELFTEST FAIL (param L2 " << paramErr << " >= " << kParamTol << ")\n";
    }

    inv.scene.reset();
    return ok ? 0 : 1;
}
