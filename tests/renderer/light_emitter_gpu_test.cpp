// light_emitter_gpu_test: a point, spot or area light is also a mesh the camera sees (RenderScene's
// emitter instances), and that mesh changes nothing but its own pixels.
//
//   emitters        a point, a spot and an area light get one emitter instance each, a sun none;
//                   turning the emitters off removes them, the lights stay.
//   out of view     the lights above a camera looking down at a floor, a box and a wall: the
//                   image with emitters is the image without them (deferred with RT shadows and
//                   GI, deferred with CSM), within the driver's BVH floor of 1 LSB on a few
//                   pixels: no emitter shadows its light or another, no GI ray adds a light twice,
//                   no cascade draws one. The path tracer's bounce rays meet emitters and weigh
//                   them against light sampling (MIS), so its samples change everywhere and its
//                   light does not: the mean, overall within 1% and per 32x32 block within 5%.
//   in view         the same room seen with its lights: the point's and the area light's emitters'
//                   centre pixels are bright, the downward spot's (the camera outside its cone) dark;
//                   past an emitter's footprint the same light (the mean overall within 1%, per
//                   32x32 block within 5%): RT direct light's reuse reads its neighbours' samples
//                   within 30 px and its denoiser and RT GI's take the mesh's depth among a pixel's
//                   neighbours, so the samples change there and the light does not (as the path
//                   tracer's do everywhere).
//   spot            the spot turned toward the camera: from inside its cone its emitter is bright.
//   motion          a moved point and area light: their emitters' prevWorld is last frame's
//                   placement (the motion vector TAA reprojects with); a frame later, the world.
//   taa             a point light moving ~1.5 px a frame under TAA: its mesh is as bright as the
//                   same light held still (it was 4.9 darker, its history the background).
//   gun             a first-person gun (ViewModel, sub-project 9 test 21) changes no light past 24 px of
//                   its footprint (the same light, as above): no CSM cascade, shadow or GI ray sees it;
//                   the same box placed in the world at its drawn pose does shadow the room.
//
// Exit code 0 = pass; the last line is a JSON summary. --keep leaves the PNGs in build/.
#include "common/materials.hpp"
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/view_model.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/scene/render_scene.hpp"
#include "runtime/image.hpp"
#include "scene/asset/model_loader.hpp"
#include "stb_image.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>   // glm::rotation

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
int g_passed = 0, g_failed = 0;
std::vector<std::string> g_failures;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
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
    return "build/light_emitter_" + name + "_" + std::to_string(static_cast<unsigned long>(_getpid()));
}

constexpr uint32_t kWidth = 480, kHeight = 270;
constexpr float kFov = 50.0f;

// The room's lights, world space (no rotation but the spot's).
const glm::vec3 kPointPos{-3.0f, 3.0f, -2.0f};
constexpr float kPointRadius = 0.25f;
const glm::vec3 kSpotPos{3.0f, 4.0f, -1.0f};
constexpr float kSpotRadius = 0.2f;
const glm::vec3 kAreaPos{-1.0f, 6.0f, -3.0f};
const glm::vec3 kAreaEdge1{2.0f, 0.0f, 0.0f}, kAreaEdge2{0.0f, 0.0f, 1.5f};   // facing down

struct View { glm::vec3 position; float pitchDeg; };
const View kLightsInView{{0.0f, 3.0f, 9.0f}, -8.0f};
const View kLightsOutOfView{{0.0f, 2.5f, 6.0f}, -50.0f};

// A floor, a back wall and a box, lit by a point, a spot and an area light and a sun, seen from
// `view`: saved as a scene file (relative to the source root, where the asset resolver looks).
std::string roomScene(const View& view, const std::string& tag, bool spotAtCamera = false,
                      const std::function<void(ecs::World&, entt::entity camera)>& extra = {}) {
    using namespace ecs;
    World w;
    w.seedIds(11);
    const Material rough = ohao::test::material({0.6f, 0.6f, 0.6f}, 0.9f, 0.0f, 0.0f);
    const auto floor = w.create("Floor", Transform{});
    w.registry().emplace<MeshSource>(floor, MeshSource{PlatformPrim{20.0f, 0.2f, 20.0f}});
    w.registry().emplace<Material>(floor, rough);
    const auto wall = w.create("Wall", Transform{});
    w.registry().emplace<MeshSource>(wall, MeshSource{BoxPrim{{-10.0f, 0.0f, -6.0f}, {10.0f, 8.0f, -5.8f}, glm::vec3(0.6f)}});
    w.registry().emplace<Material>(wall, rough);
    const auto box = w.create("Box", Transform{});
    w.registry().emplace<MeshSource>(box, MeshSource{BoxPrim{{-0.8f, 0.1f, -1.5f}, {0.8f, 1.7f, 0.1f}, glm::vec3(0.6f)}});
    w.registry().emplace<Material>(box, rough);

    const auto light = [&](const char* name, const Transform& t, const Light& l) {
        w.registry().emplace<Light>(w.create(name, t), l);
    };
    Light point;
    point.intensity = 5.0f;
    point.range = 15.0f;
    point.radius = kPointRadius;
    point.color = {1.0f, 0.8f, 0.6f};
    light("Point", Transform{kPointPos}, point);
    Light spot;
    spot.type = LightType::Spot;
    spot.intensity = 8.0f;
    spot.range = 15.0f;
    spot.radius = kSpotRadius;
    spot.innerConeDeg = 25.0f;
    spot.outerConeDeg = 40.0f;
    spot.color = {0.6f, 0.8f, 1.0f};
    Transform spotT{kSpotPos};
    spotT.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));   // -Z to -Y: down
    if (spotAtCamera)   // -Z turned toward the camera: it looks into the cone
        spotT.rotation = glm::rotation(glm::vec3(0.0f, 0.0f, -1.0f), glm::normalize(view.position - kSpotPos));
    light("Spot", spotT, spot);
    Light area;
    area.type = LightType::AreaRect;
    area.intensity = 10.0f;
    area.range = 15.0f;
    area.radius = 0.5f;
    area.areaEdge1 = kAreaEdge1;
    area.areaEdge2 = kAreaEdge2;
    light("Area", Transform{kAreaPos}, area);
    Light sun;
    sun.type = LightType::Directional;
    sun.intensity = 0.4f;
    Transform sunT{};
    sunT.rotation = glm::angleAxis(glm::radians(-60.0f), glm::normalize(glm::vec3(1.0f, 0.3f, 0.0f)));
    light("Sun", sunT, sun);

    Transform cam{view.position};
    cam.rotation = glm::angleAxis(glm::radians(view.pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));
    const entt::entity camera = w.create("MainCamera", cam);
    w.registry().emplace<ecs::Camera>(camera, ecs::Camera{kFov, 0.1f, 200.0f, true});
    if (extra) extra(w, camera);

    const std::string rel = scratch(tag) + ".scene.json";
    auto text = saveScene(w, engineTypes(), SaveOptions{reflect::JsonLayout::Compact});
    if (!text) return {};
    std::ofstream(std::string(OHAO_SOURCE_DIR) + "/" + rel, std::ios::binary | std::ios::trunc) << text.value();
    return rel;
}

struct Image {
    std::vector<uint8_t> rgba;   // kWidth x kHeight, top row first
    std::size_t emitters{0};     // the RenderScene's emitter instances during the capture
};

enum class Mode { Deferred, DeferredCsm, PathTraced };

// The scene as `ohao run` captures it, emitters on or off (switched after a warm-up capture that
// starts the renderer). `viewModel`: the first-person gun's system installed, as the CLI context does.
Image capture(const std::string& scenePath, Mode mode, bool emitters, bool viewModel = false) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    Image out;
    ecs::World world;
    if (viewModel) {
        ecs::installViewModel(world);
        ecs::installAnimation(world, assets);   // the skinned arms (sub-project 10 step 3) are posed
    }
    api::Session session(world, engineTypes());
    session.setAssetResolver(&assets);
    host.bind(world);
    session.setHost(&host);
    const std::string loaded = session.call("scene.load", std::string(R"({"replace":true,"path":")") + scenePath + "\"}");
    if (loaded.rfind("{\"result\"", 0) != 0) {
        std::printf("  scene.load %s: %s\n", scenePath.c_str(), loaded.c_str());
        return out;
    }
    api::RenderRequest r;
    r.mode = "deferred";
    r.frames = 1;
    r.width = kWidth;
    r.height = kHeight;
    (void)host.capture(r);
    if (!host.renderer()) return out;
    host.renderer()->renderScene().setLightEmitters(emitters);
    if (mode == Mode::DeferredCsm) {
        VulkanRenderer::RenderSettings s = host.renderer()->renderSettings();
        s.rtDirect = false;
        host.renderer()->setRenderSettings(s);
    }
    r.mode = mode == Mode::PathTraced ? "rt_offline" : "deferred";
    r.frames = mode == Mode::PathTraced ? 0 : 10;
    r.spp = 16;
    const std::string png = std::string(OHAO_SOURCE_DIR) + "/" + scratch("capture") + ".png";
    r.path = png;
    const api::RenderOutcome o = host.capture(r);
    out.emitters = host.renderer()->renderScene().lightEmitterSlots().size();
    if (!o.errors.empty()) return out;
    int w = 0, h = 0, n = 0;
    if (unsigned char* px = stbi_load(png.c_str(), &w, &h, &n, 4)) {
        if (w == int(kWidth) && h == int(kHeight)) out.rgba.assign(px, px + std::size_t{kWidth} * kHeight * 4);
        stbi_image_free(px);
    }
    std::remove(png.c_str());
    return out;
}

void keep(const Image& im, const std::string& name) {
    if (g_keep && !im.rgba.empty())
        (void)runtime::writePng(std::string(OHAO_SOURCE_DIR) + "/" + scratch(name) + ".png", im.rgba, kWidth, kHeight);
}

// The emitters' pixel footprints seen from `view`: a sphere's projected disc, the area light's
// projected corners' bounding box, each grown by `margin` pixels.
struct Footprint { float x, y, r; float x0, y0, x1, y1; bool box; };

glm::vec3 project(const View& view, const glm::vec3& p) {
    const glm::mat4 camWorld = glm::translate(glm::mat4(1.0f), view.position) *
        glm::mat4_cast(glm::angleAxis(glm::radians(view.pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::mat4 proj = glm::perspective(glm::radians(kFov), float(kWidth) / float(kHeight), 0.1f, 200.0f);
    const glm::vec4 c = proj * glm::inverse(camWorld) * glm::vec4(p, 1.0f);
    const glm::vec3 ndc = glm::vec3(c) / c.w;
    return {(ndc.x * 0.5f + 0.5f) * kWidth, (0.5f - ndc.y * 0.5f) * kHeight, c.w};   // y down the image
}

std::vector<Footprint> footprints(const View& view, float margin) {
    std::vector<Footprint> out;
    const float focal = 0.5f * kHeight / std::tan(glm::radians(kFov) * 0.5f);
    for (const auto& [pos, radius] : {std::pair{kPointPos, kPointRadius}, std::pair{kSpotPos, kSpotRadius}}) {
        const glm::vec3 p = project(view, pos);
        out.push_back({p.x, p.y, radius * focal / p.z + margin, 0, 0, 0, 0, false});
    }
    Footprint box{0, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f, true};
    for (const glm::vec3& corner : {kAreaPos, kAreaPos + kAreaEdge1, kAreaPos + kAreaEdge2, kAreaPos + kAreaEdge1 + kAreaEdge2}) {
        const glm::vec3 p = project(view, corner);
        box.x0 = std::min(box.x0, p.x - margin);
        box.y0 = std::min(box.y0, p.y - margin);
        box.x1 = std::max(box.x1, p.x + margin);
        box.y1 = std::max(box.y1, p.y + margin);
    }
    out.push_back(box);
    return out;
}

bool inside(const std::vector<Footprint>& fs, float x, float y) {
    for (const Footprint& f : fs) {
        if (f.box ? (x >= f.x0 && x <= f.x1 && y >= f.y0 && y <= f.y1)
                  : ((x - f.x) * (x - f.x) + (y - f.y) * (y - f.y) <= f.r * f.r))
            return true;
    }
    return false;
}

struct Diff { int maxAbs{0}; std::size_t changed{0}; };

// Pixels that differ between a and b, those inside `fs` left out.
Diff diffOutside(const Image& a, const Image& b, const std::vector<Footprint>& fs) {
    Diff d;
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            if (inside(fs, x + 0.5f, y + 0.5f)) continue;
            int m = 0;
            for (int c = 0; c < 3; ++c) {
                const std::size_t i = (std::size_t{y} * kWidth + x) * 4 + c;
                m = std::max(m, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
            }
            d.maxAbs = std::max(d.maxAbs, m);
            d.changed += m > 0;
        }
    return d;
}

// The driver builds a BVH differently for a TLAS with other instances in it: a ray exactly on a
// shared triangle edge may take the other triangle (AGENTS.md, the goldens' floor).
bool withinBvhFloor(const Diff& d) { return d.maxAbs <= 1 && d.changed <= kWidth * kHeight / 2000; }

// The path tracer weighs light sampling against a bounce ray meeting an emitter (multiple
// importance sampling), so its emitters change its samples everywhere but not its light: the mean
// outside the footprints, overall and in each 32x32 block (outside them), is the same within the
// noise of 16 samples.
struct SameLight { double whole{0.0}; double worstBlock{0.0}; };
SameLight sameLight(const Image& a, const Image& b, const std::vector<Footprint>& fs) {
    constexpr uint32_t kBlock = 32;
    const uint32_t bw = kWidth / kBlock, bh = kHeight / kBlock;
    std::vector<double> sa(bw * bh, 0.0), sb(bw * bh, 0.0);
    double ta = 0.0, tb = 0.0;
    for (uint32_t y = 0; y < bh * kBlock; ++y)
        for (uint32_t x = 0; x < bw * kBlock; ++x) {
            if (inside(fs, x + 0.5f, y + 0.5f)) continue;
            const std::size_t i = (std::size_t{y} * kWidth + x) * 4;
            const double la = a.rgba[i] + a.rgba[i + 1] + a.rgba[i + 2], lb = b.rgba[i] + b.rgba[i + 1] + b.rgba[i + 2];
            sa[(y / kBlock) * bw + x / kBlock] += la;
            sb[(y / kBlock) * bw + x / kBlock] += lb;
            ta += la;
            tb += lb;
        }
    SameLight r;
    r.whole = std::abs(ta - tb) / std::max(tb, 1.0);
    for (std::size_t k = 0; k < sa.size(); ++k)
        if (sb[k] > 3.0 * 20.0 * kBlock * kBlock * 0.25)   // blocks with light in them (mean > 20 over a quarter)
            r.worstBlock = std::max(r.worstBlock, std::abs(sa[k] - sb[k]) / sb[k]);
    return r;
}

const char* modeName(Mode m) {
    return m == Mode::Deferred ? "deferred (RT shadows, RT GI)" : m == Mode::DeferredCsm ? "deferred (CSM)" : "path traced";
}

void emitters_follow_the_lights() {
    std::printf("emitters: a point, a spot and an area light get one each, the sun none\n");
    const std::string scene = roomScene(kLightsInView, "count");
    const Image on = capture(scene, Mode::Deferred, true), off = capture(scene, Mode::Deferred, false);
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
    std::printf("  emitter instances: %zu on, %zu off\n", on.emitters, off.emitters);
    check(on.emitters == 3, "emitters: three emitter instances for the point, spot and area light (none for the sun)");
    check(off.emitters == 0, "emitters: turned off, none");
}

void out_of_view_emitters_change_nothing() {
    std::printf("out of view: with or without emitters, the same image\n");
    const std::string scene = roomScene(kLightsOutOfView, "out");
    for (const Mode mode : {Mode::Deferred, Mode::DeferredCsm, Mode::PathTraced}) {
        const Image on = capture(scene, mode, true), off = capture(scene, mode, false);
        if (on.rgba.empty() || off.rgba.empty()) {
            check(false, std::string("out of view: ") + modeName(mode) + ": the captures rendered");
            continue;
        }
        const Diff d = diffOutside(on, off, {});
        double mean = 0.0;
        for (std::size_t i = 0; i < off.rgba.size(); i += 4) mean += off.rgba[i] + off.rgba[i + 1] + off.rgba[i + 2];
        mean /= 3.0 * kWidth * kHeight;
        std::printf("  %s: max_abs %d on %zu pixels (mean %.1f)\n", modeName(mode), d.maxAbs, d.changed, mean);
        check(mean > 10.0, std::string("out of view: ") + modeName(mode) + ": the floor is lit");
        if (mode == Mode::PathTraced) {
            const SameLight l = sameLight(on, off, {});
            std::printf("  %s: mean %.4f apart, the worst 32x32 block %.4f\n", modeName(mode), l.whole, l.worstBlock);
            check(l.whole <= 0.01 && l.worstBlock <= 0.05,
                  std::string("out of view: ") + modeName(mode) + ": the same light (mean within 1%, blocks within 5%)");
        } else {
            check(withinBvhFloor(d), std::string("out of view: ") + modeName(mode) + ": the same image, within the BVH floor");
        }
        keep(on, std::string("out_on_") + std::to_string(int(mode)));
        keep(off, std::string("out_off_") + std::to_string(int(mode)));
    }
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
}

void in_view_emitters_change_only_their_pixels() {
    std::printf("in view: the emitters are bright, and nothing else changed\n");
    const std::string scene = roomScene(kLightsInView, "in");
    const std::vector<Footprint> fs = footprints(kLightsInView, 3.0f);
    for (const Mode mode : {Mode::Deferred, Mode::PathTraced}) {
        const Image on = capture(scene, mode, true), off = capture(scene, mode, false);
        if (on.rgba.empty() || off.rgba.empty()) {
            check(false, std::string("in view: ") + modeName(mode) + ": the captures rendered");
            continue;
        }
        const Diff d = diffOutside(on, off, fs), far = diffOutside(on, off, footprints(kLightsInView, 24.0f));
        std::printf("  %s: outside the footprints max_abs %d on %zu pixels; past 24 px, max_abs %d on %zu\n",
                    modeName(mode), d.maxAbs, d.changed, far.maxAbs, far.changed);
        const SameLight l = sameLight(on, off, footprints(kLightsInView, 24.0f));
        std::printf("  %s: past 24 px, mean %.4f apart, the worst 32x32 block %.4f\n", modeName(mode), l.whole,
                    l.worstBlock);
        check(l.whole <= 0.01 && l.worstBlock <= 0.05,
              std::string("in view: ") + modeName(mode) + ": past 24 px of an emitter, the same light (mean within 1%, blocks within 5%)");
        const auto centre = [&](const Image& im, const glm::vec3& p) {
            const glm::vec3 q = project(kLightsInView, p);
            const std::size_t i = (std::size_t(q.y) * kWidth + std::size_t(q.x)) * 4;
            return std::max({im.rgba[i], im.rgba[i + 1], im.rgba[i + 2]});
        };
        const glm::vec3 areaCentre = kAreaPos + 0.5f * (kAreaEdge1 + kAreaEdge2);
        const char* names[] = {"point", "spot", "area"};
        const glm::vec3 centres[] = {kPointPos, kSpotPos, areaCentre};
        for (int k = 0; k < 3; ++k) {
            const int lit = centre(on, centres[k]), dark = centre(off, centres[k]);
            std::printf("  %s: %s light's centre pixel %d with its emitter, %d without\n", modeName(mode), names[k], lit, dark);
            if (k == 1)   // the spot points down: the camera is outside its cone, and its bulb is dark
                check(lit < 30 && lit < dark,
                      std::string("in view: ") + modeName(mode) + ": the spot light's emitter, seen from outside its cone, is dark");
            else
                check(lit >= 200 && lit > dark + 10,
                      std::string("in view: ") + modeName(mode) + ": the " + names[k] + " light's emitter is visible and bright");
        }
        keep(on, std::string("in_on_") + std::to_string(int(mode)));
        keep(off, std::string("in_off_") + std::to_string(int(mode)));
    }
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
}

// The same spot turned toward the camera: seen from inside its cone, its bulb shines.
void spot_seen_from_inside_its_cone_is_bright() {
    std::printf("spot: seen from inside its cone, its emitter is bright\n");
    const std::string scene = roomScene(kLightsInView, "aim", true);
    for (const Mode mode : {Mode::Deferred, Mode::PathTraced}) {
        const Image on = capture(scene, mode, true), off = capture(scene, mode, false);
        if (on.rgba.empty() || off.rgba.empty()) {
            check(false, std::string("spot: ") + modeName(mode) + ": the captures rendered");
            continue;
        }
        const glm::vec3 q = project(kLightsInView, kSpotPos);
        const std::size_t i = (std::size_t(q.y) * kWidth + std::size_t(q.x)) * 4;
        const int lit = std::max({on.rgba[i], on.rgba[i + 1], on.rgba[i + 2]});
        const int dark = std::max({off.rgba[i], off.rgba[i + 1], off.rgba[i + 2]});
        std::printf("  %s: the spot's centre pixel %d with its emitter, %d without\n", modeName(mode), lit, dark);
        check(lit >= 200 && lit > dark + 10, std::string("spot: ") + modeName(mode) + ": seen from inside its cone, the emitter is bright");
        keep(on, std::string("aim_on_") + std::to_string(int(mode)));
    }
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
}

// A light moved since the last frame: its emitter's prevWorld is last frame's placement, the
// motion vector TAA reprojects with, as a mesh's is; one frame later it catches up.
void moved_light_emitters_carry_their_motion() {
    std::printf("motion: a moved light's emitter remembers where it was\n");
    const std::string scene = roomScene(kLightsInView, "motion");
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    ecs::World world;
    api::Session session(world, engineTypes());
    session.setAssetResolver(&assets);
    host.bind(world);
    session.setHost(&host);
    const std::string loaded = session.call("scene.load", std::string(R"({"replace":true,"path":")") + scene + "\"}");
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
    api::RenderRequest r;
    r.mode = "deferred";
    r.frames = 1;
    r.width = kWidth;
    r.height = kHeight;
    (void)host.capture(r);
    if (loaded.rfind("{\"result\"", 0) != 0 || !host.renderer()) {
        check(false, "motion: the scene loaded and rendered");
        return;
    }
    const render::RenderScene& rs = host.renderer()->renderScene();
    const auto entity = [&](const char* name) {
        for (auto [e, n] : world.registry().view<const ecs::Name>().each())
            if (n.value == name) return e;
        return entt::entity{entt::null};
    };
    // Emitter slots are in light order: point, spot, area (the sun has none).
    const auto emitterAt = [&](std::size_t i) -> const render::InstanceGpu& { return rs.instance(rs.lightEmitterSlots()[i]); };
    const auto near = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; };
    const auto origin = [](const glm::mat4& m) { return glm::vec3(m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)); };
    if (rs.lightEmitterSlots().size() != 3) {
        check(false, "motion: three emitter instances");
        return;
    }
    const glm::vec3 step{0.5f, -0.25f, 0.4f};
    const glm::vec3 areaWas = origin(emitterAt(2).world);
    world.setTransform(entity("Point"), ecs::Transform{kPointPos + step});
    world.setTransform(entity("Area"), ecs::Transform{kAreaPos + step});
    world.sync();
    const render::InstanceGpu& point = emitterAt(0);
    const render::InstanceGpu& area = emitterAt(2);
    std::printf("  point: world (%.2f %.2f %.2f) prev (%.2f %.2f %.2f)\n", point.world[3].x, point.world[3].y,
                point.world[3].z, point.prevWorld[3].x, point.prevWorld[3].y, point.prevWorld[3].z);
    check(near(origin(point.world), kPointPos + step), "motion: the point light's emitter is where the light is now");
    check(near(origin(point.prevWorld), kPointPos), "motion: the point light's emitter's prevWorld is where it was");
    check(near(glm::vec3(point.prevWorld[0]), glm::vec3(point.world[0])), "motion: the sphere's size is the same in both");
    check(near(origin(area.world), areaWas + step) && near(origin(area.prevWorld), areaWas),
          "motion: the area light's rectangle moved, and its prevWorld is where it was");
    world.sync();   // not moved again: prevWorld catches up, as a mesh's does
    check(emitterAt(0).prevWorld == emitterAt(0).world && emitterAt(2).prevWorld == emitterAt(2).world,
          "motion: a frame later, the emitters' prevWorld is their world");
}

// TAA on a point light that moves sideways ~1.5 px a frame for 12 frames, against the same 12 frames
// with the light still at its last position. With its motion vectors, TAA reprojects the mesh's own
// history and its pixels match the still light's; without them the mesh's history is the floor and
// wall behind it, and the moving mesh comes out darker, smeared toward where it was.
struct MovingCapture { Image image; bool ok{false}; };
constexpr int kTaaFrames = 12;
const glm::vec3 kTaaStep{0.03f, 0.0f, 0.0f};   // ~1.5 px a frame

MovingCapture taaCapture(const std::string& scene, bool moving) {
    MovingCapture out;
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    ecs::World world;
    api::Session session(world, engineTypes());
    session.setAssetResolver(&assets);
    host.bind(world);
    session.setHost(&host);
    if (session.call("scene.load", std::string(R"({"replace":true,"path":")") + scene + "\"}").rfind("{\"result\"", 0) != 0)
        return out;
    entt::entity point{entt::null};
    for (auto [e, n] : world.registry().view<const ecs::Name>().each())
        if (n.value == "Point") point = e;
    int frame = 0;
    world.addSyncBefore("transform", "mover", [&](ecs::World& w) {
        const int i = moving ? std::min(frame, kTaaFrames - 1) : kTaaFrames - 1;
        w.setTransform(point, ecs::Transform{kPointPos + float(i) * kTaaStep});
        ++frame;
    });
    api::RenderRequest r;
    r.mode = "deferred";
    r.frames = kTaaFrames;
    r.width = kWidth;
    r.height = kHeight;
    r.post = {"taa"};
    r.ev100 = 4.8f;   // exposure 0.03: the mesh (radiance ~25) mid-range, not clipped: TAA's blend shows
    const std::string png = std::string(OHAO_SOURCE_DIR) + "/" + scratch("taa") + ".png";
    r.path = png;
    const api::RenderOutcome o = host.capture(r);
    if (!o.errors.empty()) return out;
    int w = 0, h = 0, n = 0;
    if (unsigned char* px = stbi_load(png.c_str(), &w, &h, &n, 4)) {
        if (w == int(kWidth) && h == int(kHeight)) out.image.rgba.assign(px, px + std::size_t{kWidth} * kHeight * 4);
        stbi_image_free(px);
    }
    std::remove(png.c_str());
    out.ok = !out.image.rgba.empty() && frame >= kTaaFrames;
    return out;
}

void moving_light_emitter_is_taa_stable() {
    std::printf("taa: a moving light's mesh against the same light held still\n");
    const std::string scene = roomScene(kLightsInView, "taa");
    const MovingCapture moving = taaCapture(scene, true), still = taaCapture(scene, false);
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
    if (!moving.ok || !still.ok) {
        check(false, "taa: the captures rendered");
        return;
    }
    // The last frame's sphere and 4 px around it: inside, any history is clipped to the sphere's
    // own radiance; the edge is where a mesh's history lags or leads.
    const glm::vec3 c = project(kLightsInView, kPointPos + float(kTaaFrames - 1) * kTaaStep);
    const float focal = 0.5f * kHeight / std::tan(glm::radians(kFov) * 0.5f);
    const float rad = kPointRadius * focal / c.z + 4.0f;
    double sum = 0.0, lumMoving = 0.0, lumStill = 0.0;
    int count = 0, worst = 0;
    for (int y = int(c.y - rad); y <= int(c.y + rad); ++y)
        for (int x = int(c.x - rad); x <= int(c.x + rad); ++x) {
            if ((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) > rad * rad || x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight))
                continue;
            const std::size_t i = (std::size_t(y) * kWidth + std::size_t(x)) * 4;
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(int(moving.image.rgba[i + k]) - int(still.image.rgba[i + k]));
                sum += d;
                worst = std::max(worst, d);
                lumMoving += moving.image.rgba[i + k];
                lumStill += still.image.rgba[i + k];
            }
            ++count;
        }
    const double mean = count ? sum / (3.0 * count) : 255.0;
    std::printf("  sphere (%d px): mean |moving - still| %.1f, max %d; mean value moving %.1f, still %.1f\n", count, mean,
                worst, count ? lumMoving / (3.0 * count) : 0.0, count ? lumStill / (3.0 * count) : 0.0);
    // Measured: with motion vectors the moving sphere's mean value is 0.9 under the still one's and
    // the mean difference 12.1 (its edge, which TAA has had fewer frames to anti-alias); without them,
    // 4.9 under (darker, its history the background) and 15.0.
    const double bias = count ? (lumStill - lumMoving) / (3.0 * count) : 255.0;
    check(count > 20, "taa: the sphere covers some pixels");
    check(std::abs(bias) <= 2.5, "taa: the moving light's mesh is as bright as the still one's (its own history reprojected)");
    check(mean <= 13.5, "taa: the moving light's mesh differs from the still one's by its edge's anti-aliasing only");
    keep(moving.image, "taa_moving");
    keep(still.image, "taa_still");
}

// ---- the first-person gun (sub-project 9, test 21) -------------------------------------------------
// A ViewModel's mesh is drawn shrunk toward the eye: kept out of every CSM cascade and every shadow,
// GI and bounce ray, it changes nothing but its own pixels; the same box placed in the world where the
// gun is drawn, as an ordinary mesh, shadows the room.

const glm::vec3 kGunAt{0.25f, -0.25f, -0.6f};          // below the camera, in its space
const glm::vec3 kGunLo{-0.06f, -0.06f, -0.7f}, kGunHi{0.06f, 0.06f, 0.0f};
constexpr float kGunDepth = 0.55f;                     // ViewModel depthScale; fovDeg = the scene's

glm::mat4 cameraWorld(const View& view) {
    return glm::translate(glm::mat4(1.0f), view.position) *
           glm::mat4_cast(glm::angleAxis(glm::radians(view.pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f)));
}

// Where the gun is drawn: the camera's world times the remap (uniform, the lens being the scene's)
// times its pose below the camera.
glm::mat4 gunDrawn(const View& view) {
    return cameraWorld(view) * ecs::viewModelRemap(glm::translate(glm::mat4(1.0f), kGunAt), kFov, kFov, kGunDepth);
}

enum class Gun { None, ViewModel, PlainAtDrawn };

std::string gunScene(const View& view, const std::string& tag, Gun gun) {
    return roomScene(view, tag, false, [&](ecs::World& w, entt::entity camera) {
        using namespace ecs;
        // A lamp just behind and above the eye: a box near the eye throws a large shadow ahead.
        Light lamp;
        lamp.intensity = 6.0f;
        lamp.range = 30.0f;
        lamp.radius = 0.02f;
        w.registry().emplace<Light>(w.create("Lamp behind", Transform{view.position + glm::vec3(0.25f, 0.35f, 0.5f)}), lamp);
        if (gun == Gun::None) return;
        const Material metal = ohao::test::material({0.09f, 0.09f, 0.1f}, 0.45f, 0.6f, 0.0f);
        const MeshSource box{BoxPrim{kGunLo, kGunHi, glm::vec3(0.09f)}};
        if (gun == Gun::ViewModel) {
            const auto e = w.create("Gun", Transform{kGunAt});
            w.setParent(e, camera);
            w.registry().emplace<MeshSource>(e, box);
            w.registry().emplace<Material>(e, metal);
            ViewModel vm;
            vm.fovDeg = kFov;
            vm.depthScale = kGunDepth;
            w.registry().emplace<ViewModel>(e, vm);
        } else {   // the same box at the gun's drawn pose, an ordinary mesh in the world
            const glm::mat4 m = gunDrawn(view);
            Transform t;
            t.position = glm::vec3(m[3]);
            t.rotation = glm::quat_cast(glm::mat3(cameraWorld(view)));
            t.scale = glm::vec3(kGunDepth);
            const auto e = w.create("Box at the gun", t);
            w.registry().emplace<MeshSource>(e, box);
            w.registry().emplace<Material>(e, metal);
        }
    });
}

void view_model_changes_only_its_pixels() {
    std::printf("gun: a ViewModel's mesh shadows nothing; the same box in the world does\n");
    const View view = kLightsInView;
    const std::string none = gunScene(view, "gun_none", Gun::None), gun = gunScene(view, "gun_vm", Gun::ViewModel),
                      plain = gunScene(view, "gun_plain", Gun::PlainAtDrawn);
    const Image off = capture(none, Mode::Deferred, true, true), on = capture(gun, Mode::Deferred, true, true),
                inWorld = capture(plain, Mode::Deferred, true, true);
    for (const auto& s : {none, gun, plain}) std::remove((std::string(OHAO_SOURCE_DIR) + "/" + s).c_str());
    if (off.rgba.empty() || on.rgba.empty() || inWorld.rgba.empty()) {
        check(false, "gun: the captures rendered");
        return;
    }
    keep(on, "gun_vm");
    keep(inWorld, "gun_plain");
    // The gun's footprint: its drawn corners' bounding box, grown by the screen-space filters' 24 px.
    Footprint fp{0, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f, true};
    const glm::mat4 drawn = gunDrawn(view);
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 c((k & 1) ? kGunHi.x : kGunLo.x, (k & 2) ? kGunHi.y : kGunLo.y, (k & 4) ? kGunHi.z : kGunLo.z);
        const glm::vec3 p = project(view, glm::vec3(drawn * glm::vec4(c, 1.0f)));
        fp.x0 = std::min(fp.x0, p.x - 24.0f);
        fp.y0 = std::min(fp.y0, p.y - 24.0f);
        fp.x1 = std::max(fp.x1, p.x + 24.0f);
        fp.y1 = std::max(fp.y1, p.y + 24.0f);
    }
    const std::vector<Footprint> fps{fp};
    const Diff outside = diffOutside(on, off, fps);
    const Diff all = diffOutside(on, off, {});
    const Diff plainOutside = diffOutside(inWorld, off, fps);
    std::printf("  footprint x %.0f..%.0f y %.0f..%.0f; gun: %zu px changed, %zu outside (max %d); the plain box: %zu "
                "outside (max %d)\n",
                fp.x0, fp.x1, fp.y0, fp.y1, all.changed, outside.changed, outside.maxAbs, plainOutside.changed,
                plainOutside.maxAbs);
    check(all.changed > 200, "gun: drawn (its pixels changed)");
    const SameLight l = sameLight(on, off, fps), lp = sameLight(inWorld, off, fps);
    std::printf("  past the footprint: gun mean %.4f apart (worst block %.4f), the plain box %.4f (%.4f)\n", l.whole,
                l.worstBlock, lp.whole, lp.worstBlock);
    check(l.whole <= 0.01 && l.worstBlock <= 0.05,
          "gun: past 24 px of its footprint the same light (mean within 1%, blocks within 5%): no shadow, no GI");
    check(plainOutside.changed > outside.changed + 50 && plainOutside.maxAbs > 2,
          "gun: the same box as a world mesh shadows the room (the comparison sees a shadow)");
}

// Sub-project 10 step 3: the player's skinned arms and rifle as the ViewModel's child (as the play
// scene has them) change nothing but their own pixels: within 24 px of their projected vertices, and
// below them to the image's edge where they come from behind the eye.
void view_model_arms_change_only_their_pixels() {
    std::printf("arms: the skinned arms below a ViewModel shadow nothing\n");
    const char* glb = "assets/characters/fps_arms/fps_arms.glb";
    const View view = kLightsInView;
    const std::string none = gunScene(view, "arms_none", Gun::None);
    const std::string arms = roomScene(view, "arms_vm", false, [&](ecs::World& w, entt::entity camera) {
        using namespace ecs;
        Light lamp;
        lamp.intensity = 6.0f;
        lamp.range = 30.0f;
        lamp.radius = 0.02f;
        w.registry().emplace<Light>(w.create("Lamp behind", Transform{view.position + glm::vec3(0.25f, 0.35f, 0.5f)}), lamp);
        const auto root = w.create("Gun", Transform{});
        w.setParent(root, camera);
        ViewModel vm;
        vm.fovDeg = kFov;
        vm.depthScale = kGunDepth;
        w.registry().emplace<ViewModel>(root, vm);
        const auto a = w.create("Arms", Transform{});
        w.setParent(a, root);
        w.registry().emplace<MeshSource>(a, MeshSource{ModelAsset{AssetRef{glb, {}}, "Arms"}});
        w.registry().emplace<SkinnedMesh>(a, SkinnedMesh{AssetRef{glb, {}}, "ArmsRig"});
        w.registry().emplace<Animator>(a, Animator{AssetRef{"assets/anim/arms.animgraph.json", {}}, {}});
    });
    const Image off = capture(none, Mode::Deferred, true, true), on = capture(arms, Mode::Deferred, true, true);
    for (const auto& s : {none, arms}) std::remove((std::string(OHAO_SOURCE_DIR) + "/" + s).c_str());
    if (off.rgba.empty() || on.rgba.empty()) {
        check(false, "arms: the captures rendered");
        return;
    }
    keep(on, "arms_vm");
    // The arms before any step are at their bind pose: the mesh's own vertices, remapped below the eye.
    const auto model = ModelLoader::loadMesh((std::string(OHAO_SOURCE_DIR) + "/" + glb), "Arms");
    if (!model || !model.value()) {
        check(false, "arms: the mesh loads");
        return;
    }
    const glm::mat4 remap = ecs::viewModelRemap(glm::mat4(1.0f), kFov, kFov, kGunDepth);
    const glm::mat4 drawn = cameraWorld(view) * remap;
    Footprint fp{0, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f, true};
    bool behind = false;
    for (const Vertex& v : model.value()->vertices) {
        if ((remap * glm::vec4(v.position, 1.0f)).z > -0.1f) {   // at or behind the near plane: not drawn here
            behind = true;
            continue;
        }
        const glm::vec3 p = project(view, glm::vec3(drawn * glm::vec4(v.position, 1.0f)));
        fp.x0 = std::min(fp.x0, p.x - 24.0f);
        fp.y0 = std::min(fp.y0, p.y - 24.0f);
        fp.x1 = std::max(fp.x1, p.x + 24.0f);
        fp.y1 = std::max(fp.y1, p.y + 24.0f);
    }
    if (behind) fp.y1 = float(kHeight);   // what crosses the near plane reaches the bottom edge
    const Diff outside = diffOutside(on, off, {fp});
    const Diff all = diffOutside(on, off, {});
    std::printf("  footprint x %.0f..%.0f y %.0f..%.0f; arms: %zu px changed, %zu outside (max %d)\n", fp.x0, fp.x1, fp.y0,
                fp.y1, all.changed, outside.changed, outside.maxAbs);
    check(all.changed > 200, "arms: drawn (their pixels changed)");
    // The same light past the footprint, not the same bits: RT GI's radiance cache is fed by the frame's
    // pixels (frontier F5a), and the arms hide some, so the cache's noise moves everywhere by a level.
    const SameLight l = sameLight(on, off, {fp});
    std::printf("  past the footprint: mean %.4f, worst 32x32 block %.4f\n", l.whole, l.worstBlock);
    check(l.whole <= 0.01 && l.worstBlock <= 0.05,
          "arms: the light past 24 px of their footprint the same (no shadow, no GI; mean 1%, blocks 5%)");
}

} // namespace

int main(int argc, char** argv) {
    g_keep = argc > 1 && std::strcmp(argv[1], "--keep") == 0;
    emitters_follow_the_lights();
    out_of_view_emitters_change_nothing();
    in_view_emitters_change_only_their_pixels();
    spot_seen_from_inside_its_cone_is_bright();
    moved_light_emitters_carry_their_motion();
    moving_light_emitter_is_taa_stable();
    view_model_changes_only_its_pixels();
    view_model_arms_change_only_their_pixels();
    return test::finish("light_emitter_gpu_test", g_passed, g_failed, g_failures);
}
