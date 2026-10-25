// cull_parity_gpu_test (sub-project 2b, test 6): GPU culling changes what is drawn, never what
// is seen, and builds its lists the same way every run.
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
//
// Exit code 0 = pass; the last line is a JSON summary.
#include "api/session.hpp"
#include "common/harness.hpp"
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
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

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

struct Run {
    std::vector<uint8_t> image;
    GpuCulling::Capture lists;   // the last frame's
    std::vector<std::array<uint32_t, 4>> counts;   // per frame: early 1-/2-sided, late 1-/2-sided
};

// `frames` frames of the occlusion scene from a reset, culling as `s`, the last read back.
Run occlusionRun(GpuCulling::Settings s, int frames, bool oneSided = false) {
    Run out;
    VulkanRenderer r(320, 180);
    if (!r.initialize()) return out;
    {
        ecs::World world;   // after the renderer: released first
        ecs::installRenderExtract(world, r.renderScene());
        buildOcclusionScene(world, r, oneSided);
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
            if (out.lists.counts.size() >= 4)
                out.counts.push_back({out.lists.counts[0], out.lists.counts[1], out.lists.counts[2], out.lists.counts[3]});
        }
        const auto px = r.readback();
        out.image.assign(px.begin(), px.end());
    }
    return out;
}

std::string countsText(const Run& run) {
    std::string s;
    for (const auto& c : run.counts)
        s += "[" + std::to_string(c[0] + c[1]) + "+" + std::to_string(c[2] + c[3]) + "]";
    return s;
}

// The draws listed by `list` (their draw-table indices: firstInstance).
std::vector<uint32_t> listed(const GpuCulling::Capture& c, uint32_t list) {
    std::vector<uint32_t> out;
    if (c.counts.size() <= list) return out;
    const std::size_t base = std::size_t{list} * c.drawCount * 5;
    for (uint32_t k = 0; k < c.counts[list]; ++k) out.push_back(c.commands[base + k * 5 + 4]);
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
    // Draw order: floor 0, wall 1, hidden 2..6, seen 7..9, off screen 10..11.
    std::vector<int> drawn(run.lists.drawCount, 0);
    for (uint32_t list = 0; list < 4; ++list)
        for (const uint32_t d : listed(run.lists, list)) ++drawn[d];
    const int firstHidden = 2, firstSeen = firstHidden + kHidden, firstOff = firstSeen + kSeen;
    bool hidden = true, seen = true, off = true;
    for (int i = 0; i < kHidden; ++i) hidden = hidden && drawn[firstHidden + i] == 0;
    for (int i = 0; i < kSeen; ++i) seen = seen && drawn[firstSeen + i] == 1;
    for (int i = 0; i < kOffscreen; ++i) off = off && drawn[firstOff + i] == 0;
    std::printf("  frame 3 draws: %s (of %u)\n", countsText(run).c_str(), run.lists.drawCount);
    check(run.lists.drawCount == 2u + kHidden + kSeen + kOffscreen, "t06: one draw per instance");
    check(drawn[0] == 1 && drawn[1] == 1, "t06: the floor and the wall are drawn once");
    check(hidden, "t06: the boxes behind the wall are not drawn (occluded)");
    check(seen, "t06: each box in view is drawn exactly once (early or late)");
    check(off, "t06: the boxes off screen are not drawn (frustum)");
    const auto& c = run.counts.back();
    check(c[0] + c[1] > 0, "t06: from frame 2 the early pass draws last frame's visible set");
}

void t06_face_culling_changes_no_pixel() {
    std::printf("t06: one-sided materials (back faces culled) render closed meshes unchanged\n");
    const Run doubleSided = occlusionRun({true, true}, 2, false);
    const Run oneSided = occlusionRun({true, true}, 2, true);
    if (doubleSided.image.empty()) return;
    const int d = maxAbs(doubleSided.image, oneSided.image);
    const auto& c = oneSided.counts.back();
    std::printf("  max_abs %d; one-sided run's lists: %u one-sided + %u double-sided early, %u + %u late\n", d, c[0], c[1],
                c[2], c[3]);
    check(c[0] + c[2] > 0 && c[1] + c[3] == 0, "t06: one-sided draws use the face-culled list");
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

// A checked-in scene captured as `ohao run` captures it (the CLI's stack), culling as `s`; the
// PNG (OHAO's deterministic encoder: equal files are equal images) written to `png`.
std::vector<uint8_t> sceneCapture(const char* path, const char* capture, GpuCulling::Settings s, const std::string& png,
                                  std::string& lists) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    std::vector<uint8_t> out;
    {
        ecs::World world;
        api::Session session(world, engineTypes());
        session.setAssetResolver(&assets);
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
            r.post = {"taa", "ssao", "bloom"};
            r.exposure = 1.0f;
        } else {
            r.frames = 10;
        }
        r.path = png;
        const api::RenderOutcome o = host.capture(r);
        if (!o.errors.empty()) return out;
        out = readFile(png);
        const GpuCulling::Capture c = culling->capture();
        for (uint32_t list = 0; list < 4 && list < c.counts.size(); ++list) lists += std::to_string(c.counts[list]) + " ";
        lists += "of " + std::to_string(c.drawCount);
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
        std::string onLists, offLists;
        const auto on = sceneCapture(c.path, c.capture, {true, true}, base + "_on.png", onLists);
        const auto off = sceneCapture(c.path, c.capture, {false, false}, base + "_off.png", offLists);
        if (on.empty() || off.empty()) {
            check(false, std::string("t06: ") + c.name + ": the capture rendered");
            continue;
        }
        std::printf("  %s: PNGs %s; last frame's lists (early 1-/2-sided, late 1-/2-sided) on: %s, off: %s\n", c.name,
                    on == off ? "identical" : ("DIFFER (" + base + "_on.png / _off.png)").c_str(), onLists.c_str(),
                    offLists.c_str());
        check(on == off, std::string("t06: ") + c.name + ": culling on = off");
        if (on == off) {
            std::remove((base + "_on.png").c_str());
            std::remove((base + "_off.png").c_str());
        }
    }
}

} // namespace

int main() {
    t06_parity_occlusion_scene();
    t06_twin_runs_build_identical_lists();
    t06_occluded_and_offscreen_boxes_are_not_drawn();
    t06_face_culling_changes_no_pixel();
    t06_parity_scene_files();
    return test::finish("cull_parity_gpu_test", g_passed, g_failed, g_failures);
}
