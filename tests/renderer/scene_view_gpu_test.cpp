// scene_view_gpu_test (sub-project 3, step 8; test 20's GPU half).
//
// Each render uses a fresh renderer at 960x540. Two scenes, three renders each:
//   cornell_box (deferred, 10 frames, validation on) and model_viewer's DamagedHelmet
//   room (RT offline, 4 spp + 3 frames, denoiser off; validation off, see below):
//   1. the example's world, the camera and environment set by hand exactly as the
//      examples did before step 8;
//   2. the same world, its Camera and Environment applied by the scene host;
//   3. that world saved to scene text, loaded into a fresh World and applied the same way.
// All three images of a scene must match: bit-identical for the deferred Cornell box,
// and for the RT helmet room within the RT path's own rare 1-LSB flips (see rtSame). The
// Camera reproduces the hand-set pitch/yaw exactly; the file carries everything needed.
//
// The RT runs leave validation off: RT validation has known open findings (VUID 03004 /
// the NRI push-descriptor exit) that are not this test's subject. The deferred runs must
// report nothing. Exit code 0 = pass; the last line is a JSON summary.
#include "ecs/asset_resolver.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/scene_view.hpp"
#include "example_scenes.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "runtime/scene_view_apply.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace ohao;

namespace {

constexpr uint32_t W = 960, H = 540;

void setValidation(bool on) {
#ifdef _WIN32
    _putenv_s("OHAO_VALIDATION", on ? "1" : "0");
#else
    setenv("OHAO_VALIDATION", on ? "1" : "0", 1);
#endif
}

struct Shot {
    std::vector<uint8_t> pixels;
    uint64_t validation{0};
    bool ok{false};
    std::string note;
};

struct Setup {
    RenderMode mode;
    int frames;
    bool validation;
    std::function<bool(ecs::World&)> fill;               // builds or loads the world; false = failed
    std::function<void(VulkanRenderer&)> handView;       // null: apply the world's scene view
};

Shot shoot(const Setup& s, ecs::AssetResolver& assets) {
    setValidation(s.validation);
    Shot shot;
    VulkanRenderer renderer(W, H);
    if (!renderer.initialize()) {
        shot.note = "renderer init failed";
        return shot;
    }
    {
        ecs::World world;
        ecs::installRenderExtract(world, renderer.renderScene());
        ecs::installInstantiate(world, renderer.renderScene(), renderer.getTextureManager(), assets);
        if (!s.fill(world)) {
            shot.note = "the world could not be built or loaded";
            return shot;
        }
        if (s.handView) {
            s.handView(renderer);
        } else {
            const auto applied = runtime::applySceneView(renderer, ecs::resolveSceneView(world), assets);
            if (!applied.camera || !applied.environment || !applied.warnings.empty())
                shot.note += "the scene view was not fully applied; ";
        }
        renderer.setRenderMode(s.mode);
        if (s.mode != RenderMode::Deferred) renderer.setDenoiseMode(DenoiseMode::None);
        for (int i = 0; i < s.frames; ++i) {
            world.update(0.0f);
            if (i == s.frames - 1) renderer.requestReadback();
            renderer.render();
        }
        if (!ecs::instantiateErrors(world).empty()) shot.note += "instantiate reported errors; ";
        const auto span = renderer.readback();   // the last frame
        shot.pixels.assign(span.begin(), span.end());
    }
    shot.validation = renderer.validationMessageCount();
    shot.ok = !shot.pixels.empty() && shot.note.empty();
    return shot;
}

std::string saveText(const ecs::World& world, const reflect::TypeRegistry& types) {
    auto text = ecs::saveScene(world, types);
    return text ? text.value() : std::string();
}

struct Diff {
    std::size_t bytes{SIZE_MAX};   // differing bytes (SIZE_MAX: not comparable)
    int maxAbs{255};
};

Diff compare(const Shot& a, const Shot& b) {
    if (!a.ok || !b.ok || a.pixels.size() != b.pixels.size()) return {};
    Diff d{0, 0};
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const int delta = std::abs(int(a.pixels[i]) - int(b.pixels[i]));
        d.bytes += delta != 0;
        d.maxAbs = std::max(d.maxAbs, delta);
    }
    return d;
}

// RT offline is not perfectly deterministic: about one run in seven flips a single byte
// by 1 (measured while writing this test, renderer unchanged). A scene difference moves
// thousands of pixels on every run, so a handful of 1-LSB bytes is the renderer, not the
// scene view. Deferred comparisons stay exact.
bool rtSame(const Diff& d) { return d.bytes <= 16 && d.maxAbs <= 1; }

} // namespace

int main() {
    std::printf("scene_view_gpu_test (%ux%u)\n", W, H);
    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);

    // ---- cornell_box, deferred ----
    ecs::World cornellWorld;
    cornellWorld.seedIds(examples::kCornellBoxIdSeed);
    examples::buildCornellBox(cornellWorld);
    const std::string cornellText = saveText(cornellWorld, types);
    const auto buildCornell = [](ecs::World& w) {
        w.seedIds(examples::kCornellBoxIdSeed);
        examples::buildCornellBox(w);
        return true;
    };
    const auto loadCornell = [&](ecs::World& w) { return static_cast<bool>(ecs::loadScene(w, cornellText, types)); };
    const auto cornellByHand = [](VulkanRenderer& r) {   // cornell_box.cpp before step 8
        auto& camera = r.getCamera();
        camera.setPosition({0.0f, 0.0f, 13.0f});
        camera.setFov(38.0f);
        camera.setRotation(0.0f, -90.0f);
    };
    const Shot c1 = shoot({RenderMode::Deferred, 10, true, buildCornell, cornellByHand}, assets);
    const Shot c2 = shoot({RenderMode::Deferred, 10, true, buildCornell, nullptr}, assets);
    const Shot c3 = shoot({RenderMode::Deferred, 10, true, loadCornell, nullptr}, assets);

    // ---- model_viewer's helmet room, RT offline ----
    const ecs::AssetRef helmet{"assets/test_models/DamagedHelmet.glb", {}};
    auto model = assets.loadModel(helmet);
    std::string viewerText;
    Shot v1, v2, v3;
    if (model) {
        const FrameResult frame = SceneFramer::computeFraming(*model.value());
        const auto buildViewer = [&](ecs::World& w) {
            w.seedIds(examples::kModelViewerIdSeed);
            examples::buildModelViewerScene(w, *model.value(), frame, helmet, false);
            return true;
        };
        ecs::World viewerWorld;
        buildViewer(viewerWorld);
        viewerText = saveText(viewerWorld, types);
        const auto loadViewer = [&](ecs::World& w) { return static_cast<bool>(ecs::loadScene(w, viewerText, types)); };
        const std::string hdri = assets.absolutePath({"assets/test_models/env_studio.hdr", {}}).string();
        const auto viewerByHand = [&](VulkanRenderer& r) {   // model_viewer.cpp before step 8
            r.setEnvironmentMap(hdri);
            SceneFramer::applyCamera(r.getCamera(), frame);
        };
        v1 = shoot({RenderMode::RTOffline, 7, false, buildViewer, viewerByHand}, assets);
        v2 = shoot({RenderMode::RTOffline, 7, false, buildViewer, nullptr}, assets);
        v3 = shoot({RenderMode::RTOffline, 7, false, loadViewer, nullptr}, assets);
    }

    const Diff c12 = compare(c1, c2), c13 = compare(c1, c3);
    const Diff v12 = compare(v1, v2), v13 = compare(v1, v3);
    struct Check {
        const char* what;
        bool ok;
    };
    const Check checks[] = {
        {"the example scenes save to text", !cornellText.empty() && !viewerText.empty()},
        {"every render produced pixels", c1.ok && c2.ok && c3.ok && v1.ok && v2.ok && v3.ok},
        {"cornell: the Camera entity renders exactly as the hand-set camera", c12.bytes == 0},
        {"cornell: the saved and loaded scene renders identically", c13.bytes == 0},
        {"helmet room (RT): the Camera and Environment render as set by hand", rtSame(v12)},
        {"helmet room (RT): the saved and loaded scene renders identically", rtSame(v13)},
        {"validation reported nothing (the three deferred runs)",
         c1.validation == 0 && c2.validation == 0 && c3.validation == 0},
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
    for (const Shot* s : std::initializer_list<const Shot*>{&c1, &c2, &c3, &v1, &v2, &v3})
        if (!s->note.empty()) std::printf("  note: %s\n", s->note.c_str());
    std::printf("  differing bytes (max abs): cornell %zu (%d) / %zu (%d), helmet %zu (%d) / %zu (%d)\n", c12.bytes,
                c12.maxAbs, c13.bytes, c13.maxAbs, v12.bytes, v12.maxAbs, v13.bytes, v13.maxAbs);
    std::printf("  validation messages (deferred): %llu + %llu + %llu\n", static_cast<unsigned long long>(c1.validation),
                static_cast<unsigned long long>(c2.validation), static_cast<unsigned long long>(c3.validation));
    std::printf("{\"suite\":\"scene_view_gpu_test\",\"passed\":%d,\"failed\":%d,\"failures\":[%s]}\n", passed, failed,
                failures.c_str());
    return failed == 0 ? 0 : 1;
}
