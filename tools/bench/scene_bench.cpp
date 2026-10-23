// scene_bench: the scene codec, snapshot and API budgets (spec "Performance").
//
//   scene_bench [--check budgets.json] [--out build/scene_bench.json] [--no-scale] [--iters N]
//               [--reference build/serve_bench_reference.json]
//   scene_bench --breakdown [N]        the codec's phases timed separately, and restore into
//                                      an empty world with its page faults (not gated)
//   scene_bench --machine-test [S]     the CPU probe alone for S seconds: whether this machine
//                                      slows a busy core by itself, and when
//
// The machine is recorded beside the gated rounds (bench_env.hpp): a calibration loop before
// and after, every other process's CPU time, and per round a CPU and a memory probe that
// share nothing with the benchmark. A run that is >25% slower than the reference
// calibration, beside other processes using more than half a core, or with over a quarter
// of its rounds contended (a probe over 1.5x its fastest round) is `noisy`: exit 2, neither
// a pass nor a regression (tools/check.py reruns it). Each op's median in clean and in
// contended rounds is printed, so a slowdown that follows the probes is the machine's and
// one that does not is the code's.
//
// Generates bench10k (and bench100k for the scaling check) with a fixed seed and times
// every gated operation: median of N (50) after 5 warm-ups, p95 reported, single thread.
// Operations run interleaved, one of each per round, so a slow phase of the machine
// (thermal throttling, background load) lands on all of them instead of skewing one; the
// bench100k scaling ratios are taken against bench10k samples from the same rounds. After
// timing, every operation is run once more and its result verified: a benchmark of broken
// code fails. Results go to build/scene_bench.json; with --check the exit code is 1 when a
// median is over its budget or a bench100k median is more than maxRatio times bench10k's.
//
// The scene is the spec's: Name, Transform, Parent, Light, MeshSource, MaterialOverride,
// and (since step 8) one Camera and one Environment.
#include "api/session.hpp"
#include "bench_env.hpp"
#include "bench_world.hpp"
#include "common/harness.hpp"
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
#include <psapi.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

using bench::buildBenchWorld;

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
    std::vector<double> faults;       // page faults during each timed run
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

// The process's page faults so far (soft faults included: first touches of fresh pages).
uint64_t pageFaults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS c{};
    c.cb = sizeof c;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c)) return c.PageFaultCount;
#endif
    return 0;
}

// Evicts the CPU caches: writes one byte per cache line of a buffer larger than any L3 in
// use. The buffer is allocated and touched once, so flushing takes no page faults.
void flushCaches() {
    static std::vector<uint8_t> buffer(std::size_t{128} << 20, 1);
    for (std::size_t i = 0; i < buffer.size(); i += 64) buffer[i] = static_cast<uint8_t>(buffer[i] + 1);
}

// A copy of `ops` whose untimed setup ends by evicting the caches (scaling samples).
std::vector<Op> coldCopy(const std::vector<Op>& ops) {
    std::vector<Op> out = ops;
    for (Op& op : out) {
        op.samples.clear();
        op.faults.clear();
        op.setup = [setup = op.setup] {
            if (setup) setup();
            flushCaches();
        };
    }
    return out;
}

// The machine beside each recorded round (bench_env.hpp's probes).
struct RoundProbe {
    double cpuMs{0};
    double memoryMs{0};
};

// One run of every op per round; the first `warmups` rounds are not recorded. With
// `probes`, each recorded round starts by timing the CPU and memory probes.
void runRounds(const std::vector<Op*>& ops, int warmups, int rounds, std::vector<RoundProbe>* probes = nullptr) {
    for (int r = 0; r < warmups + rounds; ++r) {
        if (probes && r >= warmups) probes->push_back({bench::cpuProbeMs(), bench::memoryProbeMs()});
        for (Op* op : ops) {
            if (op->setup) op->setup();
            const uint64_t f0 = pageFaults();
            const auto t0 = std::chrono::steady_clock::now();
            op->run();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t f1 = pageFaults();
            if (op->teardown) op->teardown();
            if (r >= warmups) {
                op->samples.push_back(ms);
                op->faults.push_back(static_cast<double>(f1 - f0));
            }
        }
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
    char fbuf[32];
    std::size_t sink = 0;
    // Each fast path next to the reference it replaced (the tree decoder, the generic
    // writer, std::to_chars), so a regression in either shows.
    std::vector<Op> ops = {
        {"parse pretty (tree, warm)", {}, [&] { (void)parser.parse(pretty); }, {}},
        {"validate pretty (streaming)", {}, [&] { (void)validateScene(pretty, engineTypes()); }, {}},
        {"validate pretty (tree)", {}, [&] { (void)validateSceneReference(pretty, engineTypes()); }, {}},
        {"validate compact (streaming)", {}, [&] { (void)validateScene(compact, engineTypes()); }, {}},
        {"load pretty (streaming)", [&] { fresh = std::make_unique<World>(); },
         [&] { (void)loadScene(*fresh, pretty, engineTypes()); }, [&] { fresh.reset(); }},
        {"load pretty (tree)", [&] { fresh = std::make_unique<World>(); },
         [&] { (void)loadSceneReference(*fresh, pretty, engineTypes()); }, [&] { fresh.reset(); }},
        {"save pretty (per-type writers)", {}, [&] { (void)save(world); }, {}},
        {"save pretty (generic writer)", {}, [&] { (void)saveSceneGeneric(world, engineTypes()); }, {}},
        {"save compact (per-type writers)", {}, [&] { (void)save(world, JsonLayout::Compact); }, {}},
        {"Transform floats: formatFloat32", {}, [&] {
             for (float f : fs) sink += static_cast<std::size_t>(reflect::formatFloat32(f, fbuf) - fbuf);
         }, {}},
        {"Transform floats: std::to_chars", {}, [&] {
             for (float f : fs) sink += static_cast<std::size_t>(std::to_chars(fbuf, fbuf + sizeof(fbuf), f).ptr - fbuf);
         }, {}},
    };
    std::vector<Op*> list;
    for (Op& op : ops) list.push_back(&op);
    runRounds(list, 3, 20);
    std::printf("  breakdown, %d entities (%.2f MB pretty, %zu floats in Transforms)\n", n, pretty.size() / 1e6, fs.size());
    for (const Op& op : ops) {
        const Timing t = stats(op.samples);
        std::printf("    %-32s median %8.3f ms   p95 %8.3f ms\n", op.name.c_str(), t.median, t.p95);
    }
    (void)sink;

    // Restore into an empty world, and the teardown of that world, with the page faults
    // each takes (a first touch of a fresh page is a fault).
    const Snapshot snap = takeSnapshot(world, engineTypes());
    std::vector<double> restoreMs, restoreFaults, dropMs, dropFaults;
    for (int r = 0; r < 23; ++r) {
        auto w = std::make_unique<World>();
        const uint64_t f0 = pageFaults();
        const auto t0 = std::chrono::steady_clock::now();
        (void)restoreSnapshot(*w, snap);
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t f1 = pageFaults();
        w.reset();
        const auto t2 = std::chrono::steady_clock::now();
        const uint64_t f2 = pageFaults();
        if (r < 3) continue;
        restoreMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        dropMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        restoreFaults.push_back(static_cast<double>(f1 - f0));
        dropFaults.push_back(static_cast<double>(f2 - f1));
    }
    const auto line = [](const char* what, const std::vector<double>& ms, const std::vector<double>& faults) {
        const Timing t = stats(ms), f = stats(faults);
        std::printf("    %-32s median %8.3f ms   p95 %8.3f ms   page faults %8.0f\n", what, t.median, t.p95, f.median);
    };
    line("restore into an empty world", restoreMs, restoreFaults);
    line("  destroying that world", dropMs, dropFaults);
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
    // Results go under build/ (git-ignored) unless --out says otherwise.
    std::string checkPath, outPath = "build/scene_bench.json", refPath = "build/serve_bench_reference.json";
    bool scale = true;
    int iters = 50;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];        else if (a == "--no-scale") scale = false;
        else if (a == "--iters" && i + 1 < argc) iters = std::max(1, std::atoi(argv[++i]));
        else if (a == "--breakdown") {
            breakdown(i + 1 < argc ? std::max(1000, std::atoi(argv[i + 1])) : 10'000);
            return 0;
        } else if (a == "--machine-test") {
            // The CPU probe alone, back to back for N seconds, no benchmark work: whether this
            // machine slows a busy core by itself after a while (power limits), and when.
            const int seconds = i + 1 < argc ? std::max(1, std::atoi(argv[i + 1])) : 30;
            const auto start = std::chrono::steady_clock::now();
            int second = 0;
            std::vector<double> in;
            while (second < seconds) {
                in.push_back(bench::cpuProbeMs());
                const int now = static_cast<int>(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                if (now > second) {
                    std::sort(in.begin(), in.end());
                    std::printf("  second %3d: cpu probe min %.3f ms, median %.3f ms (%zu probes)\n", second, in.front(),
                                in[in.size() / 2], in.size());
                    in.clear();
                    second = now;
                }
            }
            return 0;
        } else if (a == "--write-scene" && i + 1 < argc) {
            // The bench10k scene as a file, for timing the ohao CLI on it (spec, "Recorded").
            World w;
            buildBenchWorld(w, 10'000, 2026);
            std::ofstream f(argv[i + 1], std::ios::binary | std::ios::trunc);
            f << save(w);
            return f ? 0 : 2;
        } else {
            std::fprintf(stderr, "usage: scene_bench [--check budgets.json] [--out file] [--no-scale] [--iters N]\n"
                                 "       scene_bench --breakdown [entities]\n"
                                 "       scene_bench --write-scene out.scene.json\n");
            return 2;
        }
    }

    // Budgets: {"budgetsMs": {"op": ms, ...}, "scaling": {"maxRatio": r}}.
    std::map<std::string, double> budgets;
    double maxRatio = 0.0;
    double maxSlowdown = 1.25, maxExternal = 50.0;   // "noise": as serve_bench's
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
        if (const auto* n = d.member(d.root(), "noise")) {
            if (const auto* v = d.member(*n, "maxCalibrationSlowdown")) maxSlowdown = std::stod(std::string(v->text));
            if (const auto* v = d.member(*n, "maxExternalCorePct")) maxExternal = std::stod(std::string(v->text));
        }
    }

    std::printf("scene_bench (Release, single thread, no GPU; %d rounds after 5 warm-ups)\n", iters);
    Scene s10("bench10k", 10'000);

    // Scaling (spec, "Budgets"): bench100k against bench10k, both measured the same way.
    // - Caches cold for both: every scaling sample's untimed setup ends by evicting them.
    //   bench100k cannot fit in the caches; warm bench10k samples would compare cache hits
    //   with memory traffic.
    // - The same memory history for both: the bench10k block runs first, in a process that
    //   has only built the scene, and the bench100k block last. How many page faults a 10k
    //   operation takes depends on what the heap kept from earlier frees: after a 100k
    //   world has been freed, a whole 10k world fits in the kept memory (1 fault instead of
    //   ~650); after the 50 gated rounds, anywhere from ~200 to ~560 from run to run.
    //   bench100k always takes fresh pages (~6,500 faults for restore_empty).
    // These blocks replaced the step-6 interleaved pairs, whose bench10k samples landed in
    // one memory state or the other and put restore_empty anywhere from x10.7 to x16.8.
    // The page faults of both sides are reported next to each ratio.
    std::unique_ptr<Scene> s100;
    std::vector<Op> cold10, cold100;
    const int scaleRounds = std::max(20, iters / 2);   // 25 by default; 10 was too few
    const auto runBlock = [&](std::vector<Op>& ops) {
        std::vector<Op*> list;
        for (Op& op : ops) list.push_back(&op);
        runRounds(list, 2, scaleRounds);
    };
    if (scale) {
        cold10 = coldCopy(s10.ops);
        runBlock(cold10);
    }

    // The gated bench10k rounds, with the machine recorded beside them (bench_env.hpp): a
    // calibration loop before and after, every other process's CPU time, and per round a
    // CPU and a memory probe.
    std::vector<RoundProbe> probes;
    bench::LoadMonitor monitor;
    monitor.start();
    const double cal0 = bench::calibrationMs();
    {
        std::vector<Op*> list;
        for (Op& op : s10.ops) list.push_back(&op);
        runRounds(list, 5, iters, &probes);
    }
    const double cal1 = bench::calibrationMs();
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);
    std::vector<std::string> failures = s10.verify();

    if (scale) {
        s100 = std::make_unique<Scene>("bench100k", 100'000);
        cold100 = coldCopy(s100->ops);
        runBlock(cold100);
        for (auto& p : s100->verify()) failures.push_back(std::move(p));
    }

    const std::size_t wrong = failures.size();   // verify() problems, before any budget verdict
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
            const Op& op = gated ? s.ops[i] : cold100[i];   // bench100k: its cold scaling samples
            const Timing t = stats(op.samples);
            char verdict[160] = "";
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
                const Timing base = stats(cold10[i].samples);
                const double ratio = base.median > 0 ? t.median / base.median : 0.0;
                const bool pass = maxRatio <= 0 || ratio <= maxRatio;
                std::snprintf(verdict, sizeof verdict,
                              "%s x%.1f of bench10k cold (%.3f ms; %.3f warm) page faults %.0f / %.0f",
                              pass ? "ok  " : "OVER", ratio, base.median, stats(s10.ops[i].samples).median,
                              stats(op.faults).median, stats(cold10[i].faults).median);
                if (!pass) failures.push_back(s.name() + " " + op.name + ": " + std::to_string(ratio) + "x bench10k");
                w.key("ratioTo10k"); w.float32(static_cast<float>(ratio));
                w.key("bench10kColdMs"); w.float32(static_cast<float>(base.median));
                w.key("pageFaults"); w.float32(static_cast<float>(stats(op.faults).median));
                w.key("bench10kPageFaults"); w.float32(static_cast<float>(stats(cold10[i].faults).median));
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

    // ---- the machine during the gated rounds ----------------------------------------------
    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    std::vector<double> cpu, mem;
    for (const RoundProbe& p : probes) {
        cpu.push_back(p.cpuMs);
        mem.push_back(p.memoryMs);
    }
    const double memMin = mem.empty() ? 0.0 : *std::min_element(mem.begin(), mem.end());
    const double cpuMin = cpu.empty() ? 0.0 : *std::min_element(cpu.begin(), cpu.end());
    // A round is contended when either probe ran more than 1.5x its fastest round: the probes
    // touch nothing of the benchmark's, so only the machine can slow them.
    std::vector<uint8_t> contended(probes.size(), 0);
    std::size_t nContended = 0;
    for (std::size_t r = 0; r < probes.size(); ++r) {
        contended[r] = probes[r].memoryMs > 1.5 * memMin || probes[r].cpuMs > 1.5 * cpuMin;
        nContended += contended[r];
    }
    const bool noisyCal = ref > 0 && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisyRounds = !probes.empty() && nContended * 4 > probes.size();   // over a quarter
    const bool noisy = noisyCal || noisyLoad || noisyRounds;
    char refText[96] = "none (run serve_bench --calibrate on a quiet machine)";
    if (ref > 0) std::snprintf(refText, sizeof refText, "%.3f ms (x%.2f)", ref, slow);
    std::printf("  machine during the gated rounds:\n");
    std::printf("    calibration  before %.3f ms  after %.3f ms  reference %s\n", cal0, cal1, refText);
    std::printf("    other processes  %.1f%% of a core in total%s\n", external, busy.empty() ? "" : ":");
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);
    std::printf("    probes per round: cpu median %.3f ms (min %.3f, p95 %.3f), memory median %.3f ms (min %.3f, p95 %.3f)\n",
                stats(cpu).median, cpuMin, stats(cpu).p95, stats(mem).median, memMin, stats(mem).p95);
    std::printf("    contended rounds (a probe over 1.5x its fastest): %zu of %zu\n", nContended, probes.size());
    // Each gated op's median in clean and in contended rounds: if the slowdown follows the
    // probes it is the machine; if clean rounds are slow too, it is the code.
    for (const Op& op : s10.ops) {
        std::vector<double> clean, hit;
        for (std::size_t r = 0; r < op.samples.size() && r < contended.size(); ++r)
            (contended[r] ? hit : clean).push_back(op.samples[r]);
        std::printf("    %-22s clean rounds median %9.3f ms (%zu)   contended %9.3f ms (%zu)\n", op.name.c_str(),
                    stats(clean).median, clean.size(), stats(hit).median, hit.size());
    }
    w.key("machine");
    w.beginObject();
    w.key("calibrationBeforeMs"); w.float32(static_cast<float>(cal0));
    w.key("calibrationAfterMs"); w.float32(static_cast<float>(cal1));
    w.key("referenceMs"); w.float32(static_cast<float>(ref));
    w.key("externalCorePct"); w.float32(static_cast<float>(external));
    w.key("busy");
    w.beginArray();
    for (const auto& p : busy) {
        w.beginObject();
        w.key("name"); w.string(p.name);
        w.key("corePct"); w.float32(static_cast<float>(p.corePct));
        w.endObject();
    }
    w.endArray();
    w.key("contendedRounds"); w.uint64(nContended);
    w.key("rounds");
    w.beginArray();   // per gated round: the probes and every op's time
    for (std::size_t r = 0; r < probes.size(); ++r) {
        w.beginObject();
        w.key("cpuProbeMs"); w.float32(static_cast<float>(probes[r].cpuMs));
        w.key("memoryProbeMs"); w.float32(static_cast<float>(probes[r].memoryMs));
        for (const Op& op : s10.ops)
            if (r < op.samples.size()) { w.key(op.name); w.float32(static_cast<float>(op.samples[r])); }
        w.endObject();
    }
    w.endArray();
    w.endObject();
    w.key("noisy"); w.boolean(noisy);
    w.key("checked"); w.boolean(!checkPath.empty());
    w.key("passed"); w.boolean(failures.empty());
    w.key("failures");
    w.beginArray();
    for (const auto& f : failures) w.string(f);
    w.endArray();
    w.endObject();
    if (const auto dir = std::filesystem::path(outPath).parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    std::ofstream(outPath, std::ios::binary) << w.str() << '\n';

    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    if (noisy)
        std::printf("  NOISY: %s%s%s; the timings above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%; " : "",
                    noisyLoad ? "other processes were busy; " : "",
                    noisyRounds ? "over a quarter of the rounds were contended" : "");
    std::printf("%s (results in %s)\n", noisy ? "NOISY" : failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    // The shared summary line; "noisy" lets tools/check.py rerun a noisy run.
    std::string line = "{\"suite\":\"scene_bench\",\"passed\":" + std::to_string(failures.empty() && !noisy ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.empty() && !noisy ? 0 : 1) + ",\"failures\":[";
    if (noisy) failures.push_back("noisy run (see the machine section above)");
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    line += "]";
    if (noisy) line += ",\"noisy\":true";
    std::printf("%s}\n", line.c_str());
    if (wrong) return 1;   // a wrong result fails whatever the machine did
    if (noisy) return 2;
    return failures.empty() ? 0 : 1;
}
