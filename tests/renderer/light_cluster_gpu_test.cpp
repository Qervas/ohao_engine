// light_cluster_gpu_test (sub-project 2b, test 8): clustered lighting sums exactly the lights the
// full loop sums, renders a thousand lights, reports froxels that overflow, and builds the same
// froxel lists every run.
//
//   t08 exact      every pixel summing every light (the pre-2b loop, as a switch) and the froxel
//                  lists give identical PNGs, byte for byte: Cornell (12 lights, RT shadows),
//                  the helmet room (<= 8 lights, post effects) and the bench scene's 1,000 point
//                  lights (4 shadowed) and sun, a wall lit 1,500 m away under a 3,000 m far plane
//                  (the slices reach the camera's far plane), and an orthographic camera (no
//                  froxels: every light). The <= 8-light scenes against images made before
//                  the step are the deferred goldens (render_golden.py, max_abs 0).
//   t08 lists      the bench scene's froxels hold fewer lights than the table: the lists cull.
//   t08 overflow   1,000 lights packed into a 3 m cube: froxels past 256 lights keep their first
//                  256, the frame renders, and render.stats reports lightsOverflow (absent for
//                  the bench scene, where no froxel overflows).
//   t08 twin runs  two fresh renderers build byte-identical froxel lists (counts and indices).
//
// Exit code 0 = pass; the last line is a JSON summary.
#include "common/materials.hpp"
#include "api/session.hpp"
#include "bench/bench_world.hpp"
#include "common/harness.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/spawn.hpp"
#include "ecs/world.hpp"
#include "gpu/shared_layout.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/light_clusters.hpp"
#include "render/camera/camera.hpp"
#include "render/scene/render_scene.hpp"
#include "runtime/image.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <process.h>   // _getpid

using namespace ohao;
namespace shared = gpu::shared;

namespace {

bool g_keep = false;   // --keep: leave the compared PNGs in build/ to look at
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

std::vector<uint8_t> readFile(const std::string& p) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        uint8_t buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}

// Per-process names under build/: CTest may run another copy of this test.
std::string scratch(const std::string& name) {
    return "build/light_cluster_" + name + "_" + std::to_string(static_cast<unsigned long>(_getpid()));
}

// A built world saved as a scene file (relative to the source root, where the asset resolver
// looks), so it renders through the CLI's stack like the checked-in scenes.
bool writeScene(const ecs::World& w, const std::string& rel) {
    auto text = ecs::saveScene(w, engineTypes(), ecs::SaveOptions{reflect::JsonLayout::Compact});
    if (!text) return false;
    std::ofstream(std::string(OHAO_SOURCE_DIR) + "/" + rel, std::ios::binary | std::ios::trunc) << text.value();
    return true;
}

// The bench scene's lights over a smaller grid (1,600 renderables): render_bench's
// bench*_1080p light set, 1,000 point lights (range 15 m, 4 shadowed) and the sun.
std::string benchLightsScene() {
    static const std::string rel = scratch("bench") + ".scene.json";
    static const bool ok = [] {
        ecs::World w;
        bench::buildRenderBenchWorld(w, 1600, 2026);
        return writeScene(w, rel);
    }();
    return ok ? rel : std::string{};
}

// 1,000 point lights packed into a 3 m cube (10 x 10 x 10, 0.3 m apart, range 8 m) in front of
// the camera, over a floor: every froxel around them is reached by all 1,000.
std::string packedLightsScene() {
    static const std::string rel = scratch("packed") + ".scene.json";
    static const bool ok = [] {
        using namespace ecs;
        World w;
        w.seedIds(8);
        const auto floor = w.create("Floor", Transform{});
        w.registry().emplace<MeshSource>(floor, MeshSource{PlatformPrim{40.0f, 0.2f, 40.0f}});
        w.registry().emplace<Material>(floor, ohao::test::material({0.6f, 0.6f, 0.6f}, 0.7f, 0.0f, 0.0f));
        for (int i = 0; i < 1000; ++i) {
            Transform t;
            t.position = {-1.35f + 0.3f * static_cast<float>(i % 10), 0.5f + 0.3f * static_cast<float>(i / 10 % 10),
                          -1.35f + 0.3f * static_cast<float>(i / 100)};
            const auto e = w.create("Light_" + std::to_string(i), t);
            Light l;
            l.color = {0.5f + 0.05f * static_cast<float>(i % 10), 0.8f, 1.0f - 0.05f * static_cast<float>(i / 100)};
            l.intensity = 0.05f;
            l.range = 8.0f;
            l.castShadows = false;
            w.registry().emplace<Light>(e, l);
        }
        Transform cam;
        cam.position = {0.0f, 3.0f, 9.0f};
        cam.rotation = glm::angleAxis(glm::radians(-15.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const auto c = w.create("MainCamera", cam);
        w.registry().emplace<ecs::Camera>(c, ecs::Camera{50.0f, 0.1f, 200.0f, true});
        return writeScene(w, rel);
    }();
    return ok ? rel : std::string{};
}

// A wall 1,500 m away under a camera whose far plane is 3,000 m, lit by a light 20 m in front
// of it: the froxel slices must reach the camera's far plane, not a fixed distance.
std::string farWallScene() {
    static const std::string rel = scratch("far") + ".scene.json";
    static const bool ok = [] {
        using namespace ecs;
        World w;
        w.seedIds(9);
        Transform wallT;
        wallT.position = {0.0f, 0.0f, -1500.0f};
        const auto wall = w.create("Wall", wallT);
        w.registry().emplace<MeshSource>(
            wall, MeshSource{BoxPrim{glm::vec3(-400.0f, -400.0f, -1.0f), glm::vec3(400.0f, 400.0f, 1.0f), glm::vec3(0.7f)}});
        w.registry().emplace<Material>(wall, ohao::test::material({0.7f, 0.7f, 0.7f}, 0.8f, 0.0f, 0.0f));
        Transform lt;
        lt.position = {0.0f, 0.0f, -1480.0f};
        const auto l = w.create("Light", lt);
        Light light;
        light.intensity = 5000.0f;
        light.range = 150.0f;
        light.castShadows = false;
        w.registry().emplace<Light>(l, light);
        const auto c = w.create("MainCamera", Transform{});
        w.registry().emplace<ecs::Camera>(c, ecs::Camera{20.0f, 0.1f, 3000.0f, true});
        return writeScene(w, rel);
    }();
    return ok ? rel : std::string{};
}

struct Capture {
    std::vector<uint8_t> png;
    std::vector<uint32_t> lists;        // the last frame's froxel buffer: counts, then indices
    uint32_t lightCount{0};              // the raster light table's
    std::optional<LightClusters::Overflow> overflow;
    std::string stats;                   // render.stats' reply
    std::vector<shared::LightGpu> table;  // the raster light table the lighting read
};

// A scene captured as `ohao run` captures it (host::RenderHost + api::Session), 480x270
// deferred, `frames` frames (`post`: TAA, SSAO, bloom), lighting clustered or not.
Capture capture(const std::string& scenePath, bool clustered, int frames, bool post, const std::string& png) {
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    host::RenderHost host(assets, api::HostKind::Headless);
    Capture out;
    {
        ecs::World world;
        api::Session session(world, engineTypes());
        session.setAssetResolver(&assets);
        host.bind(world);
        session.setHost(&host);
        const std::string loaded =
            session.call("scene.load", std::string(R"({"replace":true,"path":")") + scenePath + "\"}");
        if (loaded.rfind("{\"result\"", 0) != 0) {
            std::printf("  scene.load %s: %s\n", scenePath.c_str(), loaded.c_str());
            return out;
        }
        // A one-frame capture starts the renderer; then the switches, then the capture.
        api::RenderRequest warm;
        warm.mode = "deferred";
        warm.frames = 1;
        warm.width = 480;
        warm.height = 270;
        (void)host.capture(warm);
        if (!host.renderer()) return out;
        DeferredRenderer* deferred = host.renderer()->getDeferredRenderer();
        deferred->setClusteredLighting(clustered);
        // The loop lights every light only without RT direct light (which takes all but flashes).
        host.renderer()->setRTDirectEnabled(false);
        deferred->lightClusters()->setCapture(true);
        api::RenderRequest r;
        r.mode = "deferred";
        r.width = 480;
        r.height = 270;
        r.frames = frames;
        if (post) {
            r.post = {"taa", "ssao", "bloom"};
            r.ev100 = 0.0f;
        }
        r.path = png;
        const api::RenderOutcome o = host.capture(r);
        if (!o.errors.empty()) return out;
        out.png = readFile(png);
        LightClusters* lc = deferred->lightClusters();
        out.lists = lc->capture();
        // The slot the last frame used holds the table it read: every slot holds the same lights
        // once the scene stood still for a frame per slot.
        out.lightCount = lc->lightCount(0);
        if (const shared::LightGpu* t = lc->lights(0)) out.table.assign(t, t + out.lightCount);
        out.overflow = host.renderer()->lightsOverflow();
        out.stats = session.call("render.stats", "{}");
    }
    return out;
}

void removeIf(bool cond, const std::string& p) {
    if (cond && !g_keep) std::remove(p.c_str());
}

void t08_clustered_lighting_equals_the_full_loop() {
    std::printf("t08: the froxel lists light every pixel exactly as summing every light does\n");
    struct Case { const char* name; const char* tag; std::string path; int frames; bool post; };
    const Case cases[] = {
        {"cornell (12 lights, RT shadows), 10 frames", "cornell", "assets/scenes/cornell_box.scene.json", 10, false},
        {"helmet room (<= 8 lights), 30 frames with post", "helmet", "assets/scenes/helmet_game.scene.json", 30, true},
        {"bench lights (1,000 point + sun), 10 frames", "bench", benchLightsScene(), 10, false},
        {"a wall lit 1,500 m away (far plane 3,000 m), 2 frames", "far", farWallScene(), 2, false},
    };
    for (const Case& c : cases) {
        if (c.path.empty()) {
            check(false, std::string("t08: ") + c.name + ": the scene was written");
            continue;
        }
        const std::string base = std::string(OHAO_SOURCE_DIR) + "/" + scratch(c.tag);
        const Capture clustered = capture(c.path, true, c.frames, c.post, base + "_clustered.png");
        const Capture full = capture(c.path, false, c.frames, c.post, base + "_full.png");
        if (clustered.png.empty() || full.png.empty()) {
            check(false, std::string("t08: ") + c.name + ": the captures rendered");
            continue;
        }
        const bool same = clustered.png == full.png;
        std::printf("  %s: %u lights, PNGs %s\n", c.name, clustered.lightCount,
                    same ? "identical" : ("DIFFER (" + base + "_clustered.png / _full.png)").c_str());
        check(same, std::string("t08: ") + c.name + ": clustered = every light, byte for byte");
        check(full.lists.empty(), std::string("t08: ") + c.name + ": without clusters no froxel list is built");
        removeIf(same, base + "_clustered.png");
        removeIf(same, base + "_full.png");
    }
}

// The bench scene through the lists: what a froxel holds, and nothing overflowing.
void t08_bench_lights_froxels_cull_and_never_overflow() {
    std::printf("t08: the bench scene's froxels hold a few of its 1,001 lights, none past 256\n");
    const std::string path = benchLightsScene();
    if (path.empty()) return;
    const std::string png = std::string(OHAO_SOURCE_DIR) + "/" + scratch("bench_lists") + ".png";
    const Capture run = capture(path, true, 2, false, png);
    std::remove(png.c_str());
    if (run.png.empty() || run.lists.size() < shared::kClusterCount) {
        check(false, "t08: bench lights: the capture rendered with lists");
        return;
    }
    uint32_t maxCount = 0, nonEmpty = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < shared::kClusterCount; ++i) {
        maxCount = std::max(maxCount, run.lists[i]);
        nonEmpty += run.lists[i] > 0;
        total += run.lists[i];
    }
    std::printf("  %u lights; froxels: %u of %u non-empty, %.1f lights on average there, at most %u; overflow %s; "
                "render.stats %s\n",
                run.lightCount, nonEmpty, shared::kClusterCount, nonEmpty ? double(total) / nonEmpty : 0.0, maxCount,
                run.overflow ? std::to_string(run.overflow->lights).c_str() : "none", run.stats.c_str());
    check(run.lightCount == 1001, "t08: bench lights: 1,000 points and the sun in the light table");
    check(maxCount > 1 && maxCount < run.lightCount, "t08: bench lights: a froxel holds some of the lights, not all");
    check(maxCount <= shared::kClusterMaxLights, "t08: bench lights: no froxel past 256");
    check(run.overflow && run.overflow->lights == 0 && run.overflow->maxLights == maxCount,
          "t08: bench lights: the stats agree (no overflow, the largest froxel)");
    check(run.stats.find("\"lightsOverflow\"") == std::string::npos, "t08: bench lights: render.stats has no lightsOverflow");
    // The froxel entries point into the table, in light-index order (the full loop's order, so the
    // sums add in the same order).
    bool ordered = true;
    for (uint32_t i = 0; i < shared::kClusterCount && ordered; ++i) {
        const uint32_t* list = run.lists.data() + shared::kClusterCount + std::size_t{i} * shared::kClusterMaxLights;
        for (uint32_t k = 0; k < run.lists[i] && ordered; ++k)
            ordered = list[k] < run.lightCount && (k == 0 || list[k] > list[k - 1]);
    }
    check(ordered, "t08: bench lights: every list holds valid light indices, ascending");
}

void t08_thousand_packed_lights_overflow_is_reported() {
    std::printf("t08: 1,000 lights in one place: froxels keep 256, the frame renders, render.stats reports the rest\n");
    const std::string path = packedLightsScene();
    if (path.empty()) {
        check(false, "t08: packed lights: the scene was written");
        return;
    }
    const std::string png = std::string(OHAO_SOURCE_DIR) + "/" + scratch("packed") + ".png";
    const Capture run = capture(path, true, 2, false, png);
    if (run.png.empty() || run.lists.size() < shared::kClusterCount) {
        check(false, "t08: packed lights: the capture rendered with lists");
        return;
    }
    uint32_t full = 0, maxCount = 0;
    for (uint32_t i = 0; i < shared::kClusterCount; ++i) {
        full += run.lists[i] == shared::kClusterMaxLights;
        maxCount = std::max(maxCount, run.lists[i]);
    }
    const bool reported = run.stats.find("\"lightsOverflow\":") != std::string::npos;
    std::printf("  %u lights; %u froxels full (256); overflow: %u lights dropped in %u froxels, largest %u; "
                "render.stats %s\n",
                run.lightCount, full, run.overflow ? run.overflow->lights : 0u, run.overflow ? run.overflow->clusters : 0u,
                run.overflow ? run.overflow->maxLights : 0u, run.stats.c_str());
    check(run.lightCount == 1000, "t08: packed lights: 1,000 lights in the table");
    check(run.png.size() > 1000, "t08: packed lights: the frame rendered");
    check(maxCount == shared::kClusterMaxLights && full > 0, "t08: packed lights: full froxels keep 256 lights");
    // A froxel reached by exactly 256 is full without overflowing; one past it dropped a light at least.
    check(run.overflow && run.overflow->clusters > 0 && run.overflow->clusters <= full &&
              run.overflow->lights >= run.overflow->clusters && run.overflow->maxLights == 1000,
          "t08: packed lights: the overflow counts the froxels past 256, the lights they dropped, the largest (all 1,000)");
    check(reported, "t08: packed lights: render.stats reports lightsOverflow");
    std::remove(png.c_str());
}

void t08_twin_runs_build_identical_froxel_lists() {
    std::printf("t08: two fresh renderers build byte-identical froxel lists\n");
    const std::string path = benchLightsScene();
    if (path.empty()) return;
    const std::string base = std::string(OHAO_SOURCE_DIR) + "/" + scratch("twin");
    const Capture a = capture(path, true, 3, false, base + "_a.png");
    const Capture b = capture(path, true, 3, false, base + "_b.png");
    std::remove((base + "_a.png").c_str());
    std::remove((base + "_b.png").c_str());
    if (a.lists.empty()) {
        check(false, "t08: twin runs: the lists were captured");
        return;
    }
    check(a.lists == b.lists, "t08: twin runs: the same froxel counts and indices, word for word");
    check(a.png == b.png, "t08: twin runs: the same image");
}

// An orthographic camera over a floor lit by 16 small lights, through VulkanRenderer directly
// (scene cameras are perspective): froxels are slices of a perspective frustum, so lighting
// sums every light instead, and the image is the full loop's.
std::vector<uint8_t> orthographicRun(bool clustered) {
    std::vector<uint8_t> out;
    VulkanRenderer r(320, 180);
    if (!r.initialize()) return out;
    {
        ecs::World world;   // after the renderer: released first
        ecs::installRenderExtract(world, r.renderScene());
        Model floor;
        PrimitiveMeshGenerator::generateBox(floor, {-10.0f, -0.1f, -10.0f}, {10.0f, 0.0f, 10.0f}, {1, 1, 1});
        render::MaterialDesc grey;
        grey.baseColor = {0.7f, 0.7f, 0.7f};
        ecs::spawnModel(world, r.renderScene(), r.getTextureManager(), floor, "floor", {}, &grey);
        for (int i = 0; i < 16; ++i) {
            ecs::Light light;
            light.color = {0.4f + 0.04f * static_cast<float>(i), 0.8f, 1.0f - 0.04f * static_cast<float>(i)};
            light.intensity = 3.0f;
            light.range = 1.0f;
            light.castShadows = false;
            ecs::spawnLight(world, "light", light,
                            ecs::Transform{{-7.5f + 5.0f * static_cast<float>(i % 4), 0.05f, -3.75f + 2.5f * static_cast<float>(i / 4)}});
        }
        // 40 m above the floor, looking down (glm::ortho keeps GL's -1..1 depth, so geometry
        // nearer than the planes' midpoint is clipped): perspective froxels at that depth would
        // sit 40 times farther from the view's centre than the pixels they hold, and miss
        // every light away from the centre.
        r.getCamera().setPosition({0.0f, 40.0f, 0.0f});
        r.getCamera().setRotation(-89.0f, -90.0f);
        r.getCamera().setOrthographicProjection(-9.0f, 9.0f, -5.0f, 5.0f, 0.1f, 50.0f);
        r.setRenderMode(RenderMode::Deferred);
        r.getDeferredRenderer()->setClusteredLighting(clustered);
        world.sync();
        r.resetAccumulation();
        for (int f = 0; f < 2; ++f) {
            world.sync();
            if (f == 1) r.requestReadback();
            r.render();
            r.waitForSubmitted(r.framesSubmitted());
        }
        const auto px = r.readback();
        out.assign(px.begin(), px.end());
    }
    return out;
}

void t08_orthographic_camera_lights_every_pixel_fully() {
    std::printf("t08: an orthographic camera renders the full loop's image\n");
    const auto clustered = orthographicRun(true), full = orthographicRun(false);
    if (clustered.empty() || full.empty()) {
        check(false, "t08: orthographic: the frames rendered");
        return;
    }
    int m = 0;
    for (std::size_t i = 0; i < clustered.size() && i < full.size(); ++i)
        m = std::max(m, std::abs(int(clustered[i]) - int(full[i])));
    double mean = 0.0;
    for (const uint8_t v : full) mean += v;
    mean /= static_cast<double>(full.size());
    std::printf("  clustered switch on vs off: max_abs %d (mean %.1f of %zu bytes)\n", m, mean, full.size());
    if (g_keep) {
        (void)runtime::writePng(std::string(OHAO_SOURCE_DIR) + "/" + scratch("ortho_full") + ".png", full, 320, 180);
        (void)runtime::writePng(std::string(OHAO_SOURCE_DIR) + "/" + scratch("ortho_clustered") + ".png", clustered, 320, 180);
    }
    check(clustered.size() == full.size() && m == 0, "t08: orthographic: the same image as every light");
}

} // namespace

int main(int argc, char** argv) {
    g_keep = argc > 1 && std::strcmp(argv[1], "--keep") == 0;
    t08_clustered_lighting_equals_the_full_loop();
    t08_bench_lights_froxels_cull_and_never_overflow();
    t08_thousand_packed_lights_overflow_is_reported();
    t08_twin_runs_build_identical_froxel_lists();
    t08_orthographic_camera_lights_every_pixel_fully();
    for (const std::string& s : {benchLightsScene(), packedLightsScene(), farWallScene()})
        if (!s.empty()) std::remove((std::string(OHAO_SOURCE_DIR) + "/" + s).c_str());
    return test::finish("light_cluster_gpu_test", g_passed, g_failed, g_failures);
}
