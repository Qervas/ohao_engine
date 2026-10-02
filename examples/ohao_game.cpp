// ohao_game -- the windowed host. Deferred rendering straight to a swapchain,
// fixed-step loop, input actions. A fly camera over the DamagedHelmet for now;
// gameplay arrives with the ECS work.
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
#include "scene/actor/actor.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/scene.hpp"

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

// The helmet on a floor under the studio HDRI, framed the way model_viewer frames it.
std::unique_ptr<Scene> buildHelmetScene(VulkanRenderer& renderer, const std::string& modelPath) {
    auto loaded = ModelLoader::loadResult(modelPath);
    if (!loaded) {
        std::cerr << "[ohao_game] failed to load " << modelPath << ": " << loaded.error() << std::endl;
        return nullptr;
    }
    std::shared_ptr<Model> model = std::move(loaded.value());
    const FrameResult frame = SceneFramer::computeFraming(model->vertices);
    const float S = frame.roomHalfSize;

    auto scene = std::make_unique<Scene>("ohao_game");

    // Floor
    {
        auto actor = scene->createActor("Floor");
        auto m = std::make_shared<Model>();
        auto mkv = [](glm::vec3 p) {
            Vertex v{};
            v.position = p;
            v.normal = {0, 1, 0};
            v.color = {0.18f, 0.10f, 0.05f};
            v.tangent = {1, 0, 0, 1};
            v.boneWeights = {1, 0, 0, 0};
            return v;
        };
        m->vertices = {mkv({-S, -S, -S}), mkv({S, -S, -S}), mkv({S, -S, S}), mkv({-S, -S, S})};
        m->indices = {0, 1, 2, 0, 2, 3};
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(m);
        mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = {0.18f, 0.10f, 0.05f};
        mat->getMaterial().roughness = 0.6f;
    }

    // Helmet
    {
        auto actor = scene->createActor("Model");
        actor->getTransform()->setRotation(frame.modelRotation);
        actor->getTransform()->setScale(glm::vec3(frame.modelScale));
        actor->getTransform()->setPosition(frame.modelPosition);
        auto mesh = actor->addComponent<MeshComponent>();
        mesh->setModel(model);
        mesh->setVisible(true);
        auto mat = actor->addComponent<MaterialComponent>();
        mat->getMaterial().baseColor = {0.8f, 0.7f, 0.6f};
        mat->getMaterial().roughness = 0.5f;
    }

    renderer.setEnvironmentMap("assets/test_models/env_studio.hdr");
    SceneFramer::applyLights(scene.get(), frame);
    renderer.setScene(scene.get());
    SceneFramer::applyCamera(renderer.getCamera(), frame);

    if (auto* deferred = renderer.getDeferredRenderer()) {
        if (auto* pp = deferred->getPostProcessing()) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.0f);
        }
    }
    return scene;
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

    auto scene = buildHelmetScene(app.renderer(), "assets/test_models/DamagedHelmet.glb");
    if (!scene) {
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

    GameHooks hooks;
    hooks.update = [&](Application& a, float dt, float) {
        if (a.input().pressed("quit")) a.quit();

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

    scene.reset();
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
