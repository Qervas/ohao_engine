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
//                             plane within 1 LSB, RT shadows, RT GI, SSR, SSAO and post off.
//
// Exit code 0 = pass; the last line is a JSON summary. --keep leaves the PNGs in build/.
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/vfx.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/scene/render_scene.hpp"
#include "runtime/image.hpp"
#include "stb_image.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
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

void run(const char* name, void (*fn)()) {
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
              "color": [[1, 0.8, 0.4], [1, 0.8, 0.4]], "alpha": [1, 1], "blend": "additive", "drag": 4}]}
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

void box(ecs::World& w, const char* name, glm::vec3 lo, glm::vec3 hi, ecs::MaterialOverride m) {
    const entt::entity e = w.create(name, ecs::Transform{});
    w.registry().emplace<ecs::MeshSource>(e, ecs::MeshSource{ecs::BoxPrim{lo, hi, m.baseColor}});
    w.registry().emplace<ecs::MaterialOverride>(e, m);
}

ecs::MaterialOverride pane(glm::vec3 color, float opacity = 0.5f) {
    ecs::MaterialOverride m{color, 0.8f, 0.0f, 0.0f};
    m.alphaMode = ecs::MaterialAlpha::Blend;
    m.opacity = opacity;
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
    w.registry().emplace<ecs::MaterialOverride>(f, ecs::MaterialOverride{glm::vec3(0.5f), 0.9f, 0.0f, 0.0f});
}

std::string save(ecs::World& w, const std::string& tag) {
    const entt::entity s = w.create("Effects", ecs::Transform{});
    w.registry().emplace<ecs::VfxSet>(s, ecs::VfxSet{ecs::AssetRef{setPath(), {}}});
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
};

struct Options {
    int steps{2};
    const char* mode{"deferred"};
    int frames{1};
    std::vector<std::string> post{};   // none: the lit image tonemapped only
    int warmFrames{0};                 // frames rendered between a warm-up capture and the capture
    std::function<void(VulkanRenderer&)> settings{};
};

// The scene loaded, stepped `steps` times and captured; the transparent pass's objects and particles.
Frame capture(const std::string& scenePath, const Options& o, const std::string& keepAs = {}) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    ecs::World world;
    ecs::installVfx(world, assets);
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
    if (o.steps > 0) {
        const std::string stepped = session.call("world.step", "{\"frames\":" + std::to_string(o.steps) + "}");
        if (stepped.rfind("{\"result\"", 0) != 0) std::printf("\n      world.step: %s", stepped.substr(0, 300).c_str());
    }
    out.vfxInfo = session.call("vfx.info", "{}");
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
    if (auto* d = host.renderer()->getDeferredRenderer())
        if (auto* v = d->vfxRenderer()) {
            out.objects = v->objects();
            out.particles = v->readParticles();
        }
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
        ecs::MaterialOverride m{{0.8f, 0.8f, 0.8f}, 0.8f, 0.0f, 0.0f};
        m.alphaMode = mode;
        m.opacity = opacity;
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
    ecs::MaterialOverride m{{0.6f, 0.5f, 0.4f}, 0.4f, 0.0f, 0.0f};
    if (blend) {
        m.alphaMode = ecs::MaterialAlpha::Blend;
        m.opacity = 1.0f;
    }
    const entt::entity plane = w.create("Plane", ecs::Transform{});
    w.registry().emplace<ecs::MeshSource>(
        plane, ecs::MeshSource{ecs::QuadPrim{{-2.0f, 0.0f, 2.0f}, {2.0f, 0.0f, 2.0f}, {2.0f, 0.0f, -2.0f}, {-2.0f, 0.0f, -2.0f},
                                             {0.0f, 1.0f, 0.0f}, m.baseColor}});
    w.registry().emplace<ecs::MaterialOverride>(plane, m);
    camera(w, {{0.0f, 2.5f, 3.5f}, 0.0f, -35.0f});
    return save(w, tag);
}

void t32_forward_parity() {
    Options o;
    o.settings = [](VulkanRenderer& r) {
        VulkanRenderer::RenderSettings s = r.renderSettings();
        s.rtShadows = false;
        s.rtGI = false;
        s.ssr = false;
        s.ssao = false;
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

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--keep") == 0) g_keep = true;
    run("t32_sort_order", t32_sort_order);
    run("t32_lit_particles", t32_lit_particles);
    run("t32_soft", t32_soft);
    run("t32_mask_rays", t32_mask_rays);
    run("t32_blend_no_shadow", t32_blend_no_shadow);
    run("t32_capture_any_history", t32_capture_any_history);
    run("t32_forward_parity", t32_forward_parity);
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + setPath()).c_str());
    return ohao::test::finish("vfx_gpu_test", g_passed, g_failed, g_failures);
}
