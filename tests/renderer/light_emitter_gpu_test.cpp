// light_emitter_gpu_test: a point, spot or area light is also a mesh the camera sees (RenderScene's
// emitter instances), and that mesh changes nothing but its own pixels.
//
//   emitters        a point, a spot and an area light get one emitter instance each, a sun none;
//                   turning the emitters off removes them, the lights stay.
//   out of view     the lights above a camera looking down at a floor, a box and a wall: the
//                   image with emitters is the image without them (deferred with RT shadows and
//                   GI, deferred with CSM, path traced), within the driver's BVH floor of 1 LSB
//                   on a few pixels: no emitter shadows its light or another, no GI ray or bounce
//                   adds a light twice, no cascade draws one.
//   in view         the same room seen with its lights: each emitter's centre pixel is bright;
//                   every pixel that changed by more than 1 lies on an emitter's footprint, and
//                   the 1-LSB changes around it within 24 px: the deferred frame's screen-space
//                   filters (RT GI's and RT shadows' denoisers) take the mesh's depth among a
//                   pixel's neighbours (deferred and path traced).
//
// Exit code 0 = pass; the last line is a JSON summary. --keep leaves the PNGs in build/.
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/scene/render_scene.hpp"
#include "runtime/image.hpp"
#include "stb_image.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
std::string roomScene(const View& view, const std::string& tag) {
    using namespace ecs;
    World w;
    w.seedIds(11);
    const MaterialOverride rough{{0.6f, 0.6f, 0.6f}, 0.9f, 0.0f, 0.0f};
    const auto floor = w.create("Floor", Transform{});
    w.registry().emplace<MeshSource>(floor, MeshSource{PlatformPrim{20.0f, 0.2f, 20.0f}});
    w.registry().emplace<MaterialOverride>(floor, rough);
    const auto wall = w.create("Wall", Transform{});
    w.registry().emplace<MeshSource>(wall, MeshSource{BoxPrim{{-10.0f, 0.0f, -6.0f}, {10.0f, 8.0f, -5.8f}, glm::vec3(0.6f)}});
    w.registry().emplace<MaterialOverride>(wall, rough);
    const auto box = w.create("Box", Transform{});
    w.registry().emplace<MeshSource>(box, MeshSource{BoxPrim{{-0.8f, 0.1f, -1.5f}, {0.8f, 1.7f, 0.1f}, glm::vec3(0.6f)}});
    w.registry().emplace<MaterialOverride>(box, rough);

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
    w.registry().emplace<ecs::Camera>(w.create("MainCamera", cam), ecs::Camera{kFov, 0.1f, 200.0f, true});

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
// starts the renderer).
Image capture(const std::string& scenePath, Mode mode, bool emitters) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    Image out;
    ecs::World world;
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
        s.rtShadows = false;
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
        check(withinBvhFloor(d), std::string("out of view: ") + modeName(mode) + ": the same image, within the BVH floor");
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
        check(d.maxAbs <= 1, std::string("in view: ") + modeName(mode) + ": only the emitters' own pixels changed by more than 1");
        check(withinBvhFloor(far), std::string("in view: ") + modeName(mode) + ": nothing changed past 24 px of an emitter");
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
            check(lit >= 200 && lit > dark + 10,
                  std::string("in view: ") + modeName(mode) + ": the " + names[k] + " light's emitter is visible and bright");
        }
        keep(on, std::string("in_on_") + std::to_string(int(mode)));
        keep(off, std::string("in_off_") + std::to_string(int(mode)));
    }
    std::remove((std::string(OHAO_SOURCE_DIR) + "/" + scene).c_str());
}

} // namespace

int main(int argc, char** argv) {
    g_keep = argc > 1 && std::strcmp(argv[1], "--keep") == 0;
    emitters_follow_the_lights();
    out_of_view_emitters_change_nothing();
    in_view_emitters_change_only_their_pixels();
    return test::finish("light_emitter_gpu_test", g_passed, g_failed, g_failures);
}
