// physics_bench: the physics step's budgets (docs/specs/2026-10-26-player-physics-design.md,
// "Performance budgets").
//
//   physics_bench [--check budgets.json] [--out build/physics_bench.json] [--rounds N]
//                 [--reference build/serve_bench_reference.json]
//
// Step 1 measures, CPU, headless, through the World as every host steps it (World::simulate):
//   blockout_step_ms       one step of the blockout while its two crates fall and land (CrateA
//                          lifted 1 m, CrateB 2 m): the first 90 steps after the lift, N rounds
//                          (10) in fresh worlds; median and p95 of every step.
//   blockout_rest_step_ms  one step of the blockout settled (everything asleep): 200 steps after
//                          240 of settling; and it must write no Transform (a count, not a time).
//   events_1k_ms           the events part of one step in which 1,000 spheres touch a floor at
//                          once: draining, collapsing, sorting and logging 1,000 begins (the
//                          scene's own timing of that part); median of N rounds.
// Step 2 adds player_step_ms (the player part of a step walking the blockout), step 5
// boxes1k_step_ms (one step while 1,000 dynamic boxes fall and pile up in a walled pit, awake;
// none may leave it).
//
// The machine is recorded as level_bench records it (bench_env.hpp): a calibration loop before and
// after against the reference, and every other process's CPU. A run >25% slower than the
// reference or beside other processes using more than half a core is `noisy`: exit 2, neither a
// pass nor a regression. A wrong result (crates not landing, writes at rest, the wrong event
// count) is a failure whatever the machine.
#include "common/harness.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"

#include <nlohmann/json.hpp>

#include "bench_env.hpp"   // after the engine headers: it includes windows.h

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace ohao;

namespace {

constexpr float kDt = 1.0f / 60.0f;

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
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

// The blockout in a fresh World with the level and physics systems, as the hosts install them.
struct Blockout {
    ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
    ecs::World world;
    ecs::PhysicsScene* physics{nullptr};
    Blockout() {
        world.seedIds(7);
        level::installLevels(world, files, engineTypes());
        physics = &ecs::installPhysics(world, files);
        const entt::entity lv = world.createWithId(0x1e7e1, "Level");
        world.registry().emplace<ecs::Level>(lv, ecs::Level{ecs::AssetRef{"assets/levels/blockout_room.glb", {}}});
        world.sync();
    }
    entt::entity named(std::string_view name) {
        for (auto [e, n, ln] : world.registry().view<const ecs::Name, const ecs::LevelNode>().each())
            if (n.value == name) return e;
        return entt::null;
    }
    void lift() {
        for (const auto& [name, up] : {std::pair{"CrateA", 1.0f}, std::pair{"CrateB", 2.0f}}) {
            const entt::entity e = named(name);
            ecs::Transform t = world.registry().get<ecs::Transform>(e);
            t.position.y += up;
            world.setTransform(e, t);
        }
    }
};

struct Verdict {
    std::string name, verdict = "reported";
    double max{0.0};
    int from{0};
};

Verdict judge(const nlohmann::json& budgets, int currentStep, const std::string& name, double value,
              std::vector<std::string>& failures) {
    Verdict v;
    v.name = name;
    if (!budgets.contains(name)) return v;
    v.max = budgets[name].value("max", 0.0);
    v.from = budgets[name].value("gatedFrom", 0);
    if (currentStep < v.from) {
        v.verdict = "pending";
    } else {
        v.verdict = value <= v.max ? "pass" : "fail";
        if (v.verdict == "fail") {
            char text[160];
            std::snprintf(text, sizeof text, "%s: median %.4f ms over its budget of %.4f ms", name.c_str(), value, v.max);
            failures.push_back(text);
        }
    }
    return v;
}

} // namespace

int main(int argc, char** argv) {
    std::string checkPath, outPath = "build/physics_bench.json", refPath = "build/serve_bench_reference.json";
    int rounds = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--rounds" && i + 1 < argc) rounds = std::max(1, std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "usage: physics_bench [--check budgets.json] [--out file] [--rounds N] [--reference file]\n");
            return 2;
        }
    }

    nlohmann::json budgets;
    int currentStep = 0;
    double maxSlowdown = 1.25, maxExternal = 50.0;
    if (!checkPath.empty()) {
        std::ifstream f(checkPath);
        if (!f) {
            std::fprintf(stderr, "physics_bench: cannot read %s\n", checkPath.c_str());
            return 2;
        }
        const nlohmann::json doc = nlohmann::json::parse(f, nullptr, false);
        if (doc.is_discarded()) {
            std::fprintf(stderr, "physics_bench: %s is not JSON\n", checkPath.c_str());
            return 2;
        }
        budgets = doc.value("budgets", nlohmann::json::object());
        currentStep = doc.value("currentStep", 0);
        if (doc.contains("noise")) {
            maxSlowdown = doc["noise"].value("maxCalibrationSlowdown", maxSlowdown);
            maxExternal = doc["noise"].value("maxExternalCorePct", maxExternal);
        }
    }

    const double cal0 = bench::calibrationMs();
    bench::LoadMonitor monitor;
    monitor.start();
    log::setLevel(log::Level::Warn);
    std::vector<std::string> failures;

    // 1. The drop.
    std::vector<double> fall;
    bool landed = true;
    for (int r = 0; r < rounds + 1; ++r) {   // round 0 warms the caches and is not kept
        Blockout b;
        b.lift();
        for (int i = 0; i < 90; ++i) {
            b.world.simulate(kDt);
            b.world.sync();
            if (r > 0) fall.push_back(b.physics->lastStepTiming().totalMs);
        }
        const glm::vec3 pa(b.world.registry().get<ecs::WorldTransform>(b.named("CrateA")).world[3]);
        landed &= pa.y < 0.35f;
    }
    if (!landed) failures.push_back("the lifted crates did not land within 90 steps");

    // 2. At rest.
    std::vector<double> rest;
    uint64_t restWrites = 0;
    {
        Blockout b;
        b.lift();
        for (int i = 0; i < 240; ++i) {
            b.world.simulate(kDt);
            b.world.sync();
        }
        for (int i = 0; i < 200; ++i) {
            b.world.simulate(kDt);
            b.world.sync();
            rest.push_back(b.physics->lastStepTiming().totalMs);
            restWrites += b.physics->lastStepTiming().writes;
        }
    }
    if (restWrites) failures.push_back("the settled blockout wrote " + std::to_string(restWrites) + " Transforms in 200 steps (want 0)");

    // 3. 1,000 begins in one step.
    std::vector<double> events;
    std::size_t begins = 0;
    for (int r = 0; r < rounds + 1; ++r) {
        ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
        ecs::World w;
        w.seedIds(9);
        ecs::PhysicsScene& physics = ecs::installPhysics(w, files);
        auto& reg = w.registry();
        const entt::entity floor = w.create("Floor", ecs::Transform{{0.0f, -0.5f, 0.0f}});
        reg.emplace<ecs::Collider>(floor, ecs::Collider{ecs::ColliderShape::Box, {40.0f, 0.5f, 40.0f}});
        for (int i = 0; i < 1000; ++i) {
            const entt::entity s = w.create("Ball", ecs::Transform{{-15.5f + float(i % 32), 0.1f, -15.5f + float(i / 32)}});
            reg.emplace<ecs::Collider>(s, ecs::Collider{ecs::ColliderShape::Sphere, {0.1f, 0.0f, 0.0f}});
            reg.emplace<ecs::RigidBody>(s, ecs::RigidBody{ecs::Motion::Dynamic, 1.0f});
        }
        w.simulate(kDt);
        begins = physics.stepEvents().size();
        if (r > 0) events.push_back(physics.lastStepTiming().eventsMs);
    }
    if (begins != 1000) failures.push_back("1,000 spheres on a floor began " + std::to_string(begins) + " contacts (want 1,000)");

    // 4. The player walking the blockout: from the spawn, forward and turning (so it meets walls,
    // the crates and the ramp), the player part of each step, 200 steps after 20, N rounds.
    std::vector<double> player;
    bool walked = true;
    for (int r = 0; r < rounds + 1; ++r) {
        Blockout b;
        const entt::entity pl = b.world.createWithId(0xb1a7e2, "Player");
        b.world.registry().emplace<ecs::PlayerController>(pl);
        std::vector<InputEvent> go(2);
        go[0].type = InputEvent::Type::AxisSet;
        go[0].name = "move_forward";
        go[0].value = 1.0f;
        go[1].type = InputEvent::Type::AxisSet;
        go[1].name = "look_x";
        go[1].value = 6.0f;   // 0.6 degrees a step
        const glm::vec3 start(b.world.registry().get<ecs::Transform>(pl).position);
        for (int i = 0; i < 220; ++i) {
            b.world.stepInput(i == 0 ? std::span<const InputEvent>(go) : std::span<const InputEvent>());
            b.world.simulate(kDt);
            b.world.sync();
            if (r > 0 && i >= 20) player.push_back(b.physics->lastStepTiming().playerMs);
        }
        walked &= glm::length(b.world.registry().get<ecs::Transform>(pl).position - start) > 1.0f;
    }
    if (!walked) failures.push_back("the player did not walk");

    // 5. 1,000 boxes falling into a pit (step 5): a 6 x 6 m floor walled 3 m high, the boxes (0.4 m
    // cubes, 10 x 10 x 10, 0.5 m apart) dropped from 0.5-5 m above it; every step after 60 of
    // warm-up, 200, while they fall and pile up awake. None may leave the pit.
    std::vector<double> boxes, snapshotMs;
    uint32_t boxesAwake = 0;
    bool contained = true;
    for (int r = 0; r < std::max(1, rounds / 5) + 1; ++r) {
        ecs::FileAssetResolver files{OHAO_SOURCE_DIR};
        ecs::World w;
        w.seedIds(11);
        ecs::PhysicsScene& physics = ecs::installPhysics(w, files);
        auto& reg = w.registry();
        const auto wall = [&](const char* name, glm::vec3 at, glm::vec3 half) {
            const entt::entity e = w.create(name, ecs::Transform{at});
            reg.emplace<ecs::Collider>(e, ecs::Collider{ecs::ColliderShape::Box, half});
        };
        wall("Floor", {0.0f, -0.5f, 0.0f}, {3.5f, 0.5f, 3.5f});
        wall("WallN", {0.0f, 1.5f, -3.25f}, {3.5f, 1.5f, 0.25f});
        wall("WallS", {0.0f, 1.5f, 3.25f}, {3.5f, 1.5f, 0.25f});
        wall("WallW", {-3.25f, 1.5f, 0.0f}, {0.25f, 1.5f, 3.0f});
        wall("WallE", {3.25f, 1.5f, 0.0f}, {0.25f, 1.5f, 3.0f});
        std::vector<entt::entity> crates;
        for (int i = 0; i < 1000; ++i) {
            const glm::vec3 at(-2.25f + 0.5f * float(i % 10), 0.5f + 0.5f * float(i / 100), -2.25f + 0.5f * float((i / 10) % 10));
            const entt::entity e = w.create("Box", ecs::Transform{at});
            reg.emplace<ecs::Collider>(e, ecs::Collider{ecs::ColliderShape::Box, {0.2f, 0.2f, 0.2f}});
            reg.emplace<ecs::RigidBody>(e, ecs::RigidBody{ecs::Motion::Dynamic, 5.0f});
            crates.push_back(e);
        }
        for (int i = 0; i < 260; ++i) {
            w.simulate(kDt);
            w.sync();
            if (r > 0 && i >= 60) boxes.push_back(physics.lastStepTiming().totalMs);
        }
        boxesAwake = physics.info().awake;
        // What a snapshot of this world adds (the simulation it carries), reported, not budgeted.
        for (int i = 0; i < 5 && r > 0; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto saved = physics.saveSimulation();
            snapshotMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            (void)saved;
        }
        for (const entt::entity e : crates) {
            const glm::vec3 p(reg.get<ecs::WorldTransform>(e).world[3]);
            contained &= std::fabs(p.x) < 3.0f && std::fabs(p.z) < 3.0f && p.y > 0.0f;
        }
    }
    if (!contained) failures.push_back("a box of the 1,000 left the pit");

    log::setLevel(log::Level::Info);
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);
    const double cal1 = bench::calibrationMs();

    const Stats sf = stats(fall), sr = stats(rest), se = stats(events), sp = stats(player), sb = stats(boxes);
    std::printf("physics_bench (%d rounds after a warm-up)\n", rounds);
    std::printf("  blockout_step_ms        median %8.4f   p95 %8.4f   (%zu steps)\n", sf.median, sf.p95, fall.size());
    const Verdict vf = judge(budgets, currentStep, "blockout_step_ms", sf.median, failures);
    std::printf("  blockout_rest_step_ms   median %8.4f   p95 %8.4f   writes %llu\n", sr.median, sr.p95,
                static_cast<unsigned long long>(restWrites));
    const Verdict vr = judge(budgets, currentStep, "blockout_rest_step_ms", sr.median, failures);
    std::printf("  events_1k_ms            median %8.4f   p95 %8.4f   (%zu begins)\n", se.median, se.p95, begins);
    const Verdict ve = judge(budgets, currentStep, "events_1k_ms", se.median, failures);
    std::printf("  player_step_ms          median %8.4f   p95 %8.4f   (%zu steps)\n", sp.median, sp.p95, player.size());
    const Verdict vp = judge(budgets, currentStep, "player_step_ms", sp.median, failures);
    std::printf("  boxes1k_step_ms         median %8.4f   p95 %8.4f   (%zu steps; %u awake at the end)\n", sb.median, sb.p95,
                boxes.size(), boxesAwake);
    const Verdict vb = judge(budgets, currentStep, "boxes1k_step_ms", sb.median, failures);
    const Stats ss = stats(snapshotMs);
    std::printf("  boxes1k_snapshot_ms     median %8.4f   (reported: the simulation a snapshot of that world carries)\n",
                ss.median);
    for (const Verdict* v : {&vf, &vr, &ve, &vp, &vb})
        if (v->verdict != "reported")
            std::printf("    %-22s budget %.4f ms, gated from step %d: %s\n", v->name.c_str(), v->max, v->from, v->verdict.c_str());
    for (const auto& [name, b] : budgets.items())
        if (name != "blockout_step_ms" && name != "blockout_rest_step_ms" && name != "events_1k_ms" && name != "player_step_ms" &&
            name != "boxes1k_step_ms")
            std::printf("  %-22s not measured yet (gated from step %d)\n", name.c_str(), b.value("gatedFrom", 0));

    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    const bool noisyCal = ref > 0 && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisy = noisyCal || noisyLoad;
    std::printf("  machine: calibration before %.3f ms, after %.3f ms, reference %.3f ms (x%.2f); other processes %.1f%% of a core\n",
                cal0, cal1, ref, slow, external);
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);

    nlohmann::json out;
    out["blockout_step_ms"] = {{"median", sf.median}, {"p95", sf.p95}, {"min", sf.min}, {"steps", fall.size()}, {"verdict", vf.verdict}};
    out["blockout_rest_step_ms"] = {{"median", sr.median}, {"p95", sr.p95}, {"min", sr.min}, {"writes", restWrites}, {"verdict", vr.verdict}};
    out["events_1k_ms"] = {{"median", se.median}, {"p95", se.p95}, {"min", se.min}, {"begins", begins}, {"verdict", ve.verdict}};
    out["player_step_ms"] = {{"median", sp.median}, {"p95", sp.p95}, {"min", sp.min}, {"steps", player.size()}, {"verdict", vp.verdict}};
    out["boxes1k_step_ms"] = {{"median", sb.median}, {"p95", sb.p95}, {"min", sb.min}, {"steps", boxes.size()},
                              {"awakeAtEnd", boxesAwake}, {"verdict", vb.verdict}};
    out["boxes1k_snapshot_ms"] = {{"median", ss.median}, {"min", ss.min}, {"verdict", "reported"}};
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

    const bool wrong = !landed || restWrites != 0 || begins != 1000 || !walked || !contained;   // no machine excuses these
    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    if (noisy)
        std::printf("  NOISY: %s%s%s; the timings above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%" : "",
                    noisyCal && noisyLoad ? ", and " : "", noisyLoad ? "other processes were busy" : "");
    std::printf("%s (results in %s)\n", noisy && !wrong ? "NOISY" : failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    std::string line = "{\"suite\":\"physics_bench\",\"passed\":" + std::to_string(failures.empty() && !noisy ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.empty() && !noisy ? 0 : 1) + ",\"failures\":[";
    if (noisy) failures.push_back("noisy run (see the machine line above)");
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    line += "]";
    if (noisy) line += ",\"noisy\":true";
    std::printf("%s}\n", line.c_str());
    if (wrong) return 1;
    if (noisy) return 2;
    return failures.empty() ? 0 : 1;
}
