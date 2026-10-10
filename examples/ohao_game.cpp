// ohao_game -- the windowed host. Deferred rendering straight to a swapchain,
// fixed-step loop, input actions, the scene as ECS entities. A fly camera over the
// DamagedHelmet and a cube spun through World::setTransform.
//
//   ohao_game [--vsync] [--frames N] [--resize-at F]
//
//   WASD move · Q/E down/up · hold right mouse to look · Shift faster · Esc quits
//
// --frames N runs N frames and exits non-zero if any frame did a GPU->CPU
// readback, or if validation was on (OHAO_VALIDATION=1) and reported anything.
// --resize-at F resizes the window to 1280x720 at frame F. Together they are the
// presentation gate:  OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200

#include "runtime/application.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "ecs/spawn.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using namespace ohao;
using namespace ohao::runtime;

namespace {

int argInt(int argc, char** argv, const char* name, int fallback) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    return fallback;
}

bool argFlag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

// GLFW key / button codes. The input map is GLFW-free; these are just numbers.
constexpr int KEY_W = 87, KEY_A = 65, KEY_S = 83, KEY_D = 68, KEY_Q = 81, KEY_E = 69;
constexpr int KEY_ESCAPE = 256, KEY_LEFT_SHIFT = 340, MOUSE_RIGHT = 1;

// The helmet on a floor under the studio HDRI, framed the way model_viewer frames it,
// plus a cube the update hook spins. Returns the cube entity (entt::null on failure).
entt::entity buildHelmetScene(Application& app, const std::string& modelPath) {
    VulkanRenderer& renderer = app.renderer();
    ecs::World& world = app.world();
    auto loaded = ModelLoader::loadResult(modelPath);
    if (!loaded) {
        std::cerr << "[ohao_game] failed to load " << modelPath << ": " << loaded.error() << std::endl;
        return entt::null;
    }
    std::shared_ptr<Model> model = std::move(loaded.value());
    const FrameResult frame = SceneFramer::computeFraming(model->vertices);
    const float S = frame.roomHalfSize;
    render::RenderScene& rs = renderer.renderScene();
    BindlessTextureManager* tm = renderer.getTextureManager();

    // Floor
    {
        Model m;
        auto mkv = [](glm::vec3 p) {
            Vertex v{};
            v.position = p;
            v.normal = {0, 1, 0};
            v.color = {0.18f, 0.10f, 0.05f};
            v.tangent = {1, 0, 0, 1};
            v.boneWeights = {1, 0, 0, 0};
            return v;
        };
        m.vertices = {mkv({-S, -S, -S}), mkv({S, -S, -S}), mkv({S, -S, S}), mkv({-S, -S, S})};
        m.indices = {0, 1, 2, 0, 2, 3};
        render::MaterialDesc mat;
        mat.baseColor = {0.18f, 0.10f, 0.05f};
        mat.roughness = 0.6f;
        ecs::spawnModel(world, rs, tm, m, "Floor", ecs::Transform{}, &mat);
    }

    // Helmet
    {
        ecs::Transform t;
        t.position = frame.modelPosition;
        t.rotation = frame.modelRotation;
        t.scale = glm::vec3(frame.modelScale);
        render::MaterialDesc mat;
        mat.baseColor = {0.8f, 0.7f, 0.6f};
        mat.roughness = 0.5f;
        ecs::spawnModel(world, rs, tm, *model, "Model", t, &mat);
    }

    // The spinning cube, to the helmet's right, clear of the floor.
    entt::entity cube = entt::null;
    {
        ecs::Transform t;
        t.position = {S * 0.8f, -S + 4.0f, 4.0f};
        t.scale = glm::vec3(3.0f);
        render::MaterialDesc mat;
        mat.baseColor = {0.70f, 0.12f, 0.08f};
        mat.roughness = 0.35f;
        cube = ecs::spawnModel(world, rs, tm, *PrimitiveMeshGenerator::makeCube(), "SpinningCube", t, &mat);
    }

    renderer.setEnvironmentMap("assets/test_models/env_studio.hdr");
    SceneFramer::applyLights(world, frame);
    SceneFramer::applyCamera(renderer.getCamera(), frame);

    if (auto* deferred = renderer.getDeferredRenderer()) {
        if (auto* pp = deferred->getPostProcessing()) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.0f);
        }
    }
    return cube;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig cfg;
    cfg.window.title = "OHAO";
    cfg.vsync = argFlag(argc, argv, "--vsync");
    cfg.frameLimit = argInt(argc, argv, "--frames", 0);
    cfg.resizeAtFrame = argInt(argc, argv, "--resize-at", -1);

    Application app;
    if (!app.initialize(cfg)) return 1;

    const entt::entity cube = buildHelmetScene(app, "assets/test_models/DamagedHelmet.glb");
    if (cube == entt::null) {
        (void)app.shutdown();
        return 2;
    }

    InputMap& in = app.input();
    in.bindAxis("move_forward", {InputDevice::Key, KEY_W}, {InputDevice::Key, KEY_S});
    in.bindAxis("move_right", {InputDevice::Key, KEY_D}, {InputDevice::Key, KEY_A});
    in.bindAxis("move_up", {InputDevice::Key, KEY_E}, {InputDevice::Key, KEY_Q});
    in.bindAction("look", {InputDevice::MouseButton, MOUSE_RIGHT});
    in.bindAction("fast", {InputDevice::Key, KEY_LEFT_SHIFT});
    in.bindAction("quit", {InputDevice::Key, KEY_ESCAPE});

    Camera& cam = app.renderer().getCamera();
    float yaw = cam.getYaw();
    float pitch = cam.getPitch();

    float cubeAngle = 0.0f;
    GameHooks hooks;
    hooks.update = [&](Application& a, float dt, float) {
        if (a.input().pressed("quit")) a.quit();

        // Spin the cube: a tracked transform write moves it in raster and RT alike.
        cubeAngle += dt * 1.2f;
        ecs::Transform t = a.world().registry().get<ecs::Transform>(cube);
        t.rotation = glm::angleAxis(cubeAngle, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
        a.world().setTransform(cube, t);

        const bool looking = a.input().held("look");
        a.window().setCursorCaptured(looking);
        if (looking) {
            const glm::vec2 d = a.input().mouseDelta();
            yaw += d.x * 0.1f;
            pitch = std::clamp(pitch - d.y * 0.1f, -89.0f, 89.0f);
            cam.setRotation(pitch, yaw);
        }

        const float speed = (a.input().held("fast") ? 12.0f : 3.0f) * dt;
        cam.setPosition(cam.getPosition() +
                        cam.getFront() * (a.input().axis("move_forward") * speed) +
                        cam.getRight() * (a.input().axis("move_right") * speed) +
                        glm::vec3(0, 1, 0) * (a.input().axis("move_up") * speed));
    };

    const RunReport r = app.run(hooks);

    std::cout << "\nframes " << r.frames << "  fps " << r.fps << "  frame " << r.avgFrameMs
              << " ms  gpu " << r.avgGpuMs << " ms\n"
              << "readback copies " << r.readbackCopies << "  validation "
              << (r.validationEnabled ? std::to_string(r.validationMessages) + " messages"
                                      : std::string("NOT ENABLED (set OHAO_VALIDATION=1)"))
              << std::endl;

    // Counted after teardown: leaked objects are reported only at device destruction.
    const uint64_t validationTotal = app.shutdown();
    if (r.validationEnabled)
        std::cout << "validation including teardown: " << validationTotal << " messages" << std::endl;

    if (cfg.frameLimit > 0) {
        if (r.readbackCopies != 0) return 3;
        if (r.validationEnabled && validationTotal != 0) return 4;
    }
    return 0;
}
