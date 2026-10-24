// instantiate_gpu_test (sub-project 3, step 7; test 11's GPU half).
//
// Renders the Cornell box twice in deferred mode, each with a fresh renderer:
//   A. built the way the examples build it today: spawnModel with generateQuad walls and
//      generator spheres;
//   B. built from data: MeshSource {QuadPrim | SpherePrim} + MaterialOverride entities,
//      turned into meshes by the instantiate system on a real RenderScene.
// The two images must be bit-identical (B shares one sphere mesh and deduplicates the
// wall materials; neither may change a pixel). Then B's spheres are destroyed: their
// shared mesh must be released (RenderScene::meshInfo goes null) and rendering must go on
// cleanly. Validation layers are on for the whole run and must report nothing.
//
// Exit code 0 = pass. The last line is a JSON summary.
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "render/camera/camera.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ohao;

namespace {

constexpr uint32_t W = 960, H = 540;
constexpr int kFrames = 10;
constexpr float S = 5.0f;

struct Wall {
    const char* name;
    glm::vec3 a, b, c, d, normal, color;
};

std::vector<Wall> walls() {
    const glm::vec3 white(0.73f), red(0.65f, 0.05f, 0.05f), green(0.12f, 0.45f, 0.15f);
    const glm::vec3 LBB(-S, -S, -S), RBB(S, -S, -S), LTB(-S, S, -S), RTB(S, S, -S);
    const glm::vec3 LBF(-S, -S, S), RBF(S, -S, S), LTF(-S, S, S), RTF(S, S, S);
    return {{"Back", LBB, RBB, RTB, LTB, {0, 0, 1}, white},     {"Left", LBB, LTB, LTF, LBF, {1, 0, 0}, red},
            {"Right", RBB, RBF, RTF, RTB, {-1, 0, 0}, green},   {"Floor", LBB, LBF, RBF, RBB, {0, 1, 0}, white},
            {"Ceiling", LTB, RTB, RTF, LTF, {0, -1, 0}, white}};
}

struct Ball {
    const char* name;
    glm::vec3 position;
    float scale;
    ecs::MaterialOverride material;
};

std::vector<Ball> balls() {
    return {{"MetalSphere", {-2.0f, -S + 2.0f, 0.0f}, 2.0f, {{0.95f, 0.93f, 0.88f}, 0.05f, 1.0f, 0.0f}},
            {"GlassSphere", {2.5f, -S + 1.8f, 1.5f}, 1.8f, {{0.9f, 0.95f, 1.0f}, 0.02f, 0.0f, 0.0f}}};
}

void addLights(ecs::World& world) {
    const glm::vec3 colors[] = {{1.0f, 0.3f, 0.2f}, {0.2f, 1.0f, 0.3f}, {0.3f, 0.4f, 1.0f}, {1.0f, 0.9f, 0.3f}};
    const glm::vec3 positions[] = {{-3, 4, -3}, {3, 4, -3}, {-3, 4, 3}, {3, 4, 3}};
    for (int i = 0; i < 4; ++i) {
        ecs::Light light;
        light.color = colors[i];
        light.intensity = 5.0f;
        light.radius = 0.3f;
        ecs::Transform t;
        t.position = positions[i];
        ecs::spawnLight(world, "Light" + std::to_string(i), light, t);
    }
}

render::MaterialDesc toDesc(const ecs::MaterialOverride& o) {
    render::MaterialDesc d;
    d.baseColor = o.baseColor;
    d.roughness = o.roughness;
    d.metallic = o.metallic;
    d.emissiveStrength = o.emissiveStrength;
    return d;
}

void setupCamera(VulkanRenderer& renderer) {
    auto& camera = renderer.getCamera();
    camera.setPosition({0.0f, 0.0f, 13.0f});
    camera.setFov(38.0f);
    camera.setRotation(0.0f, -90.0f);
    renderer.setRenderMode(RenderMode::Deferred);
}

struct RunResult {
    std::vector<uint8_t> pixels;
    uint64_t validation{0};
    bool ok{false};
    std::string note;
};

// Renders `frames` frames, the last one requested for copyPixels().
void renderFrames(ecs::World& world, VulkanRenderer& renderer, int frames) {
    for (int i = 0; i < frames; ++i) {
        world.update(0.0f);
        if (i == frames - 1) renderer.requestReadback();
        renderer.render();
    }
}

std::vector<uint8_t> copyPixels(VulkanRenderer& renderer) {
    const auto span = renderer.readback();
    return std::vector<uint8_t>(span.begin(), span.end());
}

// A: spawnModel, as the examples build it today.
RunResult renderSpawned() {
    RunResult r;
    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) return r;
    {
        ecs::World world;
        ecs::installRenderExtract(world, renderer.renderScene());
        for (const Wall& w : walls()) {
            Model model;
            PrimitiveMeshGenerator::generateQuad(model, w.a, w.b, w.c, w.d, w.normal, w.color);
            render::MaterialDesc mat;
            mat.baseColor = w.color;
            mat.roughness = 0.95f;
            ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), model, w.name, {}, &mat);
        }
        const auto sphere = PrimitiveMeshGenerator::makeSphere();
        for (const Ball& b : balls()) {
            ecs::Transform t;
            t.position = b.position;
            t.scale = glm::vec3(b.scale);
            const render::MaterialDesc mat = toDesc(b.material);
            ecs::spawnModel(world, renderer.renderScene(), renderer.getTextureManager(), *sphere, b.name, t, &mat);
        }
        addLights(world);
        setupCamera(renderer);
        renderFrames(world, renderer, kFrames);
        r.pixels = copyPixels(renderer);
    }
    r.validation = renderer.validationMessageCount();
    r.ok = !r.pixels.empty();
    return r;
}

// B: data (MeshSource + MaterialOverride) through instantiate.
RunResult renderInstantiated(bool& releasedOk) {
    RunResult r;
    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) return r;
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    {
        ecs::World world;
        ecs::installRenderExtract(world, renderer.renderScene());
        ecs::installInstantiate(world, renderer.renderScene(), renderer.getTextureManager(), assets);
        auto& reg = world.registry();
        for (const Wall& w : walls()) {
            const auto e = world.create(w.name);
            reg.emplace<ecs::MeshSource>(e, ecs::MeshSource{ecs::QuadPrim{w.a, w.b, w.c, w.d, w.normal, w.color}});
            reg.emplace<ecs::MaterialOverride>(e, ecs::MaterialOverride{w.color, 0.95f, 0.0f, 0.0f});
        }
        std::vector<entt::entity> spheres;
        for (const Ball& b : balls()) {
            ecs::Transform t;
            t.position = b.position;
            t.scale = glm::vec3(b.scale);
            const auto e = world.create(b.name, t);
            reg.emplace<ecs::MeshSource>(e, ecs::MeshSource{ecs::SpherePrim{}});
            reg.emplace<ecs::MaterialOverride>(e, b.material);
            spheres.push_back(e);
        }
        addLights(world);
        setupCamera(renderer);
        renderFrames(world, renderer, kFrames);
        r.pixels = copyPixels(renderer);

        const render::MeshHandle shared = reg.get<ecs::MeshRenderer>(spheres[0]).mesh;
        const bool sharedOk = shared == reg.get<ecs::MeshRenderer>(spheres[1]).mesh;
        if (!sharedOk) r.note += "spheres do not share one mesh; ";
        if (!ecs::instantiateErrors(world).empty()) r.note += "instantiate reported errors; ";
        world.destroy(spheres[0]);
        renderFrames(world, renderer, 2);
        const bool kept = renderer.renderScene().meshInfo(shared) != nullptr;
        world.destroy(spheres[1]);
        renderFrames(world, renderer, 5);   // past the frames in flight: the BLAS is destroyed
        const bool released = renderer.renderScene().meshInfo(shared) == nullptr;
        releasedOk = sharedOk && kept && released;
        if (!kept) r.note += "the shared mesh was released while a sphere still used it; ";
        if (!released) r.note += "the shared mesh was not released with its last user; ";
    }
    r.validation = renderer.validationMessageCount();
    r.ok = !r.pixels.empty();
    return r;
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("OHAO_VALIDATION", "1");
#else
    setenv("OHAO_VALIDATION", "1", 1);
#endif
    std::printf("instantiate_gpu_test (%ux%u, deferred, %d frames, validation on)\n", W, H, kFrames);
    const RunResult a = renderSpawned();
    bool releasedOk = false;
    const RunResult b = renderInstantiated(releasedOk);

    std::size_t differing = 0;
    if (a.ok && b.ok && a.pixels.size() == b.pixels.size())
        for (std::size_t i = 0; i < a.pixels.size(); ++i) differing += a.pixels[i] != b.pixels[i];
    const bool identical = a.ok && b.ok && a.pixels.size() == b.pixels.size() && differing == 0;

    struct Check {
        const char* what;
        bool ok;
    };
    const Check checks[] = {
        {"both renders produced pixels", a.ok && b.ok},
        {"instantiate renders the same image as spawnModel, bit for bit", identical},
        {"the shared sphere mesh is released with its last user, not before", releasedOk},
        {"validation reported nothing (spawnModel run)", a.validation == 0},
        {"validation reported nothing (instantiate run, including the release)", b.validation == 0},
    };
    int passed = 0, failed = 0;
    std::string failures;
    for (const Check& c : checks) {
        std::printf("  %-72s %s\n", c.what, c.ok ? "PASS" : "FAIL");
        if (c.ok) ++passed;
        else {
            ++failed;
            failures += std::string(failures.empty() ? "" : ",") + "\"" + c.what + "\"";
        }
    }
    if (!identical) std::printf("  differing bytes: %zu of %zu\n", differing, a.pixels.size());
    if (!b.note.empty()) std::printf("  note: %s\n", b.note.c_str());
    std::printf("  validation messages: %llu + %llu\n", static_cast<unsigned long long>(a.validation),
                static_cast<unsigned long long>(b.validation));
    std::printf("{\"suite\":\"instantiate_gpu_test\",\"passed\":%d,\"failed\":%d,\"failures\":[%s]}\n", passed, failed,
                failures.c_str());
    return failed == 0 ? 0 : 1;
}
