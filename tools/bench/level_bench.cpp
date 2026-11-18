// level_bench: the level pipeline's budgets (docs/specs/2026-10-26-level-pipeline-design.md,
// "Performance budgets").
//
//   level_bench [--check budgets.json] [--out build/level_bench.json] [--only blockout|kit2k]
//               [--rounds N] [--reference build/serve_bench_reference.json]
//
// Step 4 measures blockout_load_ms: a cold load of assets/levels/blockout_room.glb into a
// fresh World with the level and physics systems (structure, 13 meshes through a fresh asset
// resolver, expansion, colliders; no GPU upload), median and p95 of N rounds (30) after 5
// warm-ups. The OS file cache is warm after the warm-ups; the engine's caches are not.
// level_info_bytes (step 5): the size of level.info's result on the blockout, through a Session
// as an agent calls it; a size, so a noisy machine cannot excuse a miss. `--only kit2k` measures
// the generated kit's four numbers instead (measureKit), one load after a warming one.
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
    int rounds = 30;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--rounds" && i + 1 < argc) rounds = std::max(1, std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "usage: level_bench [--check budgets.json] [--out file] [--only blockout] [--rounds N] "
                                 "[--reference file]\n");
            return 2;
        }
    }
    if (only != "blockout" && only != "kit2k") {
        std::fprintf(stderr, "level_bench: --only is blockout or kit2k\n");
        return 2;
    }

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
