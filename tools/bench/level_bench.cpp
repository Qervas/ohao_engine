// level_bench: the level pipeline's budgets (docs/specs/2026-10-26-level-pipeline-design.md,
// "Performance budgets").
//
//   level_bench [--check budgets.json] [--out build/level_bench.json] [--only blockout|kit2k|tidewall_nav]
//               [--rounds N] [--reference build/serve_bench_reference.json] [--threads N]
//
// Step 4 measures blockout_load_ms: a cold load of assets/levels/blockout_room.glb into a
// fresh World with the level and physics systems (structure, 13 meshes through a fresh asset
// resolver, expansion, colliders; no GPU upload), median and p95 of N rounds (30) after 5
// warm-ups. The OS file cache is warm after the warm-ups; the engine's caches are not.
// level_info_bytes (step 5): the size of level.info's result on the blockout, through a Session
// as an agent calls it; a size, so a noisy machine cannot excuse a miss. `--only kit2k` measures
// the generated kit's four numbers instead (measureKit), one load after a warming one. `--only tidewall_nav` measures
// the navigation module on Tidewall's yard (runNav): its navmesh cooked cold and after one edited piece, loaded from
// the cooked cache, a path query, an obstacle change, and 16 and 64 walkers' step, at --threads (else OHAO_THREADS).
//
// The machine is recorded as scene_bench records it (bench_env.hpp): a calibration loop before
// and after against the reference, and every other process's CPU. A run >25% slower than the
// reference or beside other processes using more than half a core is `noisy`: exit 2, neither
// a pass nor a regression. Logging is at Warn during the rounds: the per-mesh Info lines would
// time the console, not the loader.
#include "api/session.hpp"
#include "common/harness.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "physics/physics_scene.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/module.hpp"
#include "engine/install.hpp"
#include "ecs/world.hpp"
#include "level/level_system.hpp"
#include "level/components.hpp"
#include "navigation/components.hpp"
#include "navigation/navigation.hpp"
#include "core/jobs/pool.hpp"
#include "reflect/registry.hpp"

#include <nlohmann/json.hpp>

#include "bench_env.hpp"   // after the engine headers: it includes windows.h

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ohao;

namespace {

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (engine::registerTypes(r), true);
    (void)once;
    return r;
}

struct Stats {
    double median{0.0}, p95{0.0}, min{0.0};
};

Stats stats(std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * double(v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95), v.front()};
}

struct LoadResult {
    double ms{0.0};
    ecs::PhysicsInfo info;
    std::size_t nodes{0};
};

// One cold load of the blockout: nothing cached in the engine.
LoadResult loadBlockout() {
    const auto t0 = std::chrono::steady_clock::now();
    ecs::World world;
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    engine::install(world, {files, engineTypes()}, {"level", "physics"});
    ecs::PhysicsScene& physics = *ecs::physicsScene(world);
    const entt::entity lv = world.createWithId(0x1e7e1, "Level");
    world.registry().emplace<ecs::Level>(lv, ecs::Level{ecs::AssetRef{"assets/levels/blockout_room.glb", {}}});
    world.sync();
    LoadResult r;
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.info = physics.info();
    r.nodes = world.registry().view<ecs::LevelNode>().size();
    return r;
}

// kit2k (tools/level/kit_level.py: 2,000 nodes over 40 meshes, 998,560 triangles, every node a static mesh
// collider): one cold load with the level and physics systems, the part of it that built the colliders, 10,000
// rays down into the kit, and the two systems' sync on the still world.
struct KitResult {
    double loadMs{0.0}, collisionMs{0.0}, rayUs{0.0}, idleUs{0.0};
    std::size_t nodes{0}, hits{0};
    uint32_t statics{0};
};

KitResult measureKit() {
    KitResult r;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto t0 = std::chrono::steady_clock::now();
    ecs::World world;
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    engine::install(world, {files, engineTypes()}, {"level", "physics"});
    ecs::PhysicsScene& physics = *ecs::physicsScene(world);
    const entt::entity lv = world.createWithId(0x1e7e1, "Level");
    world.registry().emplace<ecs::Level>(lv, ecs::Level{ecs::AssetRef{"build/kit2k/kit2k.glb", {}}});
    world.sync();
    r.loadMs = ms(t0, std::chrono::steady_clock::now());
    r.collisionMs = physics.info().buildMs;
    r.nodes = world.registry().view<ecs::LevelNode>().size();
    r.statics = physics.info().bodies.statics;

    // Rays straight down over the kit's 68 m square, from a fixed sequence.
    std::vector<double> rays;
    uint32_t seed = 0x9e3779b9u;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (int i = 0; i < 10000; ++i) {
        const glm::vec3 from{(next() - 0.5f) * 68.0f, 10.0f, (next() - 0.5f) * 68.0f};
        const auto a = std::chrono::steady_clock::now();
        const auto hit = physics.raycast(from, {0.0f, -1.0f, 0.0f}, 20.0f);
        rays.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
        r.hits += hit ? 1 : 0;
    }
    r.rayUs = stats(rays).median;
    std::vector<double> idle;
    for (int i = 0; i < 500; ++i) {
        const auto a = std::chrono::steady_clock::now();
        world.sync();
        idle.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
    }
    r.idleUs = stats(idle).median;
    return r;
}

// The kit's four numbers against their budgets; the process's exit code.
int runKit(const nlohmann::json& budgets, int currentStep, const std::string& outPath) {
    if (!std::filesystem::exists(std::filesystem::path(OHAO_SOURCE_DIR) / "build/kit2k/kit2k.glb")) {
        std::fprintf(stderr, "level_bench: build/kit2k/kit2k.glb is not there; run tools/level/kit_level.py\n");
        return 2;
    }
    log::setLevel(log::Level::Warn);
    const KitResult first = measureKit();   // cooks what the cache lacks; the OS file cache warms
    const KitResult r = measureKit();
    log::setLevel(log::Level::Info);
    std::vector<std::string> failures;
    // A benchmark of broken code fails: the kit's nodes and bodies, and how many of the fixed rays hit (3,696 with
    // every collider built from its own scaled vertices, before shapes were shared).
    if (r.nodes != 2001 || r.statics != 2000 || r.hits != 3696)
        failures.push_back("kit2k did not load as expected: " + std::to_string(r.nodes) + " nodes, " +
                           std::to_string(r.statics) + " static bodies, " + std::to_string(r.hits) +
                           " rays hit (want 2001, 2000, 3696)");
    nlohmann::json out;
    std::printf("level_bench kit2k (%zu nodes, %u static bodies, %zu of 10000 rays hit)\n", r.nodes, r.statics, r.hits);
    const auto row = [&](const char* name, double value, const char* unit) {
        std::string verdict = "reported";
        if (budgets.contains(name)) {
            const double max = budgets[name].value("max", 0.0);
            verdict = currentStep < budgets[name].value("gatedFrom", 0) ? "pending" : value <= max ? "pass" : "fail";
            if (verdict == "fail") {
                char text[160];
                std::snprintf(text, sizeof text, "%s: %.3f %s over its budget of %.3f", name, value, unit, max);
                failures.push_back(text);
            }
            std::printf("  %-26s %10.3f %s   budget %.0f: %s\n", name, value, unit, max, verdict.c_str());
        } else {
            std::printf("  %-26s %10.3f %s\n", name, value, unit);
        }
        out[name] = {{"value", value}, {"verdict", verdict}};
    };
    std::printf("  the load before it          %10.3f ms   (collision %.3f ms; it cooked what the cache lacked)\n", first.loadMs,
                first.collisionMs);
    out["first_load_ms"] = first.loadMs;
    row("kit2k_load_ms", r.loadMs, "ms");
    row("kit2k_collision_build_ms", r.collisionMs, "ms");
    row("kit2k_raycast_us", r.rayUs, "us");
    row("idle_sync_us", r.idleUs, "us");
    out["passed"] = failures.empty();
    out["failures"] = failures;
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';
    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    std::string line = "{\"suite\":\"level_bench.kit2k\",\"passed\":" + std::to_string(failures.empty() ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.size()) + ",\"failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    std::printf("%s]}\n", line.c_str());
    return failures.empty() ? 0 : 1;
}

// ---- tidewall_nav: the navigation module on Tidewall's yard (docs/plans/2026-11-18-tidewall.md, part N1) ----------

struct NavWorld {
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    ecs::World world;
    NavWorld() {
        engine::install(world, {files, engineTypes()}, {"level", "physics", "navigation"});
        const entt::entity lv = world.createWithId(0x1e7e1, "Yard");
        world.registry().emplace<ecs::Level>(lv, ecs::Level{ecs::AssetRef{"assets/tidewall/levels/yard.glb", {}}});
        world.sync();
    }
    entt::entity named(std::string_view name) {
        for (auto [e, n] : world.registry().view<const ecs::Name>().each())
            if (n.value == name) return e;
        return entt::null;
    }
};

// Removes every navmesh tile from the cooked cache: the next build cooks them all.
std::size_t forgetNavTiles() {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(OHAO_SOURCE_DIR) / ".ohao/cooked", ec))
        if (f.path().extension() == ".navtile" && std::filesystem::remove(f.path(), ec)) ++n;
    return n;
}

int runNav(const nlohmann::json& budgets, int currentStep, const std::string& outPath, int rounds) {
    log::setLevel(log::Level::Warn);
    std::vector<std::string> failures;
    nlohmann::json out;
    const auto row = [&](const std::string& name, const Stats& st, const char* unit) {
        std::string verdict = "reported";
        if (budgets.contains(name)) {
            const double max = budgets[name].value("max", 0.0);
            verdict = currentStep < budgets[name].value("gatedFrom", 0) ? "pending" : st.p95 <= max ? "pass" : "fail";
            if (verdict == "fail") {
                char text[160];
                std::snprintf(text, sizeof text, "%s: p95 %.3f %s over its budget of %.3f", name.c_str(), st.p95, unit, max);
                failures.push_back(text);
            }
            std::printf("  %-30s median %9.3f  p95 %9.3f %s   budget %.3f: %s\n", name.c_str(), st.median, st.p95, unit, max,
                        verdict.c_str());
        } else {
            std::printf("  %-30s median %9.3f  p95 %9.3f %s\n", name.c_str(), st.median, st.p95, unit);
        }
        out[name] = {{"median", st.median}, {"p95", st.p95}, {"min", st.min}, {"verdict", verdict}};
    };

    // Cold: no tile in the cooked cache.
    std::vector<double> cold, coldCook;
    ecs::NavInfo info;
    for (int i = 0; i < 3; ++i) {
        forgetNavTiles();
        NavWorld w;
        info = *ecs::navInfo(w.world);
        cold.push_back(info.buildMs);
        coldCook.push_back(info.cookMs);
    }
    std::printf("level_bench tidewall_nav (yard: %u tiles, %u layers, %u polygons from %u triangles, %u links; %d threads)\n",
                info.tiles, info.layers, info.polys, info.triangles, info.links, jobs::threadCount());
    if (info.cooked != info.tiles || info.tiles == 0)
        failures.push_back("the cold build cooked " + std::to_string(info.cooked) + " of " + std::to_string(info.tiles) + " tiles");
    row("tidewall_nav_cook_ms", stats(cold), "ms");
    row("tidewall_nav_cook_tiles_ms", stats(coldCook), "ms");

    // Warm: every tile from the cache (the load a session pays).
    std::vector<double> warm;
    for (int i = 0; i < rounds; ++i) {
        NavWorld w;
        const ecs::NavInfo n = *ecs::navInfo(w.world);
        if (n.cached != n.tiles) failures.push_back("a warm build cooked " + std::to_string(n.cooked) + " tiles");
        warm.push_back(n.buildMs);
    }
    row("tidewall_nav_load_ms", stats(warm), "ms");

    // One edited piece: a barrier moved 1 m and back, its tiles cooked again (then read), the rest read.
    {
        NavWorld w;
        (void)ecs::navInfo(w.world);
        std::vector<double> edit;
        uint32_t cooked = 0;
        const entt::entity barrier = w.named("BarrierA");
        for (int i = 0; i < 2 && barrier != entt::null; ++i) {
            ecs::Transform t = w.world.registry().get<ecs::Transform>(barrier);
            t.position.x += 1.0f;
            w.world.setTransform(barrier, t);
            w.world.sync();
            const ecs::NavInfo n = *ecs::navInfo(w.world);
            edit.push_back(n.buildMs);
            if (i == 0) cooked = n.cooked;
        }
        if (barrier == entt::null) failures.push_back("the yard has no BarrierA");
        std::printf("  a barrier moved 1 m cooks %u of %u tiles\n", cooked, info.tiles);
        out["edit_cooked_tiles"] = cooked;
        row("tidewall_nav_edit_ms", stats(edit), "ms");
    }

    // Paths between fixed pseudo-random points of the yard's ground.
    {
        NavWorld w;
        (void)ecs::navInfo(w.world);
        uint32_t seed = 0x9e3779b9u;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        std::vector<glm::vec3> points;
        for (int tries = 0; points.size() < 200 && tries < 10000; ++tries) {
            const glm::vec3 p{(next() - 0.5f) * 58.0f, 0.0f, (next() - 0.5f) * 38.0f};
            if (const auto at = ecs::navNearest(w.world, p, {0.5f, 0.5f, 0.5f})) points.push_back(*at);
        }
        std::vector<double> us;
        uint32_t found = 0;
        for (std::size_t i = 0; i < 2000 && !points.empty(); ++i) {
            const glm::vec3 a = points[i % points.size()];
            const glm::vec3 b = points[(i * 7 + 3) % points.size()];
            const auto t0 = std::chrono::steady_clock::now();
            const auto r = ecs::navPath(w.world, a, b);
            us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            found += r && r->status == ecs::NavRoute::Status::Found ? 1u : 0u;
        }
        std::printf("  %u of 2000 paths found\n", found);
        out["paths_found"] = found;
        row("tidewall_nav_path_us", stats(us), "us");
    }

    // An obstacle change: the pump house door opened and closed, each applied to completion.
    {
        NavWorld w;
        (void)ecs::navInfo(w.world);
        const entt::entity door = w.named("PumpHouseDoor");
        std::vector<double> ms;
        for (int i = 0; i < 40 && door != entt::null; ++i) {
            w.world.registry().patch<ecs::NavObstacle>(door, [&](ecs::NavObstacle& o) { o.active = i % 2 == 1; });
            ms.push_back(ecs::navInfo(w.world)->obstaclesMs);
        }
        if (door == entt::null) failures.push_back("the yard has no PumpHouseDoor");
        row("tidewall_nav_obstacle_ms", stats(ms), "ms");
    }

    // Walkers: N agents on a grid of the yard's ground walking to the south entry; the navigation system's step and
    // the whole step, over 300 steps after 60.
    for (const int n : {16, 64}) {
        NavWorld w;
        (void)ecs::navInfo(w.world);
        auto& reg = w.world.registry();
        int made = 0;
        for (int z = -6; z <= 6 && made < n; ++z)
            for (int x = -10; x <= 10 && made < n; ++x) {
                const glm::vec3 p{static_cast<float>(x) * 2.5f, 0.0f, static_cast<float>(z) * 2.5f};
                if (!ecs::navNearest(w.world, p, {0.05f, 0.3f, 0.05f})) continue;
                const entt::entity e = w.world.createWithId(0x7a1c000000000100ull + static_cast<uint64_t>(made), "Walker");
                w.world.setTransform(e, ecs::Transform{p});
                ecs::PlayerController pc;
                pc.input = false;
                pc.spawn.clear();
                pc.walkSpeed = 3.0f;
                reg.emplace<ecs::PlayerController>(e, pc);
                ecs::NavAgent a;
                a.goal = {-18.0f, 0.0f, 23.0f};
                a.active = true;
                reg.emplace<ecs::NavAgent>(e, a);
                ++made;
            }
        w.world.sync();
        std::vector<double> navMs, stepMs;
        for (int i = 0; i < 360; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            w.world.simulate(1.0f / 60.0f);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (i < 60) continue;
            stepMs.push_back(ms);
            navMs.push_back(ecs::navStepTiming(w.world).totalMs);
        }
        if (made != n) failures.push_back("only " + std::to_string(made) + " of " + std::to_string(n) + " walkers placed");
        const std::string tag = "tidewall_walkers" + std::to_string(n);
        row(tag + "_nav_ms", stats(navMs), "ms");
        row(tag + "_step_ms", stats(stepMs), "ms");
    }
    log::setLevel(log::Level::Info);
    out["threads"] = jobs::threadCount();
    out["passed"] = failures.empty();
    out["failures"] = failures;
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';
    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    std::string line = "{\"suite\":\"level_bench.tidewall_nav\",\"passed\":" + std::to_string(failures.empty() ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.size()) + ",\"failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    std::printf("%s]}\n", line.c_str());
    return failures.empty() ? 0 : 1;
}

// level.info's result on the blockout, in bytes (the token budget agents pay), and the result.
std::size_t levelInfoBytes(std::string& result) {
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    ecs::World world;
    engine::install(world, {files, engineTypes()}, {"level", "physics"});
    api::Session session(world, engineTypes());
    const entt::entity lv = world.createWithId(0x1e7e1, "Level");
    world.registry().emplace<ecs::Level>(lv, ecs::Level{ecs::AssetRef{"assets/levels/blockout_room.glb", {}}});
    const std::string reply = session.call("level.info");
    constexpr std::string_view kPrefix = "{\"result\":";
    if (reply.rfind(kPrefix, 0) != 0) {
        result = reply;
        return 0;
    }
    result = reply.substr(kPrefix.size(), reply.size() - kPrefix.size() - 1);
    return result.size();
}

} // namespace

int main(int argc, char** argv) {
    std::string checkPath, outPath = "build/level_bench.json", refPath = "build/serve_bench_reference.json",
                only = "blockout";
    int rounds = 30, threads = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--rounds" && i + 1 < argc) rounds = std::max(1, std::atoi(argv[++i]));
        else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "usage: level_bench [--check budgets.json] [--out file] [--only blockout|kit2k|tidewall_nav] "
                                 "[--rounds N] [--threads N] "
                                 "[--reference file]\n");
            return 2;
        }
    }
    if (only != "blockout" && only != "kit2k" && only != "tidewall_nav") {
        std::fprintf(stderr, "level_bench: --only is blockout, kit2k or tidewall_nav\n");
        return 2;
    }
    jobs::configure(jobs::configFromEnvironment(threads));

    nlohmann::json budgets;
    int currentStep = 0;
    double maxSlowdown = 1.25, maxExternal = 50.0;
    if (!checkPath.empty()) {
        std::ifstream f(checkPath);
        if (!f) {
            std::fprintf(stderr, "level_bench: cannot read %s\n", checkPath.c_str());
            return 2;
        }
        const nlohmann::json doc = nlohmann::json::parse(f, nullptr, false);
        if (doc.is_discarded()) {
            std::fprintf(stderr, "level_bench: %s is not JSON\n", checkPath.c_str());
            return 2;
        }
        budgets = doc.value("budgets", nlohmann::json::object());
        currentStep = doc.value("currentStep", 0);
        if (doc.contains("noise")) {
            maxSlowdown = doc["noise"].value("maxCalibrationSlowdown", maxSlowdown);
            maxExternal = doc["noise"].value("maxExternalCorePct", maxExternal);
        }
    }

    if (only == "tidewall_nav")
        return runNav(budgets, currentStep, outPath == "build/level_bench.json" ? "build/level_bench_tidewall_nav.json" : outPath,
                      std::min(rounds, 10));
    if (only == "kit2k") return runKit(budgets, currentStep, outPath == "build/level_bench.json" ? "build/level_bench_kit2k.json" : outPath);

    const double cal0 = bench::calibrationMs();
    bench::LoadMonitor monitor;
    monitor.start();
    log::setLevel(log::Level::Warn);
    for (int i = 0; i < 5; ++i) (void)loadBlockout();
    std::vector<double> samples;
    LoadResult last;
    for (int i = 0; i < rounds; ++i) {
        last = loadBlockout();
        samples.push_back(last.ms);
    }
    log::setLevel(log::Level::Info);
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);
    const double cal1 = bench::calibrationMs();

    // A benchmark of broken code fails: the blockout's 18 nodes, 12 static bodies (the outdoor
    // Ground's among them), 2 crates.
    const bool wrong = last.nodes != 18 || last.info.bodies.statics != 12 || last.info.bodies.dynamics != 2;
    std::vector<std::string> failures;
    if (wrong)
        failures.push_back("the blockout did not load as expected: " + std::to_string(last.nodes) + " nodes, " +
                           std::to_string(last.info.bodies.statics) + " static and " +
                           std::to_string(last.info.bodies.dynamics) + " dynamic bodies (want 18, 12, 2)");

    const Stats s = stats(samples);
    std::printf("level_bench (%d rounds after 5 warm-ups)\n", rounds);
    std::printf("  blockout_load_ms   median %8.3f   p95 %8.3f   min %8.3f\n", s.median, s.p95, s.min);
    std::string verdict = "reported";
    if (budgets.contains("blockout_load_ms")) {
        const double max = budgets["blockout_load_ms"].value("max", 0.0);
        const int from = budgets["blockout_load_ms"].value("gatedFrom", 0);
        if (currentStep >= from) {
            verdict = s.median <= max ? "pass" : "fail";
            if (s.median > max) {
                char text[160];
                std::snprintf(text, sizeof text, "blockout_load_ms: median %.3f ms over its budget of %.3f ms", s.median, max);
                failures.push_back(text);
            }
        } else {
            verdict = "pending";
        }
        std::printf("    budget %.3f ms, gated from step %d: %s\n", max, from, verdict.c_str());
    }
    // level_info_bytes: a size, not a time, so the machine's load cannot move it.
    std::string info;
    const std::size_t infoBytes = levelInfoBytes(info);
    if (infoBytes == 0) failures.push_back("level.info failed on the blockout: " + info);
    std::printf("  level_info_bytes   %zu\n", infoBytes);
    std::string infoVerdict = "reported";
    if (budgets.contains("level_info_bytes")) {
        const double max = budgets["level_info_bytes"].value("max", 0.0);
        const int from = budgets["level_info_bytes"].value("gatedFrom", 0);
        infoVerdict = currentStep < from ? "pending" : static_cast<double>(infoBytes) <= max ? "pass" : "fail";
        if (infoVerdict == "fail")
            failures.push_back("level_info_bytes: " + std::to_string(infoBytes) + " over its budget of " +
                               std::to_string(static_cast<int>(max)));
        std::printf("    budget %.0f bytes, gated from step %d: %s\n", max, from, infoVerdict.c_str());
    }
    std::printf("  kit2k's four: level_bench --only kit2k\n");

    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    const bool noisyCal = ref > 0 && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisy = noisyCal || noisyLoad;
    std::printf("  machine: calibration before %.3f ms, after %.3f ms, reference %.3f ms (x%.2f); other processes %.1f%% of a core\n",
                cal0, cal1, ref, slow, external);
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);

    nlohmann::json out;
    out["blockout_load_ms"] = {{"median", s.median}, {"p95", s.p95}, {"min", s.min}, {"samples", samples}, {"verdict", verdict}};
    out["level_info_bytes"] = {{"bytes", infoBytes}, {"verdict", infoVerdict}, {"result", info}};
    out["blockout"] = {{"nodes", last.nodes},
                       {"static", last.info.bodies.statics},
                       {"dynamic", last.info.bodies.dynamics},
                       {"buildMs", last.info.buildMs}};
    nlohmann::json busyJson = nlohmann::json::array();
    for (const auto& p : busy) busyJson.push_back({{"name", p.name}, {"corePct", p.corePct}});
    out["machine"] = {{"calibrationBeforeMs", cal0}, {"calibrationAfterMs", cal1}, {"referenceMs", ref},
                      {"externalCorePct", external}, {"busy", busyJson}};
    out["noisy"] = noisy;
    out["checked"] = !checkPath.empty();
    out["passed"] = failures.empty();
    out["failures"] = failures;
    if (const auto dir = std::filesystem::path(outPath).parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';

    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    if (noisy)
        std::printf("  NOISY: %s%s%s; the timings above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%" : "",
                    noisyCal && noisyLoad ? ", and " : "", noisyLoad ? "other processes were busy" : "");
    std::printf("%s (results in %s)\n", noisy ? "NOISY" : failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    std::string line = "{\"suite\":\"level_bench\",\"passed\":" + std::to_string(failures.empty() && !noisy ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.empty() && !noisy ? 0 : 1) + ",\"failures\":[";
    if (noisy) failures.push_back("noisy run (see the machine line above)");
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    line += "]";
    if (noisy) line += ",\"noisy\":true";
    std::printf("%s}\n", line.c_str());
    if (wrong || infoBytes == 0 || infoVerdict == "fail") return 1;   // a size is no machine's noise
    if (noisy) return 2;
    return failures.empty() ? 0 : 1;
}
