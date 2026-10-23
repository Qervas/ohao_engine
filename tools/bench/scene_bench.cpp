// scene_bench: the scene codec, snapshot and API budgets (spec "Performance").
//
//   scene_bench [--check budgets.json] [--out scene_bench.json] [--no-scale] [--iters N]
//   scene_bench --breakdown [N]        the codec's phases timed separately (not gated)
//
// Generates bench10k (and bench100k for the scaling check) with a fixed seed and times
// every gated operation: median of N (50) after 5 warm-ups, p95 reported, single thread.
// Operations run interleaved, one of each per round, so a slow phase of the machine
// (thermal throttling, background load) lands on all of them instead of skewing one; the
// bench100k scaling ratios are taken against bench10k samples from the same rounds. After
// timing, every operation is run once more and its result verified: a benchmark of broken
// code fails. Results go to scene_bench.json; with --check the exit code is 1 when a
// median is over its budget or a bench100k median is more than maxRatio times bench10k's.
//
// The scene is the spec's: Name, Transform, Parent, Light, MeshSource, MaterialOverride,
// and (since step 8) one Camera and one Environment.
#include "api/session.hpp"
#include "ecs/components.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "reflect/json_writer.hpp"

#include <glm/gtc/quaternion.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace ohao;
using namespace ohao::ecs;
using reflect::JsonLayout;
using reflect::JsonWriter;

namespace {

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

// bench10k/bench100k: n entities with Name and Transform (random position, rotation and
// scale) and a hierarchy up to 8 deep (about 60% of the entities have a parent among the
// 64 created just before them). 99% are renderables (a MeshSource primitive and a
// MaterialOverride, from palettes), 1% lights of every type. Then a camera and an
// environment entity (n + 2 in all).
void buildBenchWorld(World& w, int n, uint64_t seed) {
    w.seedIds(seed);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> pos(-50.0f, 50.0f), unit(-1.0f, 1.0f), ang(0.0f, 6.2831853f),
        scale(0.25f, 4.0f), pos01(0.0f, 1.0f);
    std::vector<entt::entity> es;
    std::vector<uint8_t> depth;
    es.reserve(static_cast<std::size_t>(n));
    const int lights = n / 100;
    for (int i = 0; i < n; ++i) {
        const bool isLight = i >= n - lights;
        Transform t;
        t.position = {pos(rng), pos(rng), pos(rng)};
        t.rotation = glm::angleAxis(ang(rng), glm::normalize(glm::vec3(unit(rng), unit(rng), unit(rng)) + 1e-3f));
        t.scale = glm::vec3(scale(rng));
        const auto e = w.create((isLight ? "Light_" : "Mesh_") + std::to_string(i), t);
        uint8_t d = 0;
        if (i > 0 && rng() % 5 < 3) {
            const std::size_t p = es.size() - 1 - rng() % std::min<std::size_t>(64, es.size());
            if (depth[p] < 8 && w.setParent(e, es[p])) d = static_cast<uint8_t>(depth[p] + 1);
        }
        if (isLight) {
            Light l;
            l.type = static_cast<LightType>(rng() % 4);
            l.color = {pos01(rng), pos01(rng), pos01(rng)};
            l.intensity = pos01(rng) * 20.0f;
            l.range = 5.0f + pos01(rng) * 40.0f;
            w.registry().emplace<Light>(e, l);
        } else {
            // A renderable: a primitive and a material. Authored scenes reuse sizes and
            // materials, so both come from small palettes (the transforms stay random).
            static constexpr float kSizes[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
            const float s = kSizes[rng() % std::size(kSizes)];
            MeshSource ms;
            switch (rng() % 6) {
            case 0: ms.source = SpherePrim{s, 32, 16}; break;
            case 1: ms.source = CubePrim{s}; break;
            case 2: ms.source = PlatformPrim{s * 2.0f, 0.4f, s * 2.0f}; break;
            case 3: ms.source = CylinderPrim{s * 0.5f, s, 24}; break;
            case 4: ms.source = ConePrim{s * 0.5f, s, 24}; break;
            default: ms.source = BoxPrim{glm::vec3(-s), glm::vec3(s), glm::vec3(0.73f)}; break;
            }
            w.registry().emplace<MeshSource>(e, ms);
            static constexpr float kChannel[] = {0.05f, 0.12f, 0.25f, 0.45f, 0.65f, 0.73f, 0.85f, 0.95f};
            const auto c = [&] { return kChannel[rng() % std::size(kChannel)]; };
            static constexpr float kRoughness[] = {0.05f, 0.2f, 0.35f, 0.5f, 0.75f, 0.95f};
            w.registry().emplace<MaterialOverride>(
                e, MaterialOverride{{c(), c(), c()}, kRoughness[rng() % std::size(kRoughness)],
                                    rng() % 5 == 0 ? 1.0f : 0.0f, 0.0f});
        }
        es.push_back(e);
        depth.push_back(d);
    }
    // The spec's one camera and one environment, after the n entities (so they leave the
    // random stream above untouched).
    Transform cam;
    cam.position = {0.0f, 20.0f, 120.0f};
    cam.rotation = glm::angleAxis(glm::radians(-9.5f), glm::vec3(1.0f, 0.0f, 0.0f));
    const auto c = w.create("MainCamera", cam);
    w.registry().emplace<ecs::Camera>(c, ecs::Camera{50.0f, 0.1f, 500.0f, true});
    const auto env = w.create("Environment");
    w.registry().emplace<Environment>(env, Environment{AssetRef{"assets/env/bench_sky.hdr", {}}, 1.25f});
}

std::string save(const World& w, JsonLayout layout = JsonLayout::Pretty) {
    auto s = saveScene(w, engineTypes(), SaveOptions{layout});
    return s ? s.value() : std::string();
}

// ---- timing --------------------------------------------------------------------------------

struct Op {
    std::string name;
    std::function<void()> setup;      // untimed, before each run
    std::function<void()> run;        // timed
    std::function<void()> teardown;   // untimed, after each run
    std::vector<double> samples;      // ms
};

struct Timing {
    double median{0};
    double p95{0};
};

Timing stats(std::vector<double> ms) {
    if (ms.empty()) return {};
    std::sort(ms.begin(), ms.end());
    const auto at = [&](double q) { return ms[std::min(ms.size() - 1, static_cast<std::size_t>(q * (ms.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95)};
}

// One run of every op per round; the first `warmups` rounds are not recorded.
void runRounds(const std::vector<Op*>& ops, int warmups, int rounds) {
    for (int r = 0; r < warmups + rounds; ++r)
        for (Op* op : ops) {
            if (op->setup) op->setup();
            const auto t0 = std::chrono::steady_clock::now();
            op->run();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (op->teardown) op->teardown();
            if (r >= warmups) op->samples.push_back(ms);
        }
}

// ---- the benchmark scene and its gated operations -------------------------------------------

class Scene {
public:
    Scene(std::string name, int n) : m_name(std::move(name)), m_n(n) {
        buildBenchWorld(m_world, n, 2026);
        m_pretty = save(m_world);
        m_compact = save(m_world, JsonLayout::Compact);
        m_snap = takeSnapshot(m_world, engineTypes());
        for (std::size_t i = 0; i < m_snap.rows(); i += 100) m_movers.push_back(m_world.findById(m_snap.ids[i]));
        m_session = std::make_unique<api::Session>(m_world, engineTypes());

        JsonWriter c;   // 100 field.set on entities spread over the world
        c.beginObject();
        c.key("commands");
        c.beginArray();
        for (int i = 0; i < 100; ++i) {
            c.beginObject();
            c.key("op"); c.string("field.set");
            c.key("id"); c.hex64(m_snap.ids[static_cast<std::size_t>(i) * static_cast<std::size_t>(n / 100)]);
            c.key("path"); c.string("/Transform/position");
            c.key("value"); c.beginArray(); c.float32(i * 0.5f); c.float32(1.0f); c.float32(-2.25f); c.endArray();
            c.endObject();
        }
        c.endArray();
        c.endObject();
        m_batch = c.take();

        JsonWriter q;   // 1,000 ids, full components
        q.beginObject();
        q.key("ids");
        q.beginArray();
        for (int i = 0; i < 1000; ++i) q.hex64(m_snap.ids[static_cast<std::size_t>(i) * static_cast<std::size_t>(n / 1000)]);
        q.endArray();
        q.key("limit"); q.int64(1000);
        q.endObject();
        m_query = q.take();

        const auto freshWorld = [this] { m_fresh = std::make_unique<World>(); };
        const auto dropWorld = [this] { m_fresh.reset(); };
        const auto restoreBase = [this] { (void)restoreSnapshot(m_world, m_snap); };
        ops = {
            {"save_pretty", {}, [this] { m_text = save(m_world); }, {}},
            {"load", freshWorld, [this] { (void)loadScene(*m_fresh, m_pretty, engineTypes()); }, dropWorld},
            {"validate", {}, [this] { (void)validateScene(m_pretty, engineTypes()); }, {}},
            // Recaptures over a snapshot taken before (the agent loop: re-snapshot "base").
            {"snapshot", {}, [this] { takeSnapshot(m_world, engineTypes(), m_tmp); }, {}},
            {"restore_unchanged", {}, [this] { m_stats = restoreSnapshot(m_world, m_snap); }, {}},
            {"restore_1pct", [this] { moveOnePercent(); }, [this] { m_stats = restoreSnapshot(m_world, m_snap); }, {}},
            {"restore_empty", freshWorld, [this] { m_stats = restoreSnapshot(*m_fresh, m_snap); }, dropWorld},
            {"apply_100_field_set", {}, [this] { m_reply = m_session->call("world.apply", m_batch); }, restoreBase},
            {"query_1000_ids", {}, [this] { m_reply = m_session->call("world.query", m_query); }, {}},
        };
    }

    // Runs every operation once more and checks that it did its job.
    std::vector<std::string> verify() {
        std::vector<std::string> problems;
        const auto check = [&](bool ok, const char* what) {
            if (!ok) problems.push_back(m_name + ": " + what);
        };
        check(save(m_world) == m_pretty, "save is not deterministic (or a timed op left the world changed)");
        check(validateScene(m_pretty, engineTypes()).ok(), "the bench scene does not validate");
        {
            World w;
            check(loadScene(w, m_pretty, engineTypes()) && save(w) == m_pretty, "load does not reproduce the scene");
        }
        const std::size_t entities = m_world.idCount();   // m_n plus the camera and environment
        check(takeSnapshot(m_world, engineTypes()).rows() == entities, "the snapshot misses entities");
        RestoreStats st = restoreSnapshot(m_world, m_snap);
        check(st.created == 0 && st.destroyed == 0 && st.changed == 0, "an unchanged restore wrote something");
        moveOnePercent();
        st = restoreSnapshot(m_world, m_snap);
        check(st.changed == m_movers.size() && save(m_world) == m_pretty, "restoring 1% did not restore exactly those rows");
        {
            World w;
            st = restoreSnapshot(w, m_snap);
            check(st.created == entities && save(w) == m_pretty, "restore into an empty world differs");
        }
        check(m_session->call("world.apply", m_batch).rfind("{\"result\":{\"applied\":100,", 0) == 0, "world.apply failed");
        (void)restoreSnapshot(m_world, m_snap);
        check(m_session->call("world.query", m_query).find("\"count\":1000") != std::string::npos,
              "world.query did not return 1000 entities");
        return problems;
    }

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] int entities() const { return static_cast<int>(m_world.idCount()); }
    [[nodiscard]] std::size_t prettyBytes() const { return m_pretty.size(); }
    [[nodiscard]] std::size_t compactBytes() const { return m_compact.size(); }

    std::vector<Op> ops;

private:
    void moveOnePercent() {
        m_bump += 1.0f;
        for (const auto e : m_movers) m_world.registry().patch<Transform>(e, [&](Transform& t) { t.position.x += m_bump; });
    }

    std::string m_name;
    int m_n;
    World m_world;
    std::string m_pretty, m_compact, m_text, m_batch, m_query, m_reply;
    Snapshot m_snap, m_tmp;
    RestoreStats m_stats;
    std::vector<entt::entity> m_movers;
    float m_bump{0.0f};
    std::unique_ptr<World> m_fresh;
    std::unique_ptr<api::Session> m_session;
};

// ---- --breakdown ----------------------------------------------------------------------------

void breakdown(int n) {
    World world;
    buildBenchWorld(world, n, 2026);
    const std::string pretty = save(world), compact = save(world, JsonLayout::Compact);
    JsonParser parser;
    std::unique_ptr<World> fresh;
    std::vector<float> fs;
    for (const auto [e, t] : world.registry().view<const Transform>().each())
        fs.insert(fs.end(), {t.position.x, t.position.y, t.position.z, t.rotation.x, t.rotation.y, t.rotation.z,
                             t.rotation.w, t.scale.x});
    std::vector<Op> ops = {
        {"parse pretty (warm parser)", {}, [&] { (void)parser.parse(pretty); }, {}},
        {"parse compact (warm parser)", {}, [&] { (void)parser.parse(compact); }, {}},
        {"validate pretty", {}, [&] { (void)validateScene(pretty, engineTypes()); }, {}},
        {"validate compact", {}, [&] { (void)validateScene(compact, engineTypes()); }, {}},
        {"load pretty", [&] { fresh = std::make_unique<World>(); }, [&] { (void)loadScene(*fresh, pretty, engineTypes()); },
         [&] { fresh.reset(); }},
        {"save compact", {}, [&] { (void)save(world, JsonLayout::Compact); }, {}},
        {"save pretty", {}, [&] { (void)save(world); }, {}},
        {"prettyPrint(compact)", {}, [&] { (void)reflect::prettyPrint(compact); }, {}},
        {"format the Transform floats", {}, [&] {
             JsonWriter w(fs.size() * 12);
             w.beginArray();
             for (float f : fs) w.float32(f);
             w.endArray();
         }, {}},
    };
    std::vector<Op*> list;
    for (Op& op : ops) list.push_back(&op);
    runRounds(list, 3, 20);
    std::printf("  breakdown, %d entities (%.2f MB pretty, %zu floats in Transforms)\n", n, pretty.size() / 1e6, fs.size());
    for (const Op& op : ops) {
        const Timing t = stats(op.samples);
        std::printf("    %-30s median %8.3f ms   p95 %8.3f ms\n", op.name.c_str(), t.median, t.p95);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);   // less scheduling noise (spec "Risks")
#endif
    std::string checkPath, outPath = "scene_bench.json";
    bool scale = true;
    int iters = 50;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--no-scale") scale = false;
        else if (a == "--iters" && i + 1 < argc) iters = std::max(1, std::atoi(argv[++i]));
        else if (a == "--breakdown") {
            breakdown(i + 1 < argc ? std::max(1000, std::atoi(argv[i + 1])) : 10'000);
            return 0;
        } else {
            std::fprintf(stderr, "usage: scene_bench [--check budgets.json] [--out file] [--no-scale] [--iters N]\n"
                                 "       scene_bench --breakdown [entities]\n");
            return 2;
        }
    }

    // Budgets: {"budgetsMs": {"op": ms, ...}, "scaling": {"maxRatio": r}}.
    std::map<std::string, double> budgets;
    double maxRatio = 0.0;
    if (!checkPath.empty()) {
        auto doc = parseJson(readFile(checkPath));
        if (!doc) {
            std::fprintf(stderr, "scene_bench: cannot read budgets %s: %s\n", checkPath.c_str(), doc.error().message.c_str());
            return 2;
        }
        const auto& d = doc.value();
        if (const auto* b = d.member(d.root(), "budgetsMs"))
            for (uint32_t i = 0; i < b->count; ++i)
                budgets[std::string(d.child(*b, i).key)] = std::stod(std::string(d.child(*b, i).text));
        if (const auto* s = d.member(d.root(), "scaling"))
            if (const auto* r = d.member(*s, "maxRatio")) maxRatio = std::stod(std::string(r->text));
    }

    std::printf("scene_bench (Release, single thread, no GPU; %d rounds after 5 warm-ups)\n", iters);
    Scene s10("bench10k", 10'000);
    {
        std::vector<Op*> list;
        for (Op& op : s10.ops) list.push_back(&op);
        runRounds(list, 5, iters);
    }
    std::vector<std::string> failures = s10.verify();

    // bench100k, each op paired with a bench10k run in the same round.
    std::unique_ptr<Scene> s100;
    std::vector<Op> paired10;
    if (scale) {
        s100 = std::make_unique<Scene>("bench100k", 100'000);
        paired10 = s10.ops;
        std::vector<Op*> list;
        for (std::size_t i = 0; i < s100->ops.size(); ++i) {
            paired10[i].samples.clear();
            list.push_back(&s100->ops[i]);
            list.push_back(&paired10[i]);
        }
        runRounds(list, 2, std::max(10, iters / 5));
        for (auto& p : s100->verify()) failures.push_back(std::move(p));
    }

    JsonWriter w(4096, JsonLayout::Pretty);
    w.beginObject();
    w.key("suite"); w.string("scene_bench");
    w.key("rounds"); w.int64(iters);
    w.key("scenes");
    w.beginArray();
    const auto report = [&](const Scene& s, bool gated) {
        std::printf("  %s: %d entities, %.2f MB pretty, %.2f MB compact\n", s.name().c_str(), s.entities(),
                    s.prettyBytes() / 1e6, s.compactBytes() / 1e6);
        w.beginObject();
        w.key("name"); w.string(s.name());
        w.key("entities"); w.int64(s.entities());
        w.key("prettyBytes"); w.uint64(s.prettyBytes());
        w.key("compactBytes"); w.uint64(s.compactBytes());
        w.key("ops");
        w.beginObject();
        for (std::size_t i = 0; i < s.ops.size(); ++i) {
            const Op& op = s.ops[i];
            const Timing t = stats(op.samples);
            char verdict[96] = "";
            w.key(op.name);
            w.beginObject();
            w.key("medianMs"); w.float32(static_cast<float>(t.median));
            w.key("p95Ms"); w.float32(static_cast<float>(t.p95));
            if (gated) {
                if (const auto b = budgets.find(op.name); b != budgets.end()) {
                    const bool pass = t.median <= b->second;
                    std::snprintf(verdict, sizeof verdict, "%s budget %g ms", pass ? "ok  " : "OVER", b->second);
                    if (!pass)
                        failures.push_back(s.name() + " " + op.name + ": median " + std::to_string(t.median) +
                                           " ms > budget " + std::to_string(b->second) + " ms");
                    w.key("budgetMs"); w.float32(static_cast<float>(b->second));
                }
            } else {
                const Timing base = stats(paired10[i].samples);
                const double ratio = base.median > 0 ? t.median / base.median : 0.0;
                const bool pass = maxRatio <= 0 || ratio <= maxRatio;
                std::snprintf(verdict, sizeof verdict, "%s x%.1f of bench10k (%.3f ms in the same rounds)",
                              pass ? "ok  " : "OVER", ratio, base.median);
                if (!pass) failures.push_back(s.name() + " " + op.name + ": " + std::to_string(ratio) + "x bench10k");
                w.key("ratioTo10k"); w.float32(static_cast<float>(ratio));
            }
            w.endObject();
            std::printf("    %-22s median %9.3f ms   p95 %9.3f ms   %s\n", op.name.c_str(), t.median, t.p95, verdict);
        }
        w.endObject();
        w.endObject();
    };
    report(s10, true);
    if (s100) report(*s100, false);
    w.endArray();
    w.key("checked"); w.boolean(!checkPath.empty());
    w.key("passed"); w.boolean(failures.empty());
    w.key("failures");
    w.beginArray();
    for (const auto& f : failures) w.string(f);
    w.endArray();
    w.endObject();
    std::ofstream(outPath, std::ios::binary) << w.str() << '\n';

    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    std::printf("%s (results in %s)\n", failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    return failures.empty() ? 0 : 1;
}
