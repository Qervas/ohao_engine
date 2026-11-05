// exposure_gpu_test: a window's auto exposure (frontier F2 T2, postprocess/exposure.slang), headless.
//
//   meters     Cornell metered with auto exposure: the EV100 puts the frame's log-average luminance
//              (between the 50th and 95th percentiles) at middle grey;
//   doubling   every light at twice its intensity (a closed box: every pixel twice as bright) meters
//              exactly one EV more, and a compensation of +1 EV one EV less again;
//   captures   a capture after metering is the capture of a renderer that never metered, byte for
//              byte: captures are at a fixed EV100.
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "ohao/render_host.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace ohao;

namespace {

int g_passed = 0, g_failed = 0;
std::vector<std::string> g_failures;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        std::printf("  ok    %s\n", what.c_str());
    } else {
        ++g_failed;
        g_failures.push_back(what);
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

// Per-process names under build/: CTest may run another copy of this test.
std::string scratch(const std::string& name) {
    return std::string(OHAO_SOURCE_DIR) + "/build/exposure_" + name + "_" + std::to_string(ohao::test::processId()) + ".png";
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct Host {
    ecs::FileAssetResolver assets{OHAO_SOURCE_DIR};
    host::RenderHost host{assets, api::HostKind::Headless};
    ecs::World world;
    api::Session session{world, engineTypes()};
    Host() {
        session.setAssetResolver(&assets);
        host.bind(world);
        session.setHost(&host);
    }
    bool load(const char* scene) {
        return session.call("scene.load", std::string(R"({"replace":true,"path":")") + scene + "\"}").rfind(
                   "{\"result\"", 0) == 0;
    }
    // A capture of the deferred frame, as `ohao run` takes it; its PNG's bytes.
    std::string capture(const std::string& path) {
        api::RenderRequest r;
        r.mode = "deferred";
        r.frames = 1;
        r.width = 640;
        r.height = 360;
        r.path = path;
        const api::RenderOutcome o = host.capture(r);
        if (!o.errors.empty())
            std::printf("  capture: %s: %s\n", o.errors.front().code.c_str(), o.errors.front().message.c_str());
        return o.errors.empty() ? readFile(path) : std::string{};
    }
    // `n` window-style frames (a sync, then the frame), a second apart for the adaptation, then
    // the metered EV100 of the last.
    std::optional<float> meter(int n) {
        VulkanRenderer* renderer = host.renderer();
        renderer->setFrameDeltaTime(0.1f);   // 1 - e^-0.3 a frame toward the target at 3/s: converged in 120
        for (int i = 0; i < n; ++i) {
            world.sync();
            renderer->render();
        }
        renderer->waitForSubmitted(renderer->framesSubmitted());
        return renderer->meteredEv100();
    }
};

void autoExposureMetersAndDoubles() {
    Host h;
    if (!h.load("assets/scenes/cornell_box.scene.json")) {
        check(false, "Cornell loads");
        return;
    }
    const std::string fixedBefore = h.capture(scratch("fixed_before"));   // starts the renderer
    VulkanRenderer* renderer = h.host.renderer();
    if (!renderer || fixedBefore.empty()) {
        check(false, "the renderer starts and captures");
        return;
    }
    VulkanRenderer::RenderSettings s = renderer->renderSettings();
    check(!s.autoExposure, "a fresh renderer does not meter");
    s.autoExposure = true;
    renderer->setRenderSettings(s);

    const std::optional<float> ev1 = h.meter(300);   // settled, as below
    check(ev1.has_value() && std::isfinite(*ev1), "Cornell is metered");
    if (ev1) std::printf("  Cornell meters at EV100 %.4f\n", *ev1);

    auto& reg = h.world.registry();
    for (auto e : reg.view<ecs::Light>()) reg.patch<ecs::Light>(e, [](ecs::Light& l) { l.intensity *= 2.0f; });
    // 300 frames: the metering converges in 120, RT GI's bounces through its radiance cache (frontier
    // F5a) a bounce at a time, and the identities below hold only for the settled frame.
    const std::optional<float> ev2 = h.meter(300);
    if (ev1 && ev2) std::printf("  every light doubled: EV100 %.4f (%+.4f)\n", *ev2, *ev2 - *ev1);
    check(ev1 && ev2 && std::abs(*ev2 - *ev1 - 1.0f) < 0.01f, "every light doubled: one EV more, within 0.01");

    s.exposureCompensation = 1.0f;
    renderer->setRenderSettings(s);
    const std::optional<float> ev3 = h.meter(300);
    if (ev2 && ev3) std::printf("  +1 EV compensation: EV100 %.4f (%+.4f)\n", *ev3, *ev3 - *ev2);
    check(ev2 && ev3 && std::abs(*ev3 - *ev2 + 1.0f) < 0.01f, "a compensation of +1 EV meters one EV less");

    // Back to the scene's lights: a capture now is the one taken before any metering.
    for (auto e : reg.view<ecs::Light>()) reg.patch<ecs::Light>(e, [](ecs::Light& l) { l.intensity *= 0.5f; });
    const std::string fixedAfter = h.capture(scratch("fixed_after"));
    check(!fixedAfter.empty() && fixedAfter == fixedBefore,
          "a capture after metering equals the capture before it, byte for byte (captures are at a fixed EV100)");
    check(!renderer->meteredEv100(), "a capture leaves the renderer at a fixed EV100");
    std::remove(scratch("fixed_before").c_str());
    std::remove(scratch("fixed_after").c_str());
}

} // namespace

int main() {
    autoExposureMetersAndDoubles();
    return test::finish("exposure_gpu_test", g_passed, g_failed, g_failures);
}
