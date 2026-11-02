// vfx_gpu_test: sub-project 12's test 32, transparency and lit particles, on the GPU through the CLI's
// stack (api::Session + RenderHost; the world's `vfx` records drawn by `vfx_draw`).
//
//   t32_sort_order            three overlapping BLEND panes (red, green, blue) and two alpha emitters,
//                             from four views: the transparent pass draws its objects back to front
//                             (VfxRenderer::objects), the nearest pane's colour wins where all three
//                             overlap, and two panes at the same distance draw in id order.
//   t32_lit_particles         a grey lit puff 0.5 m from a red lamp is redder than the same puff 5 m
//                             away (the per-particle light read back); unlit, its colour is its own.
//   t32_soft                  an additive puff crossing the floor: soft, no pixel along a column steps
//                             by more than 1/8 of the puff's largest addition; hard (soft 0), it steps.
//   t32_mask_rays             a MASK card cut away (opacity under the cutoff) lets the RT shadow and the
//                             path tracer light the floor behind it; a kept card shadows it.
//   t32_blend_no_shadow       a BLEND pane out of view between the sun and the floor changes no pixel
//                             of the hybrid frame (no RT shadow, no GI), within the BVH floor.
//   t32_capture_any_history   a capture at step N after 0, 7 and 200 frames rendered is the same image:
//                             effects are a function of the step.
//   t32_forward_parity        a plane shaded by the forward pass (BLEND, opacity 1) equals the deferred
//                             plane within 1 LSB, RT shadows, RT GI and post off.
//
// Exit code 0 = pass; the last line is a JSON summary. --keep leaves the PNGs in build/.
#include "common/materials.hpp"
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/vfx.hpp"
#include "ecs/vfx_draw.hpp"
#include "ecs/world.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "level/level_system.hpp"
#include "ohao/render_host.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/frame/pass_registry.hpp"
#include "render/scene/render_scene.hpp"
#include "runtime/image.hpp"
#include "stb_image.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <process.h>   // _getpid

using namespace ohao;

namespace {

bool g_keep = false;

// Every check counts (a failing one does not end its test): the summary line names each failure.
int g_passed = 0, g_failed = 0;
std::vector<std::string> g_failures;
const char* g_current = "";

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        return;
    }
    ++g_failed;
    g_failures.push_back(std::string(g_current) + ": " + what);
    std::printf("\n    FAIL: %s", what.c_str());
}

// --only NAME runs one test (a development aid: the suite's summary still counts the others as not run).
std::string g_only;

void run(const char* name, void (*fn)()) {
    if (!g_only.empty() && g_only != name) return;
    g_current = name;
    std::printf("  %s", name);
    fn();
    std::printf("\n");
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

// Per-process names under build/: CTest may run another copy of this test.
std::string scratch(const std::string& name) {
    return "build/vfx_gpu_" + name + "_" + std::to_string(static_cast<unsigned long>(_getpid()));
}

constexpr uint32_t kWidth = 320, kHeight = 180;
constexpr float kFov = 60.0f;

// The test's effects: a grey puff (lit and not), a soft and a hard additive puff, a few sparks.
const char* kSet = R"({
  "effects": {
    "puff": {"emitters": [{"count": 8, "life": [30, 30], "speed": [0, 0], "size": [0.3, 0.3],
              "color": [[0.5, 0.5, 0.5], [0.5, 0.5, 0.5]], "alpha": [0.6, 0.6], "blend": "alpha", "lit": true}]},
    "puff_unlit": {"emitters": [{"count": 8, "life": [30, 30], "speed": [0, 0], "size": [0.3, 0.3],
              "color": [[0.5, 0.5, 0.5], [0.5, 0.5, 0.5]], "alpha": [0.6, 0.6], "blend": "alpha"}]},
    "soft_puff": {"emitters": [{"count": 1, "life": [30, 30], "speed": [0, 0], "size": [2.0, 2.0],
              "color": [[0.6, 0.6, 0.6], [0.6, 0.6, 0.6]], "alpha": [1, 1], "blend": "additive", "soft": 1.0}]},
    "hard_puff": {"emitters": [{"count": 1, "life": [30, 30], "speed": [0, 0], "size": [2.0, 2.0],
              "color": [[0.6, 0.6, 0.6], [0.6, 0.6, 0.6]], "alpha": [1, 1], "blend": "additive"}]},
    "sparks": {"emitters": [{"count": 24, "life": [30, 30], "speed": [0.2, 0.6], "spread": 60, "size": [0.05, 0.05],
              "color": [[1, 0.8, 0.4], [1, 0.8, 0.4]], "alpha": [1, 1], "blend": "additive", "drag": 4}]},
    "cloud": {"emitters": [{"count": 1, "life": [9, 9], "speed": [0, 0], "size": [0.1, 0.1],
              "color": [[1, 1, 1], [1, 1, 1]], "alpha": [0, 0], "blend": "alpha"}],
              "proxy": {"spheres": 1, "radius": [2.0, 2.0], "density": 5.0, "rise": 0, "life": 9}}
  }
})";

std::string setPath() {
    static const std::string rel = [] {
        const std::string r = scratch("set") + ".vfx.json";
        std::ofstream(std::string(OHAO_SOURCE_DIR) + "/" + r, std::ios::binary | std::ios::trunc) << kSet;
        return r;
    }();
    return rel;
}

struct View { glm::vec3 position; float yawDeg; float pitchDeg; };

entt::entity camera(ecs::World& w, const View& v) {
    ecs::Transform t{v.position};
    t.rotation = glm::angleAxis(glm::radians(v.yawDeg), glm::vec3(0, 1, 0)) *
                 glm::angleAxis(glm::radians(v.pitchDeg), glm::vec3(1, 0, 0));
    const entt::entity c = w.create("MainCamera", t);
    w.registry().emplace<ecs::Camera>(c, ecs::Camera{kFov, 0.1f, 100.0f, true});
    return c;
}

void box(ecs::World& w, const char* name, glm::vec3 lo, glm::vec3 hi, ecs::Material m) {
    const entt::entity e = w.create(name, ecs::Transform{});
    w.registry().emplace<ecs::MeshSource>(e, ecs::MeshSource{ecs::BoxPrim{lo, hi, m.baseColor}});
    w.registry().emplace<ecs::Material>(e, m);
}

ecs::Material pane(glm::vec3 color, float opacity = 0.5f) {
    ecs::Material m = ohao::test::material(color, 0.8f, 0.0f, 0.0f);
    m.alphaMode = ecs::MaterialAlpha::Blend;
    m.geometryOpacity = opacity;
    return m;
}

void emitter(ecs::World& w, const char* name, const char* effect, glm::vec3 at, uint32_t seed) {
    const entt::entity e = w.create(name, ecs::Transform{at});
    w.registry().emplace<ecs::VfxEmitter>(e, ecs::VfxEmitter{effect, 0.0f, seed, true});
}

void sun(ecs::World& w, float intensity = 2.0f, float yawDeg = 0.0f) {
    ecs::Light l;
    l.type = ecs::LightType::Directional;
    l.intensity = intensity;
    ecs::Transform t{};
    t.rotation = glm::angleAxis(glm::radians(yawDeg), glm::vec3(0.0f, 1.0f, 0.0f)) *
                 glm::angleAxis(glm::radians(-60.0f), glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f)));
    w.registry().emplace<ecs::Light>(w.create("Sun", t), l);
}

void floorPlane(ecs::World& w) {
    const entt::entity f = w.create("Floor", ecs::Transform{});
    w.registry().emplace<ecs::MeshSource>(f, ecs::MeshSource{ecs::PlatformPrim{20.0f, 0.2f, 20.0f}});
    w.registry().emplace<ecs::Material>(f, ohao::test::material(glm::vec3(0.5f), 0.9f, 0.0f, 0.0f));
}

std::string save(ecs::World& w, const std::string& tag, const std::string& set = {}) {
    const entt::entity s = w.create("Effects", ecs::Transform{});
    w.registry().emplace<ecs::VfxSet>(s, ecs::VfxSet{ecs::AssetRef{set.empty() ? setPath() : set, {}}});
    const std::string rel = scratch(tag) + ".scene.json";
    auto text = ecs::saveScene(w, engineTypes(), ecs::SaveOptions{reflect::JsonLayout::Compact});
    if (!text) return {};
    std::ofstream(std::string(OHAO_SOURCE_DIR) + "/" + rel, std::ios::binary | std::ios::trunc) << text.value();
    return rel;
}

struct Frame {
    std::vector<uint8_t> rgba;   // kWidth x kHeight, top row first
    std::vector<VfxRenderer::Object> objects;
    std::vector<gpu::shared::VfxParticleGpu> particles;
    std::string vfxInfo;   // vfx.info after the steps
    std::vector<vfx::SmokeSphere> smoke;   // the smoke proxies the frame's RT traces read (after the steps)
    std::vector<std::string> passes;       // the last captured frame's passes, in order
    bool rtGI{false};                      // RT GI is wanted and available in that renderer
};

struct Options {
    int steps{2};
    const char* mode{"deferred"};
    int frames{1};
    std::vector<std::string> post{};   // none: the lit image tonemapped only
    int warmFrames{0};                 // frames rendered between a warm-up capture and the capture
    std::function<void(VulkanRenderer&)> settings{};
    std::function<void(ecs::World&)> afterLoad{};   // the scene loaded, before any step (a test places records)
};

// The scene loaded, stepped `steps` times and captured; the transparent pass's objects and particles.
Frame capture(const std::string& scenePath, const Options& o, const std::string& keepAs = {}) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    ecs::World world;
    ecs::installVfx(world, assets);
    // The CLI context's installs (ohao/tools/ohao/main.cpp): a Level expands into its blockout, and its bodies build.
    level::installLevels(world, assets, engineTypes());
    ecs::installPhysics(world, assets);
    api::Session session(world, engineTypes());
    session.setAssetResolver(&assets);
    host.bind(world);
    session.setHost(&host);
    Frame out;
    const std::string loaded = session.call("scene.load", R"({"replace":true,"path":")" + scenePath + "\"}");
    if (loaded.rfind("{\"result\"", 0) != 0) {
        std::printf("  scene.load %s: %s\n", scenePath.c_str(), loaded.substr(0, 300).c_str());
        return out;
    }
    if (o.afterLoad) o.afterLoad(world);
    if (o.steps > 0) {
        const std::string stepped = session.call("world.step", "{\"frames\":" + std::to_string(o.steps) + "}");
        if (stepped.rfind("{\"result\"", 0) != 0) std::printf("\n      world.step: %s", stepped.substr(0, 300).c_str());
    }
    out.vfxInfo = session.call("vfx.info", "{}");
    if (const ecs::VfxSystem* sys = ecs::vfxSystem(world))
        out.smoke = ecs::vfxFrameOf(world, std::make_shared<const vfx::VfxSet>(sys->set()), {}, {}).smoke;
    api::RenderRequest r;
    r.mode = "deferred";
    r.frames = 1;
    r.width = kWidth;
    r.height = kHeight;
    r.post = o.post;
    (void)host.capture(r);   // starts the renderer
    if (!host.renderer()) return out;
    if (o.settings) o.settings(*host.renderer());
    for (int f = 0; f < o.warmFrames; ++f) {   // frames as a window renders them: a sync, the frame
        world.sync();
        host.renderer()->render();
    }
    if (o.warmFrames > 0) host.renderer()->waitForSubmitted(host.renderer()->framesSubmitted());
    r.mode = o.mode;
    r.frames = o.frames;
    r.spp = 16;
    const std::string png = std::string(OHAO_SOURCE_DIR) + "/" + scratch("capture") + ".png";
    r.path = png;
    const api::RenderOutcome res = host.capture(r);
    if (!res.errors.empty()) {
        std::printf("  capture: %s\n", res.errors.front().message.c_str());
        return out;
    }
    if (auto* d = host.renderer()->getDeferredRenderer()) {
        out.rtGI = d->rtGIEnabled() && d->rtGIAvailable();
        if (auto* v = d->vfxRenderer()) {
            out.objects = v->objects();
            out.particles = v->readParticles();
        }
    }
    for (const auto& f : host.renderer()->frameTimings(1))
        for (uint32_t i = 0; i < f.count; ++i) out.passes.emplace_back(render::kPasses[f.pass[i]].name);
    int w = 0, h = 0, n = 0;
    if (unsigned char* px = stbi_load(png.c_str(), &w, &h, &n, 4)) {
        if (w == int(kWidth) && h == int(kHeight)) out.rgba.assign(px, px + std::size_t{kWidth} * kHeight * 4);
        stbi_image_free(px);
    }
    if (g_keep && !keepAs.empty())
        (void)runtime::writePng(std::string(OHAO_SOURCE_DIR) + "/" + scratch(keepAs) + ".png", out.rgba, kWidth, kHeight);
    std::remove(png.c_str());
    return out;
}

glm::ivec3 pixel(const Frame& f, uint32_t x, uint32_t y) {
    const std::size_t i = (std::size_t{y} * kWidth + x) * 4;
    return {f.rgba[i], f.rgba[i + 1], f.rgba[i + 2]};
}

struct Diff { int maxAbs{0}; std::size_t changed{0}; };

Diff diff(const Frame& a, const Frame& b) {
    Diff d;
    for (std::size_t i = 0; i + 3 < a.rgba.size() && i + 3 < b.rgba.size(); i += 4) {
        int m = 0;
        for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])));
        d.maxAbs = std::max(d.maxAbs, m);
        d.changed += m > 0;
    }
    return d;
}

bool sameSize(const Frame& a, const Frame& b) { return !a.rgba.empty() && a.rgba.size() == b.rgba.size(); }

// ---- t32_sort_order ---------------------------------------------------------------------------

const View kFront{{0.0f, 1.5f, 6.0f}, 0.0f, 0.0f};
const View kBack{{0.0f, 1.5f, -8.0f}, 180.0f, 0.0f};
const View kLeft{{-7.0f, 1.5f, -1.0f}, -90.0f, 0.0f};
const View kRight{{7.0f, 1.5f, -1.0f}, 90.0f, 0.0f};

std::string sortScene(const View& v, const std::string& tag) {
    ecs::World w;
    w.seedIds(32);
    sun(w);
    sun(w, 2.0f, 180.0f);   // the panes' far faces lit too: the back view sees their colours
    box(w, "PaneRed", {-0.6f, 1.0f, -0.02f}, {0.6f, 2.0f, 0.02f}, pane({1.0f, 0.0f, 0.0f}));
    box(w, "PaneGreen", {-0.6f, 1.0f, -1.02f}, {0.6f, 2.0f, -0.98f}, pane({0.0f, 1.0f, 0.0f}));
    box(w, "PaneBlue", {-0.6f, 1.0f, -2.02f}, {0.6f, 2.0f, -1.98f}, pane({0.0f, 0.0f, 1.0f}));
    // Two panes at the same distance from the front view (mirrored across its axis).
    box(w, "TieA", {-3.4f, 1.0f, -1.02f}, {-2.6f, 1.8f, -0.98f}, pane({0.5f, 0.5f, 0.0f}));
    box(w, "TieB", {2.6f, 1.0f, -1.02f}, {3.4f, 1.8f, -0.98f}, pane({0.0f, 0.5f, 0.5f}));
    emitter(w, "PuffNear", "puff", {1.5f, 0.6f, -0.5f}, 1);
    emitter(w, "PuffFar", "puff", {-1.5f, 0.6f, -1.5f}, 2);
    camera(w, v);
    return save(w, tag);
}

void t32_sort_order() {
    struct Case { const char* name; View view; };
    for (const Case& c : {Case{"front", kFront}, Case{"back", kBack}, Case{"left", kLeft}, Case{"right", kRight}}) {
        const Frame f = capture(sortScene(c.view, std::string("sort_") + c.name), Options{}, std::string("sort_") + c.name);
        check(!f.rgba.empty(), std::string(c.name) + ": captured");
        check(f.objects.size() == 7, std::string(c.name) + ": 5 panes and 2 emitter batches drawn (" +
                                         std::to_string(f.objects.size()) + ")");
        bool ordered = true;
        for (std::size_t i = 1; i < f.objects.size(); ++i) {
            const auto& a = f.objects[i - 1];
            const auto& b = f.objects[i];
            ordered &= a.distance > b.distance || (a.distance == b.distance && (a.kind < b.kind || (a.kind == b.kind && a.id < b.id)));
        }
        check(ordered, std::string(c.name) + ": back to front, ties by kind then id");
        if (c.name == std::string("front")) {
            // The tie: the mirrored panes at one distance, next to each other, the lower draw first.
            std::size_t ties = 0;
            for (std::size_t i = 1; i < f.objects.size(); ++i)
                if (f.objects[i - 1].kind == 1 && f.objects[i].kind == 1 && f.objects[i - 1].distance == f.objects[i].distance)
                    ties += f.objects[i - 1].id < f.objects[i].id;
            check(ties == 1, "front: the two panes at one distance draw in id order");
        }
        if (!f.rgba.empty() && (c.name == std::string("front") || c.name == std::string("back"))) {
            const glm::ivec3 p = pixel(f, kWidth / 2, kHeight / 2);   // where the three overlap
            const bool front = c.name == std::string("front");
            std::printf("\n      %s centre (%d, %d, %d)", c.name, p.r, p.g, p.b);
            check(front ? (p.r > p.g && p.r > p.b) : (p.b > p.g && p.b > p.r),
                  std::string(c.name) + ": the nearest pane's colour wins where all three overlap");
        }
    }
}

// ---- t32_lit_particles ------------------------------------------------------------------------

std::string litScene(const char* effect, const std::string& tag) {
    ecs::World w;
    w.seedIds(33);
    floorPlane(w);
    ecs::Light lamp;
    lamp.intensity = 4.0f;
    lamp.range = 12.0f;
    lamp.radius = 0.05f;
    lamp.color = {1.0f, 0.1f, 0.1f};
    w.registry().emplace<ecs::Light>(w.create("RedLamp", ecs::Transform{{0.0f, 1.5f, 0.0f}}), lamp);
    emitter(w, "Near", effect, {0.5f, 1.5f, 0.0f}, 1);
    emitter(w, "Far", effect, {5.0f, 1.5f, 0.0f}, 2);
    camera(w, {{2.5f, 2.0f, 7.0f}, 0.0f, -5.0f});
    return save(w, tag);
}

void t32_lit_particles() {
    const Frame lit = capture(litScene("puff", "lit"), Options{}, "lit");
    check(lit.particles.size() == 16, "16 particle slots (" + std::to_string(lit.particles.size()) + "); vfx.info " +
                                          lit.vfxInfo.substr(0, 400));
    glm::vec3 nearL(0.0f), farL(0.0f);
    int nearN = 0, farN = 0;
    for (const auto& p : lit.particles) {
        if (p.info[3] == 0) continue;
        const glm::vec3 l(p.light[0], p.light[1], p.light[2]);
        if (p.posSize[0] < 2.5f) { nearL += l; ++nearN; }
        else { farL += l; ++farN; }
    }
    check(nearN == 8 && farN == 8, "eight living particles at each emitter");
    if (nearN > 0 && farN > 0) {
        nearL /= float(nearN);
        farL /= float(farN);
        std::printf("\n      light near (%.3f %.3f %.3f) far (%.3f %.3f %.3f)", nearL.r, nearL.g, nearL.b, farL.r, farL.g,
                    farL.b);
        check(nearL.r - farL.r > 1.0f, "near the lamp: far more red light (I/d^2 at 0.5 m against 5 m)");
        check(nearL.r / std::max(nearL.g, 1e-4f) > farL.r / std::max(farL.g, 1e-4f), "and redder");
    }
    const Frame unlit = capture(litScene("puff_unlit", "unlit"), Options{});
    bool own = !unlit.particles.empty();
    for (const auto& p : unlit.particles)
        own &= p.info[3] == 0 || (std::abs(p.color[0] - 0.5f) < 1e-6f && std::abs(p.color[1] - 0.5f) < 1e-6f &&
                                  p.light[0] == 0.0f);
    check(own, "unlit: the colour is its own, no light gathered");
}

// ---- t32_soft ---------------------------------------------------------------------------------

std::string softScene(const char* effect, const std::string& tag) {
    ecs::World w;
    w.seedIds(34);
    floorPlane(w);
    sun(w, 1.0f);
    if (effect) emitter(w, "Puff", effect, {0.0f, 0.1f, 0.0f}, 3);   // half under the floor's top
    camera(w, {{0.0f, 2.0f, 3.0f}, 0.0f, -35.0f});
    return save(w, tag);
}

// Down the image's middle column of (with - without), from the puff's brightest pixel to the
// bottom: the largest step between neighbours and the largest value. That stretch crosses where the
// puff meets the floor (the puff's own round edge is above its peak).
std::pair<int, int> columnSteps(const Frame& with, const Frame& without) {
    std::vector<int> v(kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) v[y] = pixel(with, kWidth / 2, y).g - pixel(without, kWidth / 2, y).g;
    const auto peak = std::max_element(v.begin(), v.end());
    int maxStep = 0;
    for (auto it = peak + 1; it != v.end(); ++it) maxStep = std::max(maxStep, std::abs(*it - *(it - 1)));
    return {maxStep, *peak};
}

void t32_soft() {
    const Frame none = capture(softScene(nullptr, "soft_none"), Options{});
    const Frame soft = capture(softScene("soft_puff", "soft"), Options{}, "soft");
    const Frame hard = capture(softScene("hard_puff", "hard"), Options{}, "hard");
    if (!sameSize(soft, none) || !sameSize(hard, none)) {
        check(false, "soft: captured");
        return;
    }
    const auto [softStep, softMax] = columnSteps(soft, none);
    const auto [hardStep, hardMax] = columnSteps(hard, none);
    std::printf("\n      soft: step %d of %d; hard: step %d of %d", softStep, softMax, hardStep, hardMax);
    check(softMax > 16, "the puff adds light");
    check(softStep * 8 <= softMax, "soft: no step past 1/8 of the puff's light along the column");
    check(hardStep * 2 > hardMax, "hard (soft 0): it steps where it meets the floor");
}

// ---- t32_mask_rays and t32_blend_no_shadow ----------------------------------------------------

// A card between the sun (straight down) and the floor, under the camera's view or above it.
std::string cardScene(ecs::MaterialAlpha mode, float opacity, bool card, bool inView, const std::string& tag) {
    ecs::World w;
    w.seedIds(35);
    floorPlane(w);
    ecs::Light l;
    l.type = ecs::LightType::Directional;
    l.intensity = 3.0f;
    ecs::Transform t{};
    t.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));   // -Z to -Y: down
    w.registry().emplace<ecs::Light>(w.create("Sun", t), l);
    if (card) {
        ecs::Material m = ohao::test::material({0.8f, 0.8f, 0.8f}, 0.8f, 0.0f, 0.0f);
        m.alphaMode = mode;
        m.geometryOpacity = opacity;
        const float y = inView ? 1.0f : 6.0f;
        box(w, "Card", {-1.0f, y, -1.0f}, {1.0f, y + 0.05f, 1.0f}, m);
    }
    // Looking down at the floor under the card (the view's centre); the card at 6 m stays above it.
    camera(w, {{0.0f, 3.0f, 2.4f}, 0.0f, -50.0f});
    return save(w, tag);
}

void t32_mask_rays() {
    // The floor under the card: the image's lower middle.
    const auto underCard = [](const Frame& f) {
        int sum = 0;
        for (uint32_t y = kHeight / 2 - 2; y < kHeight / 2 + 2; ++y)
            for (uint32_t x = kWidth / 2 - 2; x < kWidth / 2 + 2; ++x) sum += pixel(f, x, y).g;
        return sum / 16;
    };
    for (const char* mode : {"deferred", "rt_offline"}) {
        Options o;
        o.mode = mode;
        o.frames = std::string(mode) == "deferred" ? 4 : 0;
        const Frame open = capture(cardScene(ecs::MaterialAlpha::Opaque, 1.0f, false, false, "mask_open"), o);
        const Frame cut = capture(cardScene(ecs::MaterialAlpha::Mask, 0.3f, true, false, "mask_cut"), o, std::string("mask_cut_") + mode);
        const Frame kept = capture(cardScene(ecs::MaterialAlpha::Mask, 0.8f, true, false, "mask_kept"), o, std::string("mask_kept_") + mode);
        if (open.rgba.empty() || cut.rgba.empty() || kept.rgba.empty()) {
            check(false, std::string(mode) + ": captured");
            continue;
        }
        const int a = underCard(open), b = underCard(cut), c = underCard(kept);
        std::printf("\n      %s: floor open %d, under a cut card %d, under a kept card %d", mode, a, b, c);
        check(std::abs(a - b) <= 2, std::string(mode) + ": a MASK card cut away lets the sun through");
        check(c * 2 < a, std::string(mode) + ": a kept MASK card shadows the floor");
    }
}

void t32_blend_no_shadow() {
    Options o;
    o.frames = 4;
    const Frame without = capture(cardScene(ecs::MaterialAlpha::Opaque, 1.0f, false, false, "blend_without"), o);
    const Frame with = capture(cardScene(ecs::MaterialAlpha::Blend, 0.5f, true, false, "blend_with"), o, "blend_with");
    const Frame opaque = capture(cardScene(ecs::MaterialAlpha::Opaque, 1.0f, true, false, "blend_opaque"), o);
    if (!sameSize(with, without) || !sameSize(opaque, without)) {
        check(false, "blend: captured");
        return;
    }
    const Diff d = diff(with, without);
    const Diff dOpaque = diff(opaque, without);
    std::printf("\n      a BLEND pane above: max %d over %zu px; the opaque card: max %d over %zu px", d.maxAbs, d.changed,
                dOpaque.maxAbs, dOpaque.changed);
    check(dOpaque.maxAbs > 32, "the same card opaque shadows the floor (the scene can show a shadow)");
    check(d.maxAbs <= 1 && d.changed <= kWidth * kHeight / 2000, "a BLEND pane casts no RT shadow and bounces no GI");
}

// ---- t32_capture_any_history ------------------------------------------------------------------

void t32_capture_any_history() {
    ecs::World w;
    w.seedIds(36);
    floorPlane(w);
    sun(w);
    emitter(w, "Puff", "puff", {0.0f, 1.0f, 0.0f}, 4);
    emitter(w, "Sparks", "sparks", {0.5f, 1.2f, 0.5f}, 5);
    camera(w, {{0.0f, 1.8f, 4.0f}, 0.0f, -10.0f});
    const std::string scene = save(w, "history");
    std::vector<Frame> frames;
    for (int warm : {0, 7, 200}) {
        Options o;
        o.steps = 30;
        o.warmFrames = warm;
        o.frames = 4;
        o.post = {"taa", "bloom"};
        frames.push_back(capture(scene, o, "history_" + std::to_string(warm)));
    }
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const bool ok = sameSize(frames[0], frames[i]);
        const Diff d = ok ? diff(frames[0], frames[i]) : Diff{255, 0};
        check(ok && d.maxAbs == 0, "the capture at step 30 after " + std::string(i == 1 ? "7" : "200") +
                                       " frames equals the one after 0 (max " + std::to_string(d.maxAbs) + ")");
    }
}

// ---- t32_forward_parity -----------------------------------------------------------------------

std::string parityScene(bool blend, const std::string& tag) {
    ecs::World w;
    w.seedIds(37);
    sun(w);
    ecs::Light lamp;
    lamp.intensity = 6.0f;
    lamp.range = 10.0f;
    lamp.radius = 0.05f;
    lamp.color = {1.0f, 0.7f, 0.4f};
    w.registry().emplace<ecs::Light>(w.create("Lamp", ecs::Transform{{1.0f, 1.0f, 1.0f}}), lamp);
    ecs::Material m = ohao::test::material({0.6f, 0.5f, 0.4f}, 0.4f, 0.0f, 0.0f);
    if (blend) {
        m.alphaMode = ecs::MaterialAlpha::Blend;
        m.geometryOpacity = 1.0f;
    }
    const entt::entity plane = w.create("Plane", ecs::Transform{});
    w.registry().emplace<ecs::MeshSource>(
        plane, ecs::MeshSource{ecs::QuadPrim{{-2.0f, 0.0f, 2.0f}, {2.0f, 0.0f, 2.0f}, {2.0f, 0.0f, -2.0f}, {-2.0f, 0.0f, -2.0f},
                                             {0.0f, 1.0f, 0.0f}, m.baseColor}});
    w.registry().emplace<ecs::Material>(plane, m);
    camera(w, {{0.0f, 2.5f, 3.5f}, 0.0f, -35.0f});
    return save(w, tag);
}

void t32_forward_parity() {
    Options o;
    o.settings = [](VulkanRenderer& r) {
        VulkanRenderer::RenderSettings s = r.renderSettings();
        s.rtDirect = false;
        s.rtGI = false;
        s.sss = false;
        r.setRenderSettings(s);
    };
    const Frame deferred = capture(parityScene(false, "parity_deferred"), o, "parity_deferred");
    const Frame forward = capture(parityScene(true, "parity_forward"), o, "parity_forward");
    if (!sameSize(deferred, forward)) {
        check(false, "parity: captured");
        return;
    }
    const Diff d = diff(deferred, forward);
    std::printf("\n      forward against deferred: max %d over %zu px", d.maxAbs, d.changed);
    check(d.maxAbs <= 1, "the forward-shaded plane equals the deferred one within 1 LSB");
}

} // namespace

// ---- t33_decal_footprint, t33_decal_wraps -----------------------------------------------------
// The decal records are placed where a test names (a fight would write them where it hits; here the
// spot is the test's). A frame with the decals against one with the decals pass switched off differs
// only where the decals project; a face within 60 degrees of the decal's normal takes it, one further
// off does not.

// The decal's pose for a record at `at` facing `normal` (as ecs/vfx's poseAlong: its -Z is the normal).
glm::mat4 decalPose(glm::vec3 at, glm::vec3 normal) {
    const glm::vec3 z = -glm::normalize(normal);
    const glm::vec3 helper = std::fabs(z.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 x = glm::normalize(glm::cross(helper, z));
    const glm::vec3 y = glm::cross(z, x);
    return glm::mat4(glm::vec4(x, 0.0f), glm::vec4(y, 0.0f), glm::vec4(z, 0.0f), glm::vec4(at, 1.0f));
}

// The decal tests' camera: 1.5 m up, looking down -Z, kFov high.
const glm::vec3 kEye(0.0f, 1.5f, 0.0f);

// A world point's pixel (top row first) through the test camera.
glm::vec2 screenOf(glm::vec3 world) {
    const glm::vec3 p = world - kEye;
    const float t = std::tan(glm::radians(kFov) * 0.5f), aspect = float(kWidth) / float(kHeight);
    const float depth = -p.z;
    return {(0.5f + 0.5f * p.x / (depth * t * aspect)) * kWidth, (0.5f - 0.5f * p.y / (depth * t)) * kHeight};
}

// The slice set's decal index by name (the set the play scene names).
int sliceDecal(const char* name) {
    std::ifstream in(std::string(OHAO_SOURCE_DIR) + "/assets/vfx/slice.vfx.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto set = ohao::vfx::parseVfxSet(text);
    return set ? set.value().decal(name) : -1;
}

void placeDecal(ecs::World& w, const char* decal, glm::vec3 at, glm::vec3 normal) {
    ecs::VfxRecord r;
    r.kind = ecs::VfxRecord::Kind::Decal;
    r.effect = static_cast<uint16_t>(sliceDecal(decal));
    r.seed = 1;
    r.pose = decalPose(at, normal);
    ecs::vfxSystem(w)->addRecordForTest(r);
}

// A box turned by `yawDeg` about Y: its front face (+Z) faces the camera `yawDeg` off the view.
void slant(ecs::World& w, const char* name, glm::vec3 centre, float yawDeg) {
    ecs::Transform t{centre};
    t.rotation = glm::angleAxis(glm::radians(yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
    const entt::entity e = w.create(name, t);
    w.registry().emplace<ecs::MeshSource>(e, ecs::MeshSource{ecs::BoxPrim{{-1.0f, -1.0f, -0.1f}, {1.0f, 1.0f, 0.1f}, glm::vec3(0.8f)}});
    w.registry().emplace<ecs::Material>(e, ohao::test::material(glm::vec3(0.8f), 0.9f, 0.0f, 0.0f));
}

// The centre of a `slant` box's front face.
glm::vec3 slantFace(glm::vec3 centre, float yawDeg) {
    return centre + glm::angleAxis(glm::radians(yawDeg), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec3(0.0f, 0.0f, 0.1f);
}

// A wall at z = -5.8 (its face), two slanted boxes in front of it (45 and 61 degrees), the camera at the origin.
std::string decalScene(const std::string& tag) {
    ecs::World w;
    w.seedIds(33);
    sun(w);
    box(w, "Wall", {-6.0f, 0.0f, -6.2f}, {6.0f, 4.0f, -5.8f}, ohao::test::material(glm::vec3(0.8f), 0.9f, 0.0f, 0.0f));
    slant(w, "SlantA", {-1.0f, 1.5f, -3.6f}, 45.0f);
    slant(w, "SlantB", {1.0f, 1.5f, -3.6f}, 61.0f);
    camera(w, View{{0.0f, 1.5f, 0.0f}, 0.0f, 0.0f});
    return save(w, tag, "assets/vfx/slice.vfx.json");   // the decals are the slice's
}

// Pixels that differ between two frames within `radius` of a world point's pixel.
int changedNear(const Frame& a, const Frame& b, glm::vec3 p, int radius) {
    const glm::vec2 c = screenOf(p);
    int n = 0;
    for (int y = int(c.y) - radius; y <= int(c.y) + radius; ++y)
        for (int x = int(c.x) - radius; x <= int(c.x) + radius; ++x)
            if (x >= 0 && y >= 0 && x < int(kWidth) && y < int(kHeight) && pixel(a, x, y) != pixel(b, x, y)) ++n;
    return n;
}

// The options of a decal frame: RT GI off (a dark decal would darken the light it bounces, outside its
// footprint, which is not what this checks), the decals pass on or off.
Options decalFrame(bool decals) {
    Options o;
    o.settings = [decals](VulkanRenderer& r) {
        if (auto* d = r.getDeferredRenderer()) {
            d->setRTGIEnabled(false);
            d->setDecalsDrawn(decals);
        }
    };
    return o;
}

void t33_decal_footprint() {
    const std::string scene = decalScene("decal_footprint");
    const glm::vec3 at(0.0f, 1.5f, -5.8f);   // on the wall's face, facing the camera
    Options on = decalFrame(true), off = decalFrame(false);
    on.afterLoad = [&](ecs::World& w) { placeDecal(w, "scorch", at, {0.0f, 0.0f, 1.0f}); };
    off.afterLoad = on.afterLoad;
    const Frame a = capture(scene, on, "decal_footprint_on");
    const Frame b = capture(scene, off, "decal_footprint_off");
    if (!sameSize(a, b)) {
        check(false, "both frames captured");
        return;
    }
    {
        const Frame again = capture(scene, on);   // the same decal frame again: the run-to-run floor
        std::size_t same = 0;
        for (std::size_t i = 0; i < a.rgba.size() && i < again.rgba.size(); ++i) same += a.rgba[i] != again.rgba[i];
        std::printf("\n      run to run: %zu bytes differ between two identical decal frames", same);
    }
    // The scorch: 1.6 m across on the face, its corners projected.
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const float sx : {-0.8f, 0.8f})
        for (const float sy : {-0.8f, 0.8f}) {
            const glm::vec2 s = screenOf(at + glm::vec3(sx, sy, 0.0f));
            x0 = std::min(x0, s.x), x1 = std::max(x1, s.x), y0 = std::min(y0, s.y), y1 = std::max(y1, s.y);
        }
    const float margin = 12.0f;
    int changed = 0, outside = 0;
    uint32_t ox0 = kWidth, ox1 = 0, oy0 = kHeight, oy1 = 0;
    int maxOutside = 0;
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
            if (pixel(a, x, y) != pixel(b, x, y)) {
                ++changed;
                if (x < x0 - margin || x > x1 + margin || y < y0 - margin || y > y1 + margin) {
                    ++outside;
                    ox0 = std::min(ox0, x), ox1 = std::max(ox1, x), oy0 = std::min(oy0, y), oy1 = std::max(oy1, y);
                    for (int c = 0; c < 3; ++c)
                        maxOutside = std::max(maxOutside, std::abs(int(a.rgba[(y * kWidth + x) * 4 + c]) - int(b.rgba[(y * kWidth + x) * 4 + c])));
                }
            }
    std::printf("\n      outside: x %u..%u, y %u..%u, largest difference %d levels", ox0, ox1, oy0, oy1, maxOutside);
    std::printf("\n      %d pixels differ; the scorch covers x %.0f..%.0f, y %.0f..%.0f; %d outside it", changed, x0, x1, y0, y1,
                outside);    check(changed > 50, "the decal shows on the wall");
    check(outside == 0, "nothing changes outside the decal's footprint");
}

void t33_decal_wraps() {
    const std::string scene = decalScene("decal_wraps");
    const glm::vec3 pA = slantFace({-1.0f, 1.5f, -3.6f}, 45.0f), pB = slantFace({1.0f, 1.5f, -3.6f}, 61.0f);
    Options on = decalFrame(true), off = decalFrame(false);
    on.afterLoad = [&](ecs::World& w) {
        placeDecal(w, "scorch", pA, {0.0f, 0.0f, 1.0f});
        placeDecal(w, "scorch", pB, {0.0f, 0.0f, 1.0f});
    };
    off.afterLoad = on.afterLoad;
    const Frame a = capture(scene, on, "decal_wraps_on");
    const Frame b = capture(scene, off, "decal_wraps_off");
    if (!sameSize(a, b)) {
        check(false, "both frames captured");
        return;
    }
    const int nA = changedNear(a, b, pA, 24), nB = changedNear(a, b, pB, 24);
    std::printf("\n      %d pixels changed on the 45-degree face (at %.0f, %.0f), %d on the 61-degree face (at %.0f, %.0f)", nA,
                screenOf(pA).x, screenOf(pA).y, nB, screenOf(pB).x, screenOf(pB).y);
    check(nA > 20, "a face 45 degrees off the decal's normal takes it");
    check(nB == 0, "a face 61 degrees off is refused (the 60-degree threshold)");
}

// ---- t34_doorway_dims_gi ----------------------------------------------------------------------
// smoke_doorway.scene.json: the blockout room with a smoke_cloud in its south doorway (engine axes: the
// doorway is at z = 3.1, the room's floor at y = 0). The Sun is taken out, so the floor is lit by RT GI
// alone; the view is from inside the room, looking out at the doorway and the floor 1-2 m inside it.

const View kDoorwayView{{1.5f, 1.6f, -0.8f}, 180.0f, -35.0f};

// A view's camera: yaw about Y, then pitch about X (camera()).
glm::quat viewRotation(const View& v) {
    return glm::angleAxis(glm::radians(v.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::angleAxis(glm::radians(v.pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));
}

// Where a pixel's ray meets the floor (y = 0) from a View; false when it does not reach it.
bool floorPoint(const View& v, uint32_t x, uint32_t y, glm::vec3& out) {
    const float t = std::tan(glm::radians(kFov) * 0.5f), aspect = float(kWidth) / float(kHeight);
    const glm::vec3 dir = glm::normalize(viewRotation(v) *
                                         glm::vec3((2.0f * (float(x) + 0.5f) / kWidth - 1.0f) * t * aspect,
                                                   (1.0f - 2.0f * (float(y) + 0.5f) / kHeight) * t, -1.0f));
    if (dir.y >= -1e-6f) return false;
    out = v.position + dir * (-v.position.y / dir.y);
    return true;
}

// The mean of the frame's red, green and blue over the pixels that see the floor patch
// (x 1..2 m, z 1..2 m), as a 0..255 level.
double patchMean(const Frame& f, const View& v) {
    double sum = 0.0;
    std::size_t n = 0;
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            glm::vec3 p;
            if (!floorPoint(v, x, y, p) || p.x < 1.0f || p.x > 2.0f || p.z < 1.0f || p.z > 2.0f) continue;
            const glm::ivec3 c = pixel(f, x, y);
            sum += (c.r + c.g + c.b) / 3.0;
            ++n;
        }
    return n ? sum / double(n) : 0.0;
}

// The doorway frame: the Sun out, RT GI on, the smoke in GI as given, `steps` steps, `frames` accumulated.
// The aperture (x 0.9..2.1, y 0..2.2 m at z 3.1) is filled in the harness, not the scene: the authored
// smoke_cloud (one record, spheres of radius 0.6..2.4 m offset up to 0.6 R) covers only part of it, and
// ruling 28 keeps the 20% bar and the effect as they are. Four more clouds sit at the aperture's corners.
Options doorwayFrame(bool smokeInGi, int steps, int frames) {
    Options o;
    o.steps = steps;
    o.frames = frames;
    o.afterLoad = [](ecs::World& w) {
        std::vector<entt::entity> suns;
        for (auto [e, l] : w.registry().view<const ecs::Light>().each()) suns.push_back(e);
        for (entt::entity e : suns) w.registry().remove<ecs::Light>(e);
        for (auto [e, c] : w.registry().view<const ecs::Camera>().each())
            w.registry().patch<ecs::Transform>(e, [](ecs::Transform& t) {
                t.position = kDoorwayView.position;
                t.rotation = viewRotation(kDoorwayView);
            });
        // Fixed ids: a smoke record's seed is its emitter's id, so a random id would move the cloud every run.
        const glm::vec3 corners[4] = {{0.9f, 0.6f, 3.1f}, {2.1f, 0.6f, 3.1f}, {0.9f, 1.6f, 3.1f}, {2.1f, 1.6f, 3.1f}};
        for (uint32_t i = 0; i < 4; ++i) {
            const entt::entity e = w.createWithId(0x5c0b10c0000000a0ull + i, "DoorwayFill", ecs::Transform{corners[i]});
            w.registry().emplace<ecs::VfxEmitter>(e, ecs::VfxEmitter{"smoke_cloud", 0.0f, 2u + i, true});
        }
    };
    o.settings = [smokeInGi](VulkanRenderer& r) {
        if (auto* d = r.getDeferredRenderer()) {
            d->setRTGIEnabled(true);
            d->setSmokeInGi(smokeInGi);
        }
    };
    return o;
}

void writeJson(const std::string& rel, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(OHAO_SOURCE_DIR) / "build" / "vfx");
    std::ofstream(std::string(OHAO_SOURCE_DIR) + "/" + rel, std::ios::binary | std::ios::trunc) << text;
}

void t34_doorway_dims_gi() {
    const std::string scene = "assets/scenes/smoke_doorway.scene.json";
    const Frame on = capture(scene, doorwayFrame(true, 60, 30), "smoke_doorway_on");
    const Frame off = capture(scene, doorwayFrame(false, 60, 30), "smoke_doorway_off");
    const Frame cleared = capture(scene, doorwayFrame(true, 600, 30), "smoke_doorway_cleared");   // 10 s: the cloud's life (9 s) is past
    if (!sameSize(on, off) || !sameSize(on, cleared)) {
        check(false, "the three doorway frames captured");
        return;
    }
    const double mOn = patchMean(on, kDoorwayView), mOff = patchMean(off, kDoorwayView), mClear = patchMean(cleared, kDoorwayView);
    const double dimming = 1.0 - mOn / mOff, clearError = std::fabs(mClear - mOff) / mOff;
    std::printf("\n      floor patch: %.2f with the cloud, %.2f without (%.1f%% darker); %.2f cleared (%.2f%% off the cloudless value)",
                mOn, mOff, 100.0 * dimming, mClear, 100.0 * clearError);
    std::printf("\n      smoke spheres in the frame at 60 steps: %zu", on.smoke.size());
    char json[512];
    std::snprintf(json, sizeof(json),
                  "{\"patchMeanCloud\": %.4f, \"patchMeanCloudless\": %.4f, \"patchMeanCleared\": %.4f, \"dimming\": %.4f, "
                  "\"clearedError\": %.6f, \"spheresAt60Steps\": %zu}\n",
                  mOn, mOff, mClear, dimming, clearError, on.smoke.size());
    writeJson("build/vfx/t34.json", json);
    check(!on.smoke.empty(), "the cloud publishes its spheres to the traces");
    check(dimming >= 0.20, "the cloud dims the floor's RT GI by at least 20%");
    check(clearError <= 0.01, "the floor is back within 1% of its cloudless value once the cloud clears");
}

// ---- t34_shadow_through_smoke -----------------------------------------------------------------
// The smoke proxies' transmittance on the CPU: a sun ray through a sphere the closed form gives, the
// product of exp(-density * chord) it crosses. The GPU's trace is checked by the frame (the floor under
// the cloud is darker than the floor beside it, shadow only: RT GI is off).

void t34_shadow_through_smoke() {
    // One cloud over the floor's origin, the sun from 60 degrees up, the camera straight down. The
    // floor point the cloud shadows is where the sun ray through the cloud's centre lands; the frame's
    // pixels there, against the same frame without the cloud, are the shadow's transmittance (RT GI
    // off, so no bounce reaches the floor). The optical depth is the closed
    // form's: the spheres the frame publishes, each crossed chord times its density.
    ecs::World w;
    w.seedIds(34);
    sun(w, 3.0f, 0.0f);
    floorPlane(w);
    const glm::vec3 cloudAt(0.0f, 2.0f, 0.0f);
    const entt::entity c = w.create("Cloud", ecs::Transform{cloudAt});
    w.registry().emplace<ecs::VfxEmitter>(c, ecs::VfxEmitter{"cloud", 0.0f, 7u, true});   // the test set's: radius 2, density 5
    const View top{{0.0f, 6.0f, 0.0f}, 0.0f, -89.0f};
    camera(w, top);
    const std::string scene = save(w, "shadow_through_smoke");
    Options o;
    o.steps = 60;
    o.frames = 30;
    o.settings = [](VulkanRenderer& r) {
        if (auto* d = r.getDeferredRenderer()) d->setRTGIEnabled(false);
    };
    const Frame with = capture(scene, o, "shadow_through_smoke");
    if (with.smoke.empty()) {
        check(false, "the cloud publishes its spheres");
        return;
    }
    // The sun's direction: the light's -Z (sun() turns it so), and the ray from the floor back to it.
    const glm::quat sunRot = glm::angleAxis(glm::radians(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                             glm::angleAxis(glm::radians(-60.0f), glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f)));
    const glm::vec3 toSun = -glm::normalize(sunRot * glm::vec3(0.0f, 0.0f, -1.0f));
    // The floor point the cloud's centre shadows, and the optical depth of the sun ray from it.
    const glm::vec3 c0 = with.smoke.front().centre;   // the one sphere the cloud has (offset from its origin)
    const glm::vec3 floorAt = c0 - toSun * (c0.y / toSun.y);
    double tau = 0.0;
    for (const vfx::SmokeSphere& s : with.smoke) {
        const glm::vec3 oc = floorAt - s.centre;
        const float b = glm::dot(oc, toSun), disc = b * b - (glm::dot(oc, oc) - s.radius * s.radius);
        if (disc <= 0.0f) continue;
        const float sq = std::sqrt(disc);
        tau += s.density * std::max(0.0f, -b + sq - std::max(-b - sq, 0.0f));
    }
    const double expected = std::exp(-tau);
    // The pixel of that floor point through the top view (the same projection as the frame's).
    const float t = std::tan(glm::radians(kFov) * 0.5f), aspect = float(kWidth) / float(kHeight);
    const glm::vec3 pc = glm::conjugate(viewRotation(top)) * (floorAt - top.position);
    const float px = (0.5f + 0.5f * pc.x / (-pc.z * t * aspect)) * kWidth;
    const float py = (0.5f - 0.5f * pc.y / (-pc.z * t)) * kHeight;
    double lit = 0.0, dark = 0.0;
    std::size_t n = 0;
    Options off = o;
    off.afterLoad = [](ecs::World& ww) {
        std::vector<entt::entity> clouds;
        for (auto [e, em] : ww.registry().view<const ecs::VfxEmitter>().each()) clouds.push_back(e);
        for (entt::entity e : clouds) ww.registry().remove<ecs::VfxEmitter>(e);
    };
    const Frame without = capture(scene, off, "shadow_through_smoke_off");
    if (!sameSize(with, without)) {
        check(false, "both frames captured");
        return;
    }
    n = 0;
    for (int y = int(py) - 3; y <= int(py) + 3; ++y)
        for (int x = int(px) - 3; x <= int(px) + 3; ++x) {
            if (x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight)) continue;
            const glm::ivec3 a = pixel(with, x, y), b = pixel(without, x, y);
            lit += (a.r + a.g + a.b) / 3.0;
            dark += (b.r + b.g + b.b) / 3.0;
            ++n;
        }
    const double ratio = n && dark > 0.0 ? lit / dark : 1.0;
    std::printf("\n      optical depth %.3f: closed-form transmittance %.4f; the floor's pixels with the cloud at %.1f%% of "
                "those without (display values, tonemapped: not the linear ratio)",
                tau, expected, 100.0 * ratio);
    check(tau > 0.0, "the sun ray crosses the cloud's spheres (closed form)");
    check(ratio < 0.99, "the cloud shadows the floor under it");
}

// The doorway's floor with RT GI on against off (the smoke in GI as it is): the GI reaches the patch at all,
// so t34_doorway_dims_gi measures the bounce light and not the ambient or the environment's direct term.
void t34_gi_reaches_floor() {
    const std::string scene = "assets/scenes/smoke_doorway.scene.json";
    Options noGi = doorwayFrame(true, 60, 30);
    noGi.settings = [](VulkanRenderer& r) {
        if (auto* d = r.getDeferredRenderer()) d->setRTGIEnabled(false);
    };
    const Frame on = capture(scene, doorwayFrame(true, 60, 30), "gi_reaches_floor_on");
    const Frame off = capture(scene, noGi, "gi_reaches_floor_off");
    if (!sameSize(on, off)) {
        check(false, "both frames captured");
        return;
    }
    const double mOn = patchMean(on, kDoorwayView), mOff = patchMean(off, kDoorwayView);
    std::printf("\n      floor patch: %.2f with RT GI, %.2f without", mOn, mOff);
    std::printf("\n      rtGI wanted+available: %d / %d; passes with rt.gi: %d / %d (%zu passes)", on.rtGI, off.rtGI,
                int(std::count(on.passes.begin(), on.passes.end(), "rt.gi")), int(std::count(off.passes.begin(), off.passes.end(), "rt.gi")),
                on.passes.size());
    check(std::fabs(mOn - mOff) > 0.01 * mOff, "RT GI changes the floor the cloud is measured on");
}

// ---- t34_gi_reads_smoke -----------------------------------------------------------------------
// RT GI only (no sun, the environment lights the floor through the sky), a dense cloud over the floor's
// middle: the frame's mean with smoke in GI on against off. The cloud is invisible (its particles'
// alpha is 0), so the difference is the traces' alone.
void t34_gi_reads_smoke() {
    ecs::World w;
    w.seedIds(34);
    const entt::entity env = w.create("Environment", ecs::Transform{});
    w.registry().emplace<ecs::Environment>(env, ecs::Environment{ecs::AssetRef{"assets/test_models/env_outdoor.hdr", {}}, 1.0f});
    floorPlane(w);
    emitter(w, "Cloud", "cloud", glm::vec3(0.0f, 1.2f, 0.0f), 7u);
    camera(w, View{{0.0f, 6.0f, 0.0f}, 0.0f, -89.0f});
    const std::string scene = save(w, "probe_gi_cloud");
    double means[2] = {0.0, 0.0};
    std::size_t spheres = 0;
    for (int on = 0; on < 2; ++on) {
        Options o;
        o.steps = 60;
        o.frames = 30;
        o.settings = [on](VulkanRenderer& r) {
            if (auto* d = r.getDeferredRenderer()) {
                d->setRTGIEnabled(true);
                d->setSmokeInGi(on == 1);
            }
        };
        const Frame f = capture(scene, o, "");
        spheres = f.smoke.size();
        double sum = 0.0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x) {
                const glm::ivec3 c = pixel(f, x, y);
                sum += (c.r + c.g + c.b) / 3.0;
            }
        means[on] = sum / double(kWidth * kHeight);
    }
    const double dimming = 1.0 - means[1] / means[0];
    std::printf("\n      frame mean %.3f with smoke in GI, %.3f without (%.2f%% darker; spheres %zu)", means[1], means[0],
                100.0 * dimming, spheres);
    check(spheres > 0, "the cloud publishes its sphere to the traces");
    check(dimming > 0.01, "smoke in GI darkens the frame's RT GI by more than 1%");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--keep") == 0) g_keep = true;
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) g_only = argv[++i];
    run("t32_sort_order", t32_sort_order);
    run("t32_lit_particles", t32_lit_particles);
    run("t32_soft", t32_soft);
    run("t32_mask_rays", t32_mask_rays);
    run("t32_blend_no_shadow", t32_blend_no_shadow);
    run("t32_capture_any_history", t32_capture_any_history);
    run("t32_forward_parity", t32_forward_parity);
    run("t33_decal_footprint", t33_decal_footprint);
    run("t33_decal_wraps", t33_decal_wraps);
    run("t34_gi_reaches_floor", t34_gi_reaches_floor);
    run("t34_doorway_dims_gi", t34_doorway_dims_gi);
    run("t34_shadow_through_smoke", t34_shadow_through_smoke);
    run("t34_gi_reads_smoke", t34_gi_reads_smoke);
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + setPath()).c_str());
    return ohao::test::finish("vfx_gpu_test", g_passed, g_failed, g_failures);
}
