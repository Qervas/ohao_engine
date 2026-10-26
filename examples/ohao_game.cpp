// ohao_game -- the windowed game: a scene file played by the live host (runtime::LiveHost)
// in realtime. Deferred rendering straight to a swapchain, fixed-step simulation, input as
// data: the scene's InputActions bind the keys, its FlyController flies the camera, its
// Spin turns the cube, its PlayerController walks. No host code moves anything.
//
//   ohao_game [scene] [--vsync] [--frames N] [--resize-at F] [--mode deferred|rt_realtime|rt_offline]
//
//   default scene: assets/scenes/helmet_game.scene.json; --mode: what the window presents
//   (the path tracer needs ray tracing; rt_offline accumulates while the view stands still)
//   a fly scene:  WASD move · Q/E down/up · hold right mouse to look · Shift faster · Esc quits
//   play mode (a scene with a PlayerController, e.g. assets/scenes/blockout_play.scene.json):
//                 the mouse is captured while the window has focus and looks; WASD walk,
//                 Shift sprints, Space jumps; Esc (release_mouse) frees the mouse, a click
//                 (capture_mouse) takes it again; close the window to quit
//
// --frames N runs N frames and exits non-zero if any frame did an unrequested GPU->CPU
// readback, or if validation was on (OHAO_VALIDATION=1) and reported anything. --resize-at
// F resizes the window to 1280x720 at frame F. Together they are the presentation gate:
//   OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200
//
// Exit codes: 0 ok; 1 no window or GPU; 2 the scene did not load; 3 a readback; 4 validation.

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "runtime/live_host.hpp"

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

// The first argument that is not an option or an option's value.
std::string sceneArg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" || a == "--resize-at" || a == "--mode") { ++i; continue; }
        if (a.rfind("--", 0) == 0) continue;
        return a;
    }
    return "assets/scenes/helmet_game.scene.json";
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    LiveConfig cfg;
    cfg.window.title = "OHAO";
    cfg.vsync = argFlag(argc, argv, "--vsync");
    cfg.frameLimit = argInt(argc, argv, "--frames", 0);
    cfg.resizeAtFrame = argInt(argc, argv, "--resize-at", -1);
    cfg.timeMode = TimeMode::realtime;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") != 0) continue;
        const std::string m = argv[i + 1];
        if (m == "rt_realtime") cfg.mode = RenderMode::RTRealtime;
        else if (m == "rt_offline") cfg.mode = RenderMode::RTOffline;
        else if (m != "deferred") {
            std::cerr << "[ohao_game] --mode is one of: deferred, rt_realtime, rt_offline" << std::endl;
            return 1;
        }
    }

    LiveHost host;
    if (!host.initialize(cfg, engineTypes())) return 1;

    const std::string scene = sceneArg(argc, argv);
    std::string params = "{\"path\":\"" + scene + "\"}";
    const std::string loaded = host.session().call("scene.load", params);
    if (loaded.rfind("{\"result\":", 0) != 0) {
        std::cerr << "[ohao_game] " << scene << " did not load: " << loaded << std::endl;
        (void)host.shutdown();
        return 2;
    }
    // Render settings are the host's (a scene file holds none): the look ohao_game has
    // always had, post effects on at exposure 1.
    if (auto* deferred = host.renderer().getDeferredRenderer()) {
        if (auto* pp = deferred->getPostProcessing()) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.0f);
        }
    }

    GameHooks hooks;
    // Play mode: the world has a player, whose look is the mouse.
    const bool play = !host.world().registry().view<ecs::PlayerController>().empty();
    auto released = std::make_shared<bool>(false);   // play mode: Esc freed the mouse
    // Input is stepped once per fixed step, so it is read after each step.
    hooks.fixedUpdate = [play, released](LiveHost& h, float) {
        if (!play) {
            if (h.input().pressed("quit")) h.quit();
            return;
        }
        if (h.input().pressed("release_mouse")) *released = true;
        if (h.input().pressed("capture_mouse")) *released = false;
    };
    hooks.update = [play, released](LiveHost& h, float, float) {
        h.window().setCursorCaptured(play ? !*released && h.window().focused() : h.input().held("look"));
    };

    const RunReport r = host.run(hooks);

    std::cout << "\nframes " << r.frames << "  fps " << r.fps << "  frame " << r.avgFrameMs << " ms  gpu "
              << r.avgGpuMs << " ms\n"
              << "readback copies " << r.readbackCopies << "  validation "
              << (r.validationEnabled ? std::to_string(r.validationMessages) + " messages"
                                      : std::string("NOT ENABLED (set OHAO_VALIDATION=1)"))
              << std::endl;

    // Counted after teardown: leaked objects are reported only at device destruction.
    const uint64_t validationTotal = host.shutdown();
    if (r.validationEnabled)
        std::cout << "validation including teardown: " << validationTotal << " messages" << std::endl;

    if (cfg.frameLimit > 0) {
        if (r.readbackCopies != 0) return 3;
        if (r.validationEnabled && validationTotal != 0) return 4;
    }
    return 0;
}
