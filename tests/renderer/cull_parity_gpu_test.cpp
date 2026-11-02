// cull_parity_gpu_test (sub-project 2b, tests 6 and 7): GPU culling changes what is drawn, never
// what is seen, and builds its lists the same way every run; the meshlet path draws the same
// image through task and mesh shaders as through their compute fallback.
//
//   t06 parity     the same frames with culling on and off are identical images (max_abs 0):
//                  a built scene with a wall hiding a row of boxes, boxes off screen and boxes in
//                  view, over several frames (so the early and late passes both draw), and the
//                  checked-in Cornell and helmet scenes as their goldens render them.
//   t06 twin runs  two fresh renderers give byte-identical visibility, draw counts and indirect
//                  commands (prefix-sum compaction: draw order, no atomics).
//   t06 occluded   the boxes behind the wall and off screen are in no list; every box in view is
//                  in exactly one (early or late).
//   t06 face cull  one-sided materials (back faces culled) render the helmet as double-sided ones
//                  do: the culled faces were hidden anyway.
//   t07 meshlets   every draw on the meshlet path, task + mesh shaders and the fallback (compute
//                  culling, per-meshlet indirect draws, vertex pulling) give identical images
//                  (max_abs 0, PNG byte for byte) for the occlusion scene, Cornell and the helmet
//                  room; meshlet culling on and off too; the fallback's lists are the same
//                  every run, and the helmet's meshlet culling drops meshlets.
//
// Exit code 0 = pass; the last line is a JSON summary.
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_types.hpp"
#include "ohao/render_host.hpp"
#include "ecs/spawn.hpp"
#include "ecs/world.hpp"
#include "gpu/shared_layout.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "render/camera/camera.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/scene/render_scene.hpp"
#include "asset/model.hpp"
#include "asset/primitive_mesh_generator.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <process.h>   // _getpid

using namespace ohao;
namespace shared = gpu::shared;

namespace {

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

int maxAbs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || a.size() != b.size()) return 256;
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

// The occlusion scene: a floor, a wall across the view, five boxes behind it (hidden), three
// in front of it (seen), two far to the side (off screen). Draw order = spawn order.
constexpr int kHidden = 5, kSeen = 3, kOffscreen = 2;

void buildOcclusionScene(ecs::World& world, VulkanRenderer& r, bool oneSided) {
    Model floor, wall;
    PrimitiveMeshGenerator::generateBox(floor, {-20.0f, -0.1f, -20.0f}, {20.0f, 0.0f, 20.0f}, {1, 1, 1});
    PrimitiveMeshGenerator::generateBox(wall, {-6.0f, 0.0f, -0.2f}, {6.0f, 4.0f, 0.2f}, {1, 1, 1});
    render::MaterialDesc grey, red, blue;
    grey.baseColor = {0.7f, 0.7f, 0.7f};
    red.baseColor = {0.8f, 0.2f, 0.1f};
    blue.baseColor = {0.2f, 0.3f, 0.8f};
    grey.doubleSided = red.doubleSided = blue.doubleSided = !oneSided;
    ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &grey);
    ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), wall, "wall", {}, &grey);
    const auto box = [&](const char* name, glm::vec3 at, const render::MaterialDesc& m) {
        Model b;
        PrimitiveMeshGenerator::generateBox(b, at - glm::vec3(0.4f), at + glm::vec3(0.4f), {1, 1, 1});
        ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), b, name, {}, &m);
    };
    for (int i = 0; i < kHidden; ++i) box("hidden", {-4.0f + 2.0f * i, 0.5f, -3.0f}, red);
    for (int i = 0; i < kSeen; ++i) box("seen", {-2.0f + 2.0f * i, 0.5f, 3.0f}, blue);
    for (int i = 0; i < kOffscreen; ++i) box("offscreen", {40.0f + 4.0f * i, 0.5f, 0.0f}, blue);
    ecs::Light light;
    light.intensity = 60.0f;
    light.castShadows = false;   // raster only: the test is about the G-buffer
    ecs::spawnLight(world, "light", light, ecs::Transform{{0.0f, 6.0f, 6.0f}});
    r.getCamera().setPosition({0.0f, 2.0f, 9.0f});
    r.getCamera().setRotation(-10.0f, -90.0f);
    r.setRenderMode(RenderMode::Deferred);
}

// A G-buffer phase's lists: instance one-sided, instance double-sided, meshlet one-sided,
// meshlet double-sided; early (lists 0-3) then late (4-7).
constexpr uint32_t kGBufferLists = 8;

// The cone scene: one one-sided sphere (128 x 64 sectors, over kMeshletMinTriangles: the
// meshlet path under geometry auto) seen from the front, its back half facing away.
void buildSphereScene(ecs::World& world, VulkanRenderer& r) {
    Model sphere;
    PrimitiveMeshGenerator::generateSphere(sphere, 1.0f, 128, 64);
    render::MaterialDesc m;
    m.baseColor = {0.7f, 0.6f, 0.5f};
    m.doubleSided = false;
    ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), sphere, "sphere", {}, &m);
    ecs::Light light;
    light.intensity = 40.0f;
    light.castShadows = false;
    ecs::spawnLight(world, "light", light, ecs::Transform{{2.0f, 3.0f, 4.0f}});
    r.getCamera().setPosition({0.0f, 0.5f, 4.0f});
    r.getCamera().setRotation(-7.0f, -90.0f);
    r.setRenderMode(RenderMode::Deferred);
}

struct Run {
    std::vector<uint8_t> image;
    GpuCulling::Capture lists;   // the last frame's
    std::vector<std::array<uint32_t, kGBufferLists>> counts;   // per frame, the G-buffer lists' draw counts
    uint32_t meshlets{0};        // the scene's meshlets
};

enum class Scene { Occlusion, Sphere };

// `frames` frames of a built scene from a reset, culling as `s`, the last read back.
Run sceneRun(Scene scene, GpuCulling::Settings s, int frames, bool oneSided = false) {
    Run out;
    VulkanRenderer r(320, 180);
    if (!r.initialize()) return out;
    {
        ecs::World world;   // after the renderer: released first
        ecs::installRenderExtract(world, r.renderScene());
        if (scene == Scene::Occlusion) buildOcclusionScene(world, r, oneSided);
        else buildSphereScene(world, r);
        GpuCulling* culling = r.getDeferredRenderer()->culling();
        culling->setSettings(s);
        culling->setCapture(true);
        world.sync();
        r.resetAccumulation();
        for (int f = 0; f < frames; ++f) {
            world.sync();
            if (f == frames - 1) r.requestReadback();
            r.render();
            r.waitForSubmitted(r.framesSubmitted());
            out.lists = culling->capture();
            if (out.lists.counts.size() >= kGBufferLists) {
                std::array<uint32_t, kGBufferLists> c{};
                std::copy_n(out.lists.counts.begin(), kGBufferLists, c.begin());
                out.counts.push_back(c);
            }
        }
        const auto px = r.readback();
        out.image.assign(px.begin(), px.end());
        for (const auto& d : r.renderScene().draws()) out.meshlets += d.meshletCount;
    }
    return out;
}

Run occlusionRun(GpuCulling::Settings s, int frames, bool oneSided = false) {
    return sceneRun(Scene::Occlusion, s, frames, oneSided);
}

std::string countsText(const Run& run) {
    std::string s;
    for (const auto& c : run.counts)
        s += "[" + std::to_string(c[0] + c[1] + c[2] + c[3]) + "+" + std::to_string(c[4] + c[5] + c[6] + c[7]) + "]";
    return s;
}

// The draws listed by `list` (their draw-table indices: an instance command's firstInstance,
// a meshlet command's fourth word).
std::vector<uint32_t> listed(const GpuCulling::Capture& c, uint32_t list) {
    std::vector<uint32_t> out;
    if (c.counts.size() <= list) return out;
    const std::size_t base = std::size_t{list} * c.drawCount * 5;
    const uint32_t word = list % 4 >= 2 ? 3 : 4;
    for (uint32_t k = 0; k < c.counts[list]; ++k) out.push_back(c.commands[base + k * 5 + word]);
    return out;
}

void t06_parity_occlusion_scene() {
    std::printf("t06: culling on and off render the occlusion scene identically\n");
    for (const int frames : {1, 2, 4}) {
        const Run on = occlusionRun({true, true}, frames);
        if (on.image.empty()) {
            std::printf("  (no Vulkan device -- skipped)\n");
            return;
        }
        const Run frustumOnly = occlusionRun({true, false}, frames);
        const Run off = occlusionRun({false, false}, frames);
        const int dOn = maxAbs(on.image, off.image), dFrustum = maxAbs(frustumOnly.image, off.image);
        std::printf("  %d frames: on vs off max_abs %d, frustum only vs off %d; draws (early+late) on %s, off %s\n",
                    frames, dOn, dFrustum, countsText(on).c_str(), countsText(off).c_str());
        check(dOn == 0, "t06: culling on = off, " + std::to_string(frames) + " frames");
        check(dFrustum == 0, "t06: frustum culling only = off, " + std::to_string(frames) + " frames");
    }
}

void t06_twin_runs_build_identical_lists() {
    std::printf("t06: two fresh renderers build byte-identical lists\n");
    const Run a = occlusionRun({true, true}, 3), b = occlusionRun({true, true}, 3);
    if (a.image.empty()) return;
    check(!a.lists.commands.empty(), "t06: the lists were captured");
    check(a.lists.counts == b.lists.counts, "t06: twin runs: the same draw counts");
    check(a.lists.visibility == b.lists.visibility, "t06: twin runs: the same visibility");
    check(a.lists.commands == b.lists.commands, "t06: twin runs: the same indirect commands, byte for byte");
    check(maxAbs(a.image, b.image) == 0, "t06: twin runs: the same image");
}

void t06_occluded_and_offscreen_boxes_are_not_drawn() {
    std::printf("t06: hidden and off-screen boxes are in no list\n");
    const Run run = occlusionRun({true, true}, 3);
    if (run.image.empty()) return;
    // Draw order: floor 0, wall 1, hidden 2..6, seen 7..9, off screen 10..11, then the light's
    // emitter mesh (RenderScene::setLights), 12.
    std::vector<int> drawn(run.lists.drawCount, 0);
    for (uint32_t list = 0; list < kGBufferLists; ++list)
        for (const uint32_t d : listed(run.lists, list)) ++drawn[d];
    const int firstHidden = 2, firstSeen = firstHidden + kHidden, firstOff = firstSeen + kSeen;
    bool hidden = true, seen = true, off = true;
    for (int i = 0; i < kHidden; ++i) hidden = hidden && drawn[firstHidden + i] == 0;
    for (int i = 0; i < kSeen; ++i) seen = seen && drawn[firstSeen + i] == 1;
    for (int i = 0; i < kOffscreen; ++i) off = off && drawn[firstOff + i] == 0;
    std::printf("  frame 3 draws: %s (of %u)\n", countsText(run).c_str(), run.lists.drawCount);
    check(run.lists.drawCount == 2u + kHidden + kSeen + kOffscreen + 1u,
          "t06: one draw per instance, and one for the light's emitter mesh");
    check(drawn[0] == 1 && drawn[1] == 1, "t06: the floor and the wall are drawn once");
    check(hidden, "t06: the boxes behind the wall are not drawn (occluded)");
    check(seen, "t06: each box in view is drawn exactly once (early or late)");
    check(off, "t06: the boxes off screen are not drawn (frustum)");
    const auto& c = run.counts.back();
    check(c[0] + c[1] + c[2] + c[3] > 0, "t06: from frame 2 the early pass draws last frame's visible set");
}

void t06_face_culling_changes_no_pixel() {
    std::printf("t06: one-sided materials (back faces culled) render closed meshes unchanged\n");
    const Run doubleSided = occlusionRun({true, true}, 2, false);
    const Run oneSided = occlusionRun({true, true}, 2, true);
    if (doubleSided.image.empty()) return;
    const int d = maxAbs(doubleSided.image, oneSided.image);
    const auto& c = oneSided.counts.back();
    const uint32_t one = c[0] + c[2] + c[4] + c[6], two = c[1] + c[3] + c[5] + c[7];
    std::printf("  max_abs %d; one-sided run's lists: %u one-sided, %u double-sided\n", d, one, two);
    check(one > 0 && two == 0, "t06: one-sided draws use the face-culled lists");
    check(d == 0, "t06: face culling changes no pixel of closed boxes");
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

std::vector<uint8_t> readFile(const std::string& p) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        uint8_t buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}

// What a scene capture's culling did in its last frame.
struct SceneLists {
    std::string text;               // the G-buffer lists' counts
    GpuCulling::Capture capture;
    uint32_t meshletDrawMeshlets{0};   // the meshlets of the draws on the meshlet path
};

// A checked-in scene captured as `ohao run` captures it (the CLI's stack), culling as `s`; the
// PNG (OHAO's deterministic encoder: equal files are equal images) written to `png`.
std::vector<uint8_t> sceneCapture(const char* path, const char* capture, GpuCulling::Settings s, const std::string& png,
                                  SceneLists& lists) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    std::vector<uint8_t> out;
    {
        ecs::World world;
        api::Session session(world, engineTypes());
        session.setAssetResolver(&assets);
        ecs::installAnimation(world, assets);   // the skinned rig's pose (test 8); nothing else animates
        host.bind(world);
        session.setHost(&host);
        const std::string loaded = session.call("scene.load", std::string(R"({"replace":true,"path":")") + path + "\"}");
        if (loaded.rfind("{\"result\"", 0) != 0) return out;
        // A one-frame capture starts the renderer; then the settings, then the capture.
        api::RenderRequest warm;
        warm.mode = "deferred";
        warm.frames = 1;
        warm.width = 480;
        warm.height = 270;
        (void)host.capture(warm);
        if (!host.renderer()) return out;
        GpuCulling* culling = host.renderer()->getDeferredRenderer()->culling();
        culling->setSettings(s);
        culling->setCapture(true);
        api::RenderRequest r;
        r.mode = "deferred";
        r.width = 480;
        r.height = 270;
        if (std::strcmp(capture, "post") == 0) {
            r.frames = 30;
            r.post = {"taa", "bloom"};
            r.ev100 = 0.0f;
        } else {
            r.frames = 10;
        }
        r.path = png;
        const api::RenderOutcome o = host.capture(r);
        if (!o.errors.empty()) return out;
        out = readFile(png);
        lists.capture = culling->capture();
        const GpuCulling::Capture& c = lists.capture;
        for (uint32_t list = 0; list < kGBufferLists && list < c.counts.size(); ++list)
            lists.text += std::to_string(c.counts[list]) + " ";
        lists.text += "of " + std::to_string(c.drawCount);
        for (const auto& d : host.renderer()->renderScene().draws()) {
            const bool meshletPath = d.meshletCount > 0 && (s.geometry == GpuCulling::Geometry::Meshlets ||
                                                            (s.geometry == GpuCulling::Geometry::Auto &&
                                                             d.indexCount / 3 >= shared::kMeshletMinTriangles));
            if (meshletPath) lists.meshletDrawMeshlets += d.meshletCount;
        }
    }
    return out;
}

void t06_parity_scene_files() {
    std::printf("t06: culling on and off render the checked-in scenes identically\n");
    struct Case { const char* name; const char* tag; const char* path; const char* capture; };
    for (const Case& c : {Case{"cornell, 10 frames", "cornell", "assets/scenes/cornell_box.scene.json", "plain"},
                          Case{"helmet room, 30 frames with post", "helmet", "assets/scenes/helmet_game.scene.json", "post"}}) {
        // Per-process names: CTest may run another copy of this test.
        const std::string base = std::string(OHAO_BINARY_DIR) + "/cull_parity_" + c.tag + "_" +
                                 std::to_string(static_cast<unsigned long>(_getpid()));
        SceneLists onLists, offLists;
        const auto on = sceneCapture(c.path, c.capture, {true, true}, base + "_on.png", onLists);
        const auto off = sceneCapture(c.path, c.capture, {false, false}, base + "_off.png", offLists);
        if (on.empty() || off.empty()) {
            check(false, std::string("t06: ") + c.name + ": the capture rendered");
            continue;
        }
        std::printf("  %s: PNGs %s; last frame's lists (early, late: instance 1-/2-sided, meshlet 1-/2-sided) on: %s, off: %s\n", c.name,
                    on == off ? "identical" : ("DIFFER (" + base + "_on.png / _off.png)").c_str(), onLists.text.c_str(),
                    offLists.text.c_str());
        check(on == off, std::string("t06: ") + c.name + ": culling on = off");
        if (on == off) {
            std::remove((base + "_on.png").c_str());
            std::remove((base + "_off.png").c_str());
        }
    }
}

// ---- test 7: meshlets ---------------------------------------------------------------------------

using Geometry = GpuCulling::Geometry;
constexpr GpuCulling::Settings kMeshShaders{true, true, Geometry::Meshlets, true};
constexpr GpuCulling::Settings kFallback{true, true, Geometry::Meshlets, false};

uint32_t meshletDraws(const std::array<uint32_t, kGBufferLists>& c) { return c[2] + c[3] + c[6] + c[7]; }

uint32_t fallbackMeshlets(const GpuCulling::Capture& c) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < shared::kCullMeshletLists && shared::kCullLists + i < c.counts.size(); ++i)
        n += c.counts[shared::kCullLists + i];
    return n;
}

// Every draw of the occlusion scene on the meshlet path: task and mesh shaders, and the compute
// fallback, over 1, 2 and 4 frames (the early pass drawing from frame 2), with culling on and off.
void t07_mesh_shaders_and_fallback_render_the_occlusion_scene_identically() {
    std::printf("t07: the mesh-shader and fallback meshlet paths render the occlusion scene identically\n");
    for (const int frames : {1, 2, 4}) {
        const Run mesh = occlusionRun(kMeshShaders, frames), fallback = occlusionRun(kFallback, frames);
        if (mesh.image.empty()) {
            std::printf("  (no Vulkan device -- skipped)\n");
            return;
        }
        GpuCulling::Settings off = kFallback;
        off.frustum = off.occlusion = false;
        const Run fallbackOff = occlusionRun(off, frames);
        const int d = maxAbs(mesh.image, fallback.image), dOff = maxAbs(fallback.image, fallbackOff.image);
        std::printf("  %d frames: mesh vs fallback max_abs %d, fallback culling on vs off %d; meshlet draws %u, fallback "
                    "meshlets drawn %u of %u\n",
                    frames, d, dOff, meshletDraws(mesh.counts.back()), fallbackMeshlets(fallback.lists),
                    fallback.lists.meshletEntries);
        check(meshletDraws(mesh.counts.back()) > 0 && meshletDraws(fallback.counts.back()) > 0,
              "t07: geometry meshlets puts the draws on the meshlet lists");
        check(fallback.lists.meshletEntries > 0 && fallbackMeshlets(fallback.lists) > 0,
              "t07: the fallback culls and lists meshlets");
        check(d == 0, "t07: mesh shaders = fallback, occlusion scene, " + std::to_string(frames) + " frames");
        check(dOff == 0, "t07: meshlet culling on = off, occlusion scene, " + std::to_string(frames) + " frames");
    }
}

// The cone test on the GPU: the sphere's back-facing meshlets are dropped, by the task shader
// and by the fallback alike, and the image is the instance path's.
void t07_cone_culling_drops_back_facing_meshlets() {
    std::printf("t07: normal cones drop the one-sided sphere's back-facing meshlets\n");
    const GpuCulling::Settings mesh{true, true, Geometry::Auto, true}, fallback{true, true, Geometry::Auto, false},
        instances{true, true, Geometry::Instances, true};
    const Run m = sceneRun(Scene::Sphere, mesh, 2), f = sceneRun(Scene::Sphere, fallback, 2),
              i = sceneRun(Scene::Sphere, instances, 2);
    if (m.image.empty()) return;
    const uint32_t drawn = fallbackMeshlets(f.lists);
    std::printf("  fallback drew %u of the sphere's %u meshlets; mesh vs fallback max_abs %d, meshlets vs instances %d\n",
                drawn, f.meshlets, maxAbs(m.image, f.image), maxAbs(m.image, i.image));
    check(meshletDraws(m.counts.back()) == 1, "t07: geometry auto puts the 16,000-triangle sphere on the meshlet path");
    check(drawn > 0 && drawn < f.meshlets, "t07: the cone test drops the sphere's back-facing meshlets");
    check(maxAbs(m.image, f.image) == 0, "t07: sphere: mesh shaders = fallback");
    check(maxAbs(m.image, i.image) == 0, "t07: sphere: culled meshlets = the instance path's image");
}

void t07_fallback_twin_runs_build_identical_meshlet_lists() {
    std::printf("t07: two fresh renderers build byte-identical fallback meshlet lists\n");
    const Run a = occlusionRun(kFallback, 3), b = occlusionRun(kFallback, 3);
    if (a.image.empty()) return;
    check(!a.lists.meshletCommands.empty(), "t07: the fallback's meshlet commands were captured");
    check(a.lists.counts == b.lists.counts && a.lists.meshletCommands == b.lists.meshletCommands,
          "t07: twin runs: the same per-meshlet commands, byte for byte");
}

// The checked-in scenes on the meshlet path, mesh shaders against the fallback (PNG byte for
// byte), and the meshlets actually culled: the helmet (one-sided, 15,452 triangles) draws fewer
// meshlets than it has.
void t07_mesh_shaders_and_fallback_render_the_scene_files_identically() {
    std::printf("t07: the mesh-shader and fallback meshlet paths render the checked-in scenes identically\n");
    struct Case { const char* name; const char* tag; const char* path; const char* capture; Geometry geometry; };
    for (const Case& c : {Case{"cornell, every draw on meshlets", "cornell", "assets/scenes/cornell_box.scene.json", "plain",
                               Geometry::Meshlets},
                          Case{"helmet room, geometry auto, post", "helmet", "assets/scenes/helmet_game.scene.json", "post",
                               Geometry::Auto}}) {
        const std::string base = std::string(OHAO_BINARY_DIR) + "/meshlet_parity_" + c.tag + "_" +
                                 std::to_string(static_cast<unsigned long>(_getpid()));
        SceneLists meshLists, fallbackLists, instanceLists;
        const auto mesh = sceneCapture(c.path, c.capture, {true, true, c.geometry, true}, base + "_mesh.png", meshLists);
        const auto fallback =
            sceneCapture(c.path, c.capture, {true, true, c.geometry, false}, base + "_fallback.png", fallbackLists);
        const auto instances =
            sceneCapture(c.path, c.capture, {true, true, Geometry::Instances, true}, base + "_instances.png", instanceLists);
        if (mesh.empty() || fallback.empty() || instances.empty()) {
            check(false, std::string("t07: ") + c.name + ": the captures rendered");
            continue;
        }
        const uint32_t drawn = fallbackMeshlets(fallbackLists.capture);
        std::printf("  %s: mesh vs fallback PNGs %s, meshlets vs instances PNGs %s; lists mesh %s, fallback %s; fallback "
                    "drew %u meshlets of the meshlet draws' %u\n",
                    c.name, mesh == fallback ? "identical" : "DIFFER", mesh == instances ? "identical" : "differ",
                    meshLists.text.c_str(), fallbackLists.text.c_str(), drawn, fallbackLists.meshletDrawMeshlets);
        check(mesh == fallback, std::string("t07: ") + c.name + ": mesh shaders = fallback");
        check(drawn > 0 && drawn <= fallbackLists.meshletDrawMeshlets,
              std::string("t07: ") + c.name + ": the fallback draws meshlets of the meshlet draws only");
        check(mesh == instances, std::string("t07: ") + c.name + ": the meshlet path = the instance path");
        if (mesh == fallback) {
            std::remove((base + "_mesh.png").c_str());
            std::remove((base + "_fallback.png").c_str());
        }
        if (mesh == instances) std::remove((base + "_instances.png").c_str());
    }
}

// ---- test 8 (sub-project 10, t24_cull_parity_skinned) -------------------------------------------

// The rig with its arm raised (raise held at 1 s: the column above 0.5 m swings 1.5 m to -X), seen
// from where its bind box (x within 0.125 of 0) is off screen and only the raised arm is in view.
// `bind`: the same scene in the bind pose (nothing in view but the floor).
std::string skinnedOffscreenScene(bool bind) {
    const std::string rig = R"({
  "format": "ohao.scene",
  "version": 1,
  "components": { "Animator": 1, "Camera": 1, "Light": 1, "Material": 1, "MeshSource": 1, "Name": 1, "SkinnedMesh": 1, "Transform": 1 },
  "entities": [
    { "id": "a17b000000000001", "Name": { "value": "MainCamera" },
      "Transform": { "position": [-2.2, 0.62, 1.8] }, "Camera": { "fovDeg": 30 } },
    { "id": "a17b000000000002", "Name": { "value": "Floor" },
      "MeshSource": { "source": { "BoxPrim": { "lo": [-6, -0.1, -6], "hi": [6, 0, 4], "color": [0.6, 0.6, 0.6] } } },
      "Material": { "base_color": [0.6, 0.6, 0.6], "specular_roughness": 0.9 } },
    { "id": "a17b000000000003", "Name": { "value": "Sun" },
      "Transform": { "rotation": [-0.40822, 0.23457, 0.10938, 0.87543] },
      "Light": { "type": "Directional", "color": [1, 0.95, 0.85], "intensity": 3 } },
    { "id": "a17b000000000004", "Name": { "value": "Rig" },
      "MeshSource": { "source": { "ModelAsset": { "asset": { "path": "tests/anim/fixtures/rig.glb" }, "mesh": "Column" } } },
      "SkinnedMesh": { "file": { "path": "tests/anim/fixtures/rig.glb" }, "skin": "RigSkin" },
      "Animator": { "graph": { "path": "tests/anim/fixtures/rig.animgraph.json" },
        "layers": [ { "state": ")" + std::string(bind ? "wave" : "held") + R"(", "steps": )" + (bind ? "0" : "1") +
                            R"( }, { "state": "still", "steps": 0 } ] } }
  ]
})";
    const std::string path = std::string(OHAO_BINARY_DIR) + "/cull_parity_rig_" + (bind ? "bind_" : "raised_") +
                             std::to_string(static_cast<unsigned long>(_getpid())) + ".scene.json";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(rig.data(), 1, rig.size(), f);
        std::fclose(f);
    }
    return path;
}

void t08_skinned_arm_outside_its_bind_box_is_drawn() {
    std::printf("t08: a skinned rig whose bind box is off screen and whose raised arm is in view\n");
    const std::string raised = skinnedOffscreenScene(false), bind = skinnedOffscreenScene(true);
    const std::string base = std::string(OHAO_BINARY_DIR) + "/cull_parity_rig_" +
                             std::to_string(static_cast<unsigned long>(_getpid()));
    SceneLists l1, l2, l3, l4, l5;
    const auto on = sceneCapture(raised.c_str(), "plain", {true, true}, base + "_on.png", l1);
    const auto off = sceneCapture(raised.c_str(), "plain", {false, false}, base + "_off.png", l2);
    const auto mesh = sceneCapture(raised.c_str(), "plain", kMeshShaders, base + "_mesh.png", l3);
    const auto fallback = sceneCapture(raised.c_str(), "plain", kFallback, base + "_fallback.png", l4);
    const auto bound = sceneCapture(bind.c_str(), "plain", {true, true}, base + "_bind.png", l5);
    if (on.empty() || off.empty() || mesh.empty() || fallback.empty() || bound.empty()) {
        check(false, "t08: the rig captures rendered");
        return;
    }
    std::printf("  raised: culling on vs off %s, mesh shaders %s, fallback %s; against the bind pose %s\n",
                on == off ? "identical" : "DIFFER", mesh == on ? "identical" : "DIFFER",
                fallback == on ? "identical" : "DIFFER", on == bound ? "IDENTICAL (arm not drawn)" : "differs (arm drawn)");
    check(on == off, "t08: the raised rig: culling on = off");
    check(mesh == on, "t08: the raised rig on the meshlet path (mesh shaders) = the instance path");
    check(fallback == on, "t08: the raised rig on the meshlet fallback = the instance path");
    check(on != bound, "t08: the raised arm is drawn though the bind box is off screen");
    for (const char* s : {"_on.png", "_off.png", "_mesh.png", "_fallback.png", "_bind.png"}) std::remove((base + s).c_str());
    std::remove(raised.c_str());
    std::remove(bind.c_str());
}

} // namespace

int main() {
    t06_parity_occlusion_scene();
    t06_twin_runs_build_identical_lists();
    t06_occluded_and_offscreen_boxes_are_not_drawn();
    t06_face_culling_changes_no_pixel();
    t06_parity_scene_files();
    t07_mesh_shaders_and_fallback_render_the_occlusion_scene_identically();
    t07_cone_culling_drops_back_facing_meshlets();
    t07_fallback_twin_runs_build_identical_meshlet_lists();
    t07_mesh_shaders_and_fallback_render_the_scene_files_identically();
    t08_skinned_arm_outside_its_bind_box_is_drawn();
    return test::finish("cull_parity_gpu_test", g_passed, g_failed, g_failures);
}
