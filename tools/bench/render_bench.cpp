// render_bench: the renderer's GPU budgets (sub-project 2b spec, "Profiling first").
//
//   render_bench [--check tests/bench/render_budgets.json] [--out build/render_bench.json]
//                [--reference build/render_bench_reference.json] [--frames N] [--only SCENE]
//   render_bench --calibrate          writes the CPU and GPU reference calibrations (quiet machine)
//   render_bench --evaluate RESULTS --check BUDGETS [--out FILE]
//                                     the verdict on recorded results, without a GPU (test 2)
//
// Each scene is a headless capture, exactly as `ohao run` renders it (host::RenderHost,
// runtime::captureFrame): the scene file, its size and post effects, then 5 warm-up and 100
// timed frames back to back. Every frame carries its per-pass GPU timestamps
// (render/frame/gpu_pass_timer.hpp); the bench reads all of them after the last frame's
// fence. Per scene it reports the median and p95 of
//   frame   GPU time of the frame's rendering: first pass begin to the start of the final
//           copy out (present / readback), the quantity the frame budgets bound
//   span    the whole frame, the copy out included
//   cpu     recording and submission on the CPU
//   each pass, over the frames that ran it.
//
// Budgets (tests/bench/render_budgets.json) are tagged with the 2b build step that delivers
// them; `--check` gates those whose step is done (gatedFrom <= currentStep) and reports the
// rest as pending, with their measurements. A budget names a measure: the frame, the CPU,
// a ratio of CPU times, or a group of passes (the sum of their medians; "x.*" matches every
// pass named x.<something>).
//
// Noise (spec 4's measurement rules, plus the GPU): a CPU calibration loop and a GPU
// calibration kernel (shaders/bench/gpu_calibrate.slang) run before and after the frames;
// either more than 25% slower than its reference, or other processes using more than half a
// core in total, makes the run `noisy`: exit 2, neither a pass nor a failure (tools/check.py
// reruns it). Exit 1 when a gated budget is missed or a scene could not be rendered.
#include "api/session.hpp"
#include "bench_env.hpp"
#include "bench_world.hpp"
#include "common/harness.hpp"
#include "core/log.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "gpu_calibration.hpp"
#include "ohao/render_host.hpp"
#include "reflect/json_writer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/gpu_culling.hpp"
#include "render/frame/pass_registry.hpp"
#include "ecs/combat.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/vfx.hpp"
#include "render/scene/meshlet_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace ohao;
namespace fs = std::filesystem;

namespace {

constexpr int kWarmup = 5;
constexpr int kTimed = 100;
// The GPU calibration's hash rounds per element (2^31 in all): about 1.6 ms on the
// reference GPU, long enough that clock ramp-up and timestamp granularity do not matter.
constexpr uint32_t kGpuCalibrationIterations = 8192;

struct SceneSpec {
    const char* name;
    const char* path;
    uint32_t width, height;
    std::vector<std::string> post;
    std::optional<float> exposure;
    bool hud{false};   // the combat HUD over the frame (sub-project 7: the hud pass)
    // Sub-project 12: the scripted moment of play_vfx_1080p before the frames: a grenade thrown and
    // the rifle held down, stepped until the boom; the frames then step with the trigger held.
    bool vfx{false};
    // Sub-project 10: the world steps before every frame (one fixed step, then one sync and the
    // frame, as a window runs), so the skinned instances' poses change every frame.
    bool animate{false};
};

// anim16_1080p: the blockout room with 16 test rigs (tests/anim/fixtures/rig.glb) on a 4 x 4 grid,
// each running wave from another phase (7 steps apart).
constexpr int kAnimRigs = 16;

// play_vfx_1080p's script: steps from the throw to the boom (combat::kGrenade's 2.5 s fuse).
constexpr int kVfxScriptSteps = 151;

// The spec's table. The bench scenes are built in code (bench_world.hpp) and written to
// build/render_bench/ by writeBenchScenes before the run.
const std::vector<SceneSpec>& sceneSpecs() {
    static const std::vector<SceneSpec> s = {
        {"cornell_720p", "assets/scenes/cornell_box.scene.json", 1280, 720, {}, std::nullopt},
        {"helmet_game_720p", "assets/scenes/helmet_game.scene.json", 1280, 720, {"taa", "ssao", "bloom"}, 1.0f},
        {"helmet_game_1080p", "assets/scenes/helmet_game.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f},
        {"bench10k_1080p", "build/render_bench/bench10k.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f},
        {"bench100k_1080p", "build/render_bench/bench100k.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f},
        // The play scene through the player's eyes with its HUD (health, ammo, crosshair).
        {"play_hud_1080p", "assets/scenes/blockout_play.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f, true},
        // 16 skinned rigs in the blockout, every pose changing every frame: skin, skin.bounds and
        // rt.blas.refit at full load (sub-project 10).
        {"anim16_1080p", "build/render_bench/anim16.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f, false, false, true},
        // The play scene stepped before every frame: its two skinned enemies (sub-project 10 step 2)
        // posed every frame, as in a window: the play scene's skin, skin.bounds and rt.blas.refit.
        {"play_anim_1080p", "assets/scenes/blockout_play.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f, false, false, true},
        // The play scene's effects (sub-project 12): a grenade's boom and smoke, the rifle's muzzles
        // and impacts, stepped before every frame: vfx.expand, vfx.sort and transparent. Last: it
        // installs the combat systems (physics, combat, vfx) on the bench's world.
        {"play_vfx_1080p", "assets/scenes/blockout_play.scene.json", 1920, 1080, {"taa", "ssao", "bloom"}, 1.0f, false, true, true},
    };
    return s;
}

const reflect::TypeRegistry& engineTypes();
std::string readText(const fs::path& p);

// bench10k and bench100k (bench_world.hpp's render scenes) as scene files, for the run's
// scene.load; only those the run renders.
bool writeBenchScenes(const std::string& only) {
    fs::create_directories("build/render_bench");
    for (const auto& [name, n] : {std::pair{"bench10k", 10'000}, std::pair{"bench100k", 100'000}}) {
        if (!only.empty() && only.rfind(name, 0) != 0) continue;
        ecs::World w;
        bench::buildRenderBenchWorld(w, n, 2026);
        auto text = ecs::saveScene(w, engineTypes(), ecs::SaveOptions{reflect::JsonLayout::Compact});
        if (!text) return false;
        std::ofstream(std::string("build/render_bench/") + name + ".scene.json", std::ios::binary | std::ios::trunc)
            << text.value();
    }
    if (only.empty() || only.rfind("anim16", 0) == 0) {
        // The blockout room's scene with the rigs added to its entities (and their types to its header).
        std::string text = readText("assets/scenes/blockout_room.scene.json");
        const auto header = text.find("\"components\": {");
        if (header == std::string::npos) return false;
        text.insert(header + 15, "\n    \"Animator\": 1,\n    \"MeshSource\": 1,\n    \"SkinnedMesh\": 1,");
        const auto end = text.rfind("\n  ]");
        if (end == std::string::npos) return false;
        std::string rigs;
        for (int i = 0; i < kAnimRigs; ++i) {
            char id[17];
            std::snprintf(id, sizeof id, "a16a0000000000%02x", i + 1);
            const float x = -2.0f + 1.2f * static_cast<float>(i % 4), z = -2.0f + 1.2f * static_cast<float>(i / 4);
            rigs += ",\n    { \"id\": \"" + std::string(id) + "\", \"Name\": { \"value\": \"Rig" + std::to_string(i) +
                    "\" }, \"Transform\": { \"position\": [" + std::to_string(x) + ", 0, " + std::to_string(z) +
                    "] }, \"MeshSource\": { \"source\": { \"ModelAsset\": { \"asset\": { \"path\": "
                    "\"tests/anim/fixtures/rig.glb\" }, \"mesh\": \"Column\" } } }, \"SkinnedMesh\": { \"file\": { "
                    "\"path\": \"tests/anim/fixtures/rig.glb\" }, \"skin\": \"RigSkin\" }, \"Animator\": { \"graph\": "
                    "{ \"path\": \"tests/anim/fixtures/rig.animgraph.json\" }, \"layers\": [ { \"state\": \"wave\", "
                    "\"steps\": " + std::to_string(7 * i) + " }, { \"state\": \"still\", \"steps\": 0 } ] } }";
        }
        text.insert(end, rigs);
        std::ofstream("build/render_bench/anim16.scene.json", std::ios::binary | std::ios::trunc) << text;
    }
    return true;
}

// ---- the meshlet build (CPU) ----------------------------------------------------------------

// The meshlet builder on a million triangles: a 708x708 grid, heights from two waves, in row
// order as a vertex-cache-ordered asset would be. Builds back to back, the median of them.
constexpr const char* kMeshletScene = "meshlet_build_1m";
constexpr int kMeshletBuilds = 9;

std::vector<double> timeMeshletBuild() {
    constexpr uint32_t n = 708;   // 2 * 708^2 = 1,002,528 triangles
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    verts.reserve(std::size_t{n + 1} * (n + 1));
    idx.reserve(std::size_t{n} * n * 6);
    for (uint32_t y = 0; y <= n; ++y)
        for (uint32_t x = 0; x <= n; ++x) {
            Vertex v{};
            v.position = glm::vec3(float(x) * 0.1f, std::sin(float(x) * 0.031f) * std::cos(float(y) * 0.047f) * 4.0f, float(y) * 0.1f);
            verts.push_back(v);
        }
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    std::vector<double> ms;
    for (int i = 0; i < kMeshletBuilds; ++i) {
        render::MeshletSet set;
        const auto t0 = std::chrono::steady_clock::now();
        render::buildMeshlets(verts, idx, 0, static_cast<uint32_t>(idx.size()), set);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return ms;
}

// ---- results --------------------------------------------------------------------------------

struct Stats {
    double median{0}, p95{0};
};

Stats stats(std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * (v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95)};
}

struct PassResult {
    std::string name;
    Stats s;
    int frames{0};   // frames that ran it
};

struct SceneResult {
    std::string name;
    uint32_t width{0}, height{0};
    int frames{0};               // frames, or for meshlet_build_1m builds
    Stats frame, span, cpu;
    Stats meshletBuild;          // meshlet_build_1m only
    std::vector<PassResult> passes;   // registry (frame) order
    std::string error;                // the capture failed
};

struct Measurement {
    std::string gpu;
    double cpuCal0{0}, cpuCal1{0}, cpuRef{0};
    double gpuCal0{0}, gpuCal1{0}, gpuRef{0};
    double external{0};
    std::vector<bench::BusyProcess> busy;
    std::vector<SceneResult> scenes;
};

bool isCopyOut(std::string_view pass) {
    return pass == "present" || pass == "readback" || pass == "pt.present" || pass == "pt.readback";
}

SceneResult summarize(const SceneSpec& spec, const std::vector<render::FrameTiming>& frames) {
    SceneResult r;
    r.name = spec.name;
    r.width = spec.width;
    r.height = spec.height;
    r.frames = static_cast<int>(frames.size());
    std::vector<double> frame, span, cpu;
    std::vector<std::vector<double>> pass(static_cast<std::size_t>(render::kPassCount));
    for (const render::FrameTiming& f : frames) {
        double copyOut = 0.0;
        for (uint32_t i = 0; i < f.count && i < render::kMaxTimedPasses; ++i) {
            if (f.pass[i] >= pass.size()) continue;
            pass[f.pass[i]].push_back(f.ms[i]);
            if (isCopyOut(render::kPasses[f.pass[i]].name)) copyOut += f.ms[i];
        }
        span.push_back(f.gpuMs);
        frame.push_back(std::max(0.0, f.gpuMs - copyOut));
        cpu.push_back(f.cpuMs);
    }
    r.frame = stats(frame);
    r.span = stats(span);
    r.cpu = stats(cpu);
    for (std::size_t i = 0; i < pass.size(); ++i)
        if (!pass[i].empty())
            r.passes.push_back({std::string(render::kPasses[i].name), stats(pass[i]), static_cast<int>(pass[i].size())});
    return r;
}

// ---- JSON in and out ------------------------------------------------------------------------

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double jsonNumber(const std::string& text, const std::string& key, double fallback) {
    const auto at = text.find("\"" + key + "\"");
    if (at == std::string::npos) return fallback;
    const auto colon = text.find(':', at);
    return colon == std::string::npos ? fallback : std::atof(text.c_str() + colon + 1);
}

struct Reader {
    const reflect::JsonDocument& doc;
    const reflect::JsonNode* at(const reflect::JsonNode* n, std::string_view key) const {
        return n && n->type == reflect::JsonType::Object ? doc.member(*n, key) : nullptr;
    }
    double num(const reflect::JsonNode* n, std::string_view key, double fallback = 0.0) const {
        const reflect::JsonNode* m = at(n, key);
        return m && m->type == reflect::JsonType::Number ? std::strtod(std::string(m->text).c_str(), nullptr) : fallback;
    }
    std::string str(const reflect::JsonNode* n, std::string_view key) const {
        const reflect::JsonNode* m = at(n, key);
        return m && m->type == reflect::JsonType::String ? std::string(m->text) : std::string();
    }
    Stats stat(const reflect::JsonNode* n, std::string_view key) const {
        const reflect::JsonNode* m = at(n, key);
        return {num(m, "median"), num(m, "p95")};
    }
    template <class Fn>
    void each(const reflect::JsonNode* n, std::string_view key, Fn&& fn) const {
        const reflect::JsonNode* a = at(n, key);
        if (!a || a->type != reflect::JsonType::Array) return;
        for (uint32_t i = 0; i < a->count; ++i) fn(&doc.child(*a, i));
    }
};

// Results as render_bench writes them (its own output, or a fixture).
std::optional<Measurement> readMeasurement(const std::string& text, std::string& why) {
    auto parsed = ecs::parseJson(text);
    if (!parsed) {
        why = "not JSON";
        return std::nullopt;
    }
    const reflect::JsonDocument& doc = parsed.value();
    const Reader r{doc};
    const reflect::JsonNode* root = &doc.root();
    if (root->type != reflect::JsonType::Object) {
        why = "not a JSON object";
        return std::nullopt;
    }
    Measurement m;
    m.gpu = r.str(root, "gpu");
    const reflect::JsonNode* cal = r.at(root, "calibration");
    m.cpuCal0 = r.num(r.at(cal, "cpu"), "beforeMs");
    m.cpuCal1 = r.num(r.at(cal, "cpu"), "afterMs");
    m.gpuCal0 = r.num(r.at(cal, "gpu"), "beforeMs");
    m.gpuCal1 = r.num(r.at(cal, "gpu"), "afterMs");
    m.cpuRef = r.num(r.at(cal, "cpu"), "referenceMs");
    m.gpuRef = r.num(r.at(cal, "gpu"), "referenceMs");
    const reflect::JsonNode* load = r.at(root, "externalLoad");
    m.external = r.num(load, "totalCorePct");
    r.each(load, "busy", [&](const reflect::JsonNode* p) { m.busy.push_back({r.str(p, "name"), r.num(p, "corePct")}); });
    r.each(root, "scenes", [&](const reflect::JsonNode* s) {
        SceneResult sr;
        sr.name = r.str(s, "name");
        sr.width = static_cast<uint32_t>(r.num(s, "width"));
        sr.height = static_cast<uint32_t>(r.num(s, "height"));
        sr.frames = static_cast<int>(r.num(s, "frames"));
        sr.frame = r.stat(s, "frame");
        sr.span = r.stat(s, "span");
        sr.cpu = r.stat(s, "cpu");
        sr.meshletBuild = r.stat(s, "meshletBuild");
        sr.error = r.str(s, "error");
        r.each(s, "passes", [&](const reflect::JsonNode* p) {
            sr.passes.push_back({r.str(p, "name"), {r.num(p, "median"), r.num(p, "p95")}, static_cast<int>(r.num(p, "frames"))});
        });
        m.scenes.push_back(std::move(sr));
    });
    return m;
}

// ---- budgets and the verdict ----------------------------------------------------------------

struct Budget {
    std::string scene, name, measure, of;   // measure: frame | cpu | cpuRatio | passes
    std::vector<std::string> passes;
    double limit{0};
    std::string unit{"ms"};
    int gatedFrom{0};
    // the evaluation
    std::string status;   // pass | over | pending | unmeasured | noisy
    std::optional<double> measured;
    bool ran{false};      // a pass budget: at least one of its passes ran
};

bool matches(std::string_view pattern, std::string_view pass) {
    if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == ".*")
        return pass.size() > pattern.size() - 1 && pass.substr(0, pattern.size() - 1) == pattern.substr(0, pattern.size() - 1);
    return pattern == pass;
}

std::vector<Budget> readBudgets(const reflect::JsonDocument& doc, int& currentStep) {
    const Reader r{doc};
    const reflect::JsonNode* root = &doc.root();
    currentStep = static_cast<int>(r.num(root, "currentStep"));
    std::vector<Budget> out;
    r.each(root, "budgets", [&](const reflect::JsonNode* n) {
        Budget b;
        b.scene = r.str(n, "scene");
        b.name = r.str(n, "name");
        b.measure = r.str(n, "measure");
        b.of = r.str(n, "of");
        r.each(n, "passes", [&](const reflect::JsonNode* p) {
            if (p->type == reflect::JsonType::String) b.passes.emplace_back(p->text);
        });
        if (b.measure.empty()) b.measure = b.passes.empty() ? "frame" : "passes";
        if (r.at(n, "budgetRatio")) {
            b.limit = r.num(n, "budgetRatio");
            b.unit = "x";
        } else {
            b.limit = r.num(n, "budgetMs");
        }
        b.gatedFrom = static_cast<int>(r.num(n, "gatedFrom"));
        out.push_back(std::move(b));
    });
    return out;
}

const SceneResult* findScene(const Measurement& m, const std::string& name) {
    for (const SceneResult& s : m.scenes)
        if (s.name == name && s.error.empty() && s.frames > 0) return &s;
    return nullptr;
}

void measure(const Measurement& m, Budget& b) {
    const SceneResult* s = findScene(m, b.scene);
    if (!s) return;
    if (b.measure == "frame") {
        b.measured = s->frame.median;
    } else if (b.measure == "cpu") {
        b.measured = s->cpu.median;
    } else if (b.measure == "meshletBuild") {
        b.measured = s->meshletBuild.median;
    } else if (b.measure == "cpuRatio") {
        const SceneResult* of = findScene(m, b.of);
        if (of && of->cpu.median > 0) b.measured = s->cpu.median / of->cpu.median;
    } else if (b.measure == "passes") {
        double sum = 0.0;
        for (const PassResult& p : s->passes)
            for (const std::string& pattern : b.passes)
                if (matches(pattern, p.name)) {
                    sum += p.s.median;
                    b.ran = true;
                    break;
                }
        b.measured = sum;
    }
}

std::string budgetLabel(const Budget& b) {
    std::string label = b.scene + " " + b.name;
    if (b.measure == "passes" && !(b.passes.size() == 1 && b.passes[0] == b.name)) {
        label += " (";
        for (std::size_t i = 0; i < b.passes.size(); ++i) label += (i ? " + " : "") + b.passes[i];
        label += ")";
    }
    return label;
}

std::string fixed3(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

struct Verdict {
    int passed{0}, failed{0};
    bool noisy{false}, noisyCpu{false}, noisyGpu{false}, noisyLoad{false};
    std::vector<std::string> failures;
};

Verdict evaluate(const Measurement& m, std::vector<Budget>& budgets, int currentStep, double maxSlowdown,
                 double maxGpuSlowdown, double maxExternal) {
    Verdict v;
    v.noisyCpu = m.cpuRef > 0 && std::max(m.cpuCal0, m.cpuCal1) / m.cpuRef > maxSlowdown;
    v.noisyGpu = m.gpuRef > 0 && std::max(m.gpuCal0, m.gpuCal1) / m.gpuRef > maxGpuSlowdown;
    v.noisyLoad = m.external > maxExternal;
    v.noisy = v.noisyCpu || v.noisyGpu || v.noisyLoad;
    for (const SceneResult& s : m.scenes)
        if (!s.error.empty()) {
            ++v.failed;
            v.failures.push_back(s.name + ": " + s.error);
        }
    for (Budget& b : budgets) {
        measure(m, b);
        const bool gated = b.gatedFrom <= currentStep;
        if (!gated) {
            b.status = b.measured ? "pending" : "unmeasured";
            continue;
        }
        if (!b.measured) {
            b.status = "unmeasured";
            ++v.failed;
            v.failures.push_back(budgetLabel(b) + ": gated from step " + std::to_string(b.gatedFrom) +
                                 " but not measured");
            continue;
        }
        if (v.noisy) {
            b.status = "noisy";
            continue;
        }
        // Compared in microseconds (the precision the budgets are written in).
        const bool pass = std::round(*b.measured * 1000.0) <= std::round(b.limit * 1000.0);
        b.status = pass ? "pass" : "over";
        if (pass) {
            ++v.passed;
        } else {
            ++v.failed;
            v.failures.push_back(budgetLabel(b) + ": median " + fixed3(*b.measured) + " " + b.unit + " over its budget " +
                                 fixed3(b.limit) + " " + b.unit);
        }
    }
    return v;
}

// ---- report ---------------------------------------------------------------------------------

void writeStats(reflect::JsonWriter& j, const char* key, const Stats& s, std::optional<double> budget = std::nullopt) {
    j.key(key);
    j.beginObject();
    j.key("median"); j.float32(static_cast<float>(s.median));
    j.key("p95"); j.float32(static_cast<float>(s.p95));
    if (budget) { j.key("budget"); j.float32(static_cast<float>(*budget)); }
    j.endObject();
}

// The budget of a scene's frame, or of a single pass, for the spec's `scenes` shape.
std::optional<double> budgetOf(const std::vector<Budget>& budgets, const std::string& scene, const std::string& measure,
                               const std::string& pass = {}) {
    for (const Budget& b : budgets)
        if (b.scene == scene && b.measure == measure &&
            (pass.empty() || (b.passes.size() == 1 && b.passes[0] == pass)))
            return b.limit;
    return std::nullopt;
}

int report(const Measurement& m, std::vector<Budget>& budgets, bool haveBudgets, int currentStep, const Verdict& v,
           const std::string& outPath) {
    int timedFrames = 0;
    for (const SceneResult& s : m.scenes)
        if (s.name != kMeshletScene) { timedFrames = s.frames; break; }
    std::printf("render_bench (sub-project 2b): %d warm-up + %d timed frames per scene, headless, %s\n", kWarmup,
                timedFrames, m.gpu.empty() ? "GPU unknown" : m.gpu.c_str());
    const auto cal = [](double a, double b, double ref) {
        char buf[128];
        if (ref > 0)
            std::snprintf(buf, sizeof buf, "before %.3f ms  after %.3f ms  reference %.3f ms (x%.2f)", a, b, ref,
                          std::max(a, b) / ref);
        else
            std::snprintf(buf, sizeof buf, "before %.3f ms  after %.3f ms  reference none (run --calibrate)", a, b);
        return std::string(buf);
    };
    std::printf("    CPU calibration  %s\n", cal(m.cpuCal0, m.cpuCal1, m.cpuRef).c_str());
    std::printf("    GPU calibration  %s\n", cal(m.gpuCal0, m.gpuCal1, m.gpuRef).c_str());
    std::printf("    other processes  %.1f%% of a core in total%s\n", m.external, m.busy.empty() ? "" : ":");
    for (const auto& p : m.busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);
    std::printf("    other processes on the GPU: not sampled (the GPU calibration is the contention check)\n");
    for (const SceneResult& s : m.scenes) {
        if (!s.error.empty()) {
            std::printf("    %s: FAILED: %s\n", s.name.c_str(), s.error.c_str());
            continue;
        }
        if (s.name == kMeshletScene) {
            std::printf("    %s, %d builds: median %.3f ms p95 %.3f ms (CPU)\n", s.name.c_str(), s.frames,
                        s.meshletBuild.median, s.meshletBuild.p95);
            continue;
        }
        std::printf("    %s %ux%u, %d frames: frame median %.3f ms p95 %.3f ms (span %.3f, CPU %.3f ms)\n", s.name.c_str(),
                    s.width, s.height, s.frames, s.frame.median, s.frame.p95, s.span.median, s.cpu.median);
        for (const PassResult& p : s.passes)
            std::printf("        %-16s median %7.3f ms  p95 %7.3f ms%s\n", p.name.c_str(), p.s.median, p.s.p95,
                        p.frames == s.frames ? "" : ("  (" + std::to_string(p.frames) + " frames)").c_str());
    }
    if (haveBudgets) {
        std::printf("    budgets (current step %d):\n", currentStep);
        for (const Budget& b : budgets) {
            std::string measured = b.measured ? fixed3(*b.measured) + " " + b.unit : std::string("-");
            if (b.measure == "passes" && b.measured && !b.ran) measured += " (not run)";
            std::string status = b.status;
            if (b.status == "pending" || b.status == "unmeasured")
                status += " (gated from step " + std::to_string(b.gatedFrom) + ")";
            else if (b.status == "over")
                status = "OVER";
            std::printf("        %-52s %-18s budget %-10s %s\n", budgetLabel(b).c_str(), measured.c_str(),
                        (fixed3(b.limit) + " " + b.unit).c_str(), status.c_str());
        }
    }

    std::vector<std::string> failures = v.failures;
    if (v.noisy) {
        std::string why;
        if (v.noisyCpu) why += "the CPU calibration is more than 25% over its reference";
        if (v.noisyGpu) why += std::string(why.empty() ? "" : "; ") + "the GPU calibration is more than 25% over its reference";
        if (v.noisyLoad) why += std::string(why.empty() ? "" : "; ") + "other processes were busy";
        std::printf("    NOISY: %s; the numbers above are not a verdict on the code\n", why.c_str());
        failures.push_back("noisy run: " + why);
    }

    reflect::JsonWriter j(8192);
    j.beginObject();
    j.key("suite"); j.string("render_bench");
    j.key("passed"); j.int64(v.passed);
    j.key("failed"); j.int64(v.failed + (v.noisy ? 1 : 0));
    j.key("failures");
    j.beginArray();
    for (const auto& f : failures) j.string(f);
    j.endArray();
    j.key("noisy"); j.boolean(v.noisy);
    j.key("verdict"); j.string(v.noisy ? "noisy" : (v.failed ? "over" : "pass"));
    j.key("currentStep"); j.int64(currentStep);
    j.key("gpu"); j.string(m.gpu);
    j.key("calibration");
    j.beginObject();
    for (const auto& [key, a, b, ref] : {std::tuple{"cpu", m.cpuCal0, m.cpuCal1, m.cpuRef},
                                         std::tuple{"gpu", m.gpuCal0, m.gpuCal1, m.gpuRef}}) {
        j.key(key);
        j.beginObject();
        j.key("beforeMs"); j.float32(static_cast<float>(a));
        j.key("afterMs"); j.float32(static_cast<float>(b));
        j.key("referenceMs"); j.float32(static_cast<float>(ref));
        j.endObject();
    }
    j.endObject();
    j.key("externalLoad");
    j.beginObject();
    j.key("totalCorePct"); j.float32(static_cast<float>(m.external));
    j.key("busy");
    j.beginArray();
    for (const auto& p : m.busy) {
        j.beginObject();
        j.key("name"); j.string(p.name);
        j.key("corePct"); j.float32(static_cast<float>(p.corePct));
        j.endObject();
    }
    j.endArray();
    j.key("gpu"); j.string("unknown");
    j.endObject();
    j.key("scenes");
    j.beginArray();
    for (const SceneResult& s : m.scenes) {
        j.beginObject();
        j.key("name"); j.string(s.name);
        j.key("width"); j.uint64(s.width);
        j.key("height"); j.uint64(s.height);
        j.key("frames"); j.int64(s.frames);
        if (!s.error.empty()) { j.key("error"); j.string(s.error); }
        writeStats(j, "frame", s.frame, budgetOf(budgets, s.name, "frame"));
        writeStats(j, "span", s.span);
        writeStats(j, "cpu", s.cpu, budgetOf(budgets, s.name, "cpu"));
        if (s.name == kMeshletScene) writeStats(j, "meshletBuild", s.meshletBuild, budgetOf(budgets, s.name, "meshletBuild"));
        j.key("passes");
        j.beginArray();
        for (const PassResult& p : s.passes) {
            j.beginObject();
            j.key("name"); j.string(p.name);
            j.key("median"); j.float32(static_cast<float>(p.s.median));
            j.key("p95"); j.float32(static_cast<float>(p.s.p95));
            j.key("frames"); j.int64(p.frames);
            if (const auto b = budgetOf(budgets, s.name, "passes", p.name)) { j.key("budget"); j.float32(static_cast<float>(*b)); }
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();
    j.key("budgets");
    j.beginArray();
    for (const Budget& b : budgets) {
        j.beginObject();
        j.key("scene"); j.string(b.scene);
        j.key("name"); j.string(b.name);
        j.key("measure"); j.string(b.measure);
        if (!b.passes.empty()) {
            j.key("passes");
            j.beginArray();
            for (const auto& p : b.passes) j.string(p);
            j.endArray();
        }
        j.key("budget"); j.float32(static_cast<float>(b.limit));
        j.key("unit"); j.string(b.unit);
        j.key("gatedFrom"); j.int64(b.gatedFrom);
        if (b.measured) { j.key("measured"); j.float32(static_cast<float>(*b.measured)); }
        j.key("status"); j.string(b.status);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    if (!outPath.empty()) {
        if (std::ofstream f(outPath, std::ios::binary | std::ios::trunc); f) f << j.str() << "\n";
        else std::fprintf(stderr, "render_bench: cannot write %s\n", outPath.c_str());
    }

    // The shared summary line; "noisy" lets tools/check.py rerun a noisy run.
    std::string line = "{\"suite\":\"render_bench\",\"passed\":" + std::to_string(v.passed) +
                       ",\"failed\":" + std::to_string(v.failed + (v.noisy ? 1 : 0)) + ",\"failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    line += "]";
    if (v.noisy) line += ",\"noisy\":true";
    line += "}";
    std::printf("Results: %d/%d passed\n%s\n", v.passed, v.passed + v.failed, line.c_str());
    // A scene that could not render is wrong whatever the machine did.
    for (const SceneResult& s : m.scenes)
        if (!s.error.empty()) return 1;
    return v.noisy ? 2 : (v.failed ? 1 : 0);
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

// ---- the GPU part ---------------------------------------------------------------------------

struct Gpu {
    std::unique_ptr<ecs::FileAssetResolver> assets;
    std::unique_ptr<host::RenderHost> host;   // before the world: it must outlive it
    std::unique_ptr<ecs::World> world;
    std::unique_ptr<api::Session> session;
    bool vfxInstalled{false};
    Gpu() {
        assets = std::make_unique<ecs::FileAssetResolver>(".");
        host = std::make_unique<host::RenderHost>(*assets, api::HostKind::Headless);
        world = std::make_unique<ecs::World>();
        session = std::make_unique<api::Session>(*world, engineTypes());
        session->setAssetResolver(assets.get());
        ecs::installAnimation(*world, *assets);   // anim16's poses, as the CLI context installs it
        host->bind(*world);
        session->setHost(host.get());
    }
    ~Gpu() {
        session.reset();
        world.reset();   // releases its RenderScene slots while the renderer is alive
        host.reset();
    }

    // Loads the scene and renders `frames` frames of it; empty on success, else why not.
    std::string render(const SceneSpec& spec, int frames) {
        if (spec.vfx && !vfxInstalled) {
            // As the CLI context installs them (before the load: the play scene's bodies and records).
            ecs::installPhysics(*world, *assets);
            ecs::installCombat(*world);
            ecs::installVfx(*world, *assets);
            vfxInstalled = true;
        }
        const std::string loaded =
            session->call("scene.load", std::string(R"({"replace":true,"path":")") + spec.path + "\"}");
        if (loaded.rfind("{\"result\"", 0) != 0) return "scene.load failed: " + loaded.substr(0, 300);
        if (spec.vfx) {
            // The script: G throws, fire held; the steps up to the boom (the frames step on with it held).
            const std::string inject = session->call(
                "input.inject", R"({"events":[{"action":"grenade","down":true},{"action":"fire","down":true},)"
                                R"({"step":1,"action":"grenade","down":false}]})");
            if (inject.rfind("{\"result\"", 0) != 0) return "input.inject failed: " + inject.substr(0, 300);
            const std::string stepped =
                session->call("world.step", "{\"frames\":" + std::to_string(kVfxScriptSteps) + "}");
            if (stepped.rfind("{\"result\"", 0) != 0) return "world.step failed: " + stepped.substr(0, 300);
        }
        api::RenderRequest r;
        r.mode = "deferred";
        r.frames = spec.animate ? 1 : frames;   // animated: the capture sets the view and the passes up
        r.width = spec.width;
        r.height = spec.height;
        r.post = spec.post;
        r.exposure = spec.exposure;
        r.hud = spec.hud;
        const api::RenderOutcome o = host->capture(r);
        if (!o.errors.empty()) return o.errors.front().code + ": " + o.errors.front().message;
        if (spec.animate) {
            // A window's frame: a fixed step, one sync, the frame (the poses change every frame).
            VulkanRenderer* renderer = host->renderer();
            for (int f = 0; f < frames; ++f) {
                world->simulate(1.0f / 60.0f);
                world->sync();
                renderer->render();
            }
            renderer->waitForSubmitted(renderer->framesSubmitted());
        }
        return {};
    }
};

// The calibration kernel's SPIR-V, from the shaders beside this executable (bin/shaders).
VkShaderModule loadKernel(VkDevice device) {
    fs::path dir = fs::current_path();
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) dir = fs::path(self).parent_path();
#endif
    const std::string code = readText(dir / "bin" / "shaders" / "bench_gpu_calibrate.slang.spv");
    if (code.empty() || code.size() % 4) return VK_NULL_HANDLE;
    std::vector<uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size();
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

int calibrateReference(const std::string& refPath) {
    const double cpu = bench::calibrationMs();
    Gpu gpu;
    const std::string err = gpu.render(sceneSpecs().front(), 1);
    if (!err.empty() || !gpu.host->renderer()) {
        std::fprintf(stderr, "render_bench: no renderer for the GPU calibration (%s)\n", err.c_str());
        return 2;
    }
    VulkanRenderer& r = *gpu.host->renderer();
    bench::GpuCalibration kernel;
    const VkShaderModule module = loadKernel(r.getDevice());
    const bool ok = kernel.init(r.getDevice(), r.getPhysicalDevice(), r.graphicsQueue(), r.graphicsQueueFamily(), module);
    const double gpuMs = ok ? kernel.calibrationMs(kGpuCalibrationIterations) : -1.0;
    kernel.destroy();
    if (module) vkDestroyShaderModule(r.getDevice(), module, nullptr);
    if (gpuMs <= 0) {
        std::fprintf(stderr, "render_bench: the GPU calibration kernel did not run\n");
        return 2;
    }
    std::ofstream f(refPath, std::ios::binary | std::ios::trunc);
    f << "{\"calibrationMs\":" << cpu << ",\"gpuCalibrationMs\":" << gpuMs << "}\n";
    if (!f) {
        std::fprintf(stderr, "render_bench: cannot write %s\n", refPath.c_str());
        return 2;
    }
    std::printf("calibration CPU %.3f ms, GPU %.3f ms written to %s\n", cpu, gpuMs, refPath.c_str());
    return 0;
}

// --geometry / --fallback: GPU culling's meshlet settings for every scene (default: the
// renderer's, geometry auto on task and mesh shaders).
struct GeometryChoice {
    std::optional<GpuCulling::Geometry> geometry;
    bool fallback{false};
};

Measurement run(const std::string& only, int timed, const GeometryChoice& choice) {
    Measurement m;
    if (!writeBenchScenes(only)) std::fprintf(stderr, "render_bench: the bench scenes could not be written\n");
    bench::LoadMonitor monitor;
    monitor.start();
    m.cpuCal0 = bench::calibrationMs();
    if (only.empty() || only == kMeshletScene) {
        const std::vector<double> ms = timeMeshletBuild();
        SceneResult res;
        res.name = kMeshletScene;
        res.frames = static_cast<int>(ms.size());
        res.meshletBuild = stats(ms);
        m.scenes.push_back(std::move(res));
        if (only == kMeshletScene) {
            m.cpuCal1 = bench::calibrationMs();
            m.busy = monitor.stop(5.0, &m.external);
            return m;
        }
    }
    Gpu gpu;
    // The renderer starts on the first capture; the GPU calibration needs its device.
    const SceneSpec& first = sceneSpecs().front();
    const std::string startErr = gpu.render(first, 1);
    bench::GpuCalibration kernel;
    VkShaderModule module = VK_NULL_HANDLE;
    VulkanRenderer* r = gpu.host->renderer();
    if (startErr.empty() && r && r->getDeferredRenderer() && r->getDeferredRenderer()->culling()) {
        GpuCulling* culling = r->getDeferredRenderer()->culling();
        GpuCulling::Settings s = culling->settings();
        if (choice.geometry) s.geometry = *choice.geometry;
        if (choice.fallback) s.meshShaders = false;
        culling->setSettings(s);
    }
    if (startErr.empty() && r) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(r->getPhysicalDevice(), &props);
        m.gpu = props.deviceName;
        module = loadKernel(r->getDevice());
        if (kernel.init(r->getDevice(), r->getPhysicalDevice(), r->graphicsQueue(), r->graphicsQueueFamily(), module))
            m.gpuCal0 = kernel.calibrationMs(kGpuCalibrationIterations);
    }
    for (const SceneSpec& spec : sceneSpecs()) {
        if (!only.empty() && only != spec.name) continue;
        SceneResult res;
        std::string err = startErr.empty() ? gpu.render(spec, kWarmup + timed) : startErr;
        if (err.empty()) {
            const std::vector<render::FrameTiming> frames = gpu.host->renderer()->frameTimings(static_cast<std::size_t>(timed));
            if (static_cast<int>(frames.size()) < timed)
                err = "only " + std::to_string(frames.size()) + " of " + std::to_string(timed) + " frames were timed";
            else
                res = summarize(spec, frames);
        }
        if (!err.empty()) {
            res.name = spec.name;
            res.width = spec.width;
            res.height = spec.height;
            res.error = err;
        }
        m.scenes.push_back(std::move(res));
    }
    if (m.gpuCal0 > 0) m.gpuCal1 = kernel.calibrationMs(kGpuCalibrationIterations);
    kernel.destroy();
    if (module) vkDestroyShaderModule(r->getDevice(), module, nullptr);
    m.cpuCal1 = bench::calibrationMs();
    m.busy = monitor.stop(5.0, &m.external);
    return m;
}

} // namespace

int main(int argc, char** argv) {
    std::error_code ec;
    fs::current_path(OHAO_SOURCE_DIR, ec);   // scenes, assets and SPIR-V are found from the repo root
    std::string checkPath, outPath = "build/render_bench.json", refPath = "build/render_bench_reference.json";
    std::string evaluatePath, only;
    bool calibrate = false, outGiven = false;
    int timed = kTimed;
    GeometryChoice geometry;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) { outPath = argv[++i]; outGiven = true; }
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--evaluate" && i + 1 < argc) evaluatePath = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--frames" && i + 1 < argc) timed = std::clamp(std::atoi(argv[++i]), 10, 110);
        else if (a == "--calibrate") calibrate = true;
        else if (a == "--geometry" && i + 1 < argc && std::strcmp(argv[i + 1], "instances") == 0) geometry.geometry = GpuCulling::Geometry::Instances, ++i;
        else if (a == "--geometry" && i + 1 < argc && std::strcmp(argv[i + 1], "auto") == 0) geometry.geometry = GpuCulling::Geometry::Auto, ++i;
        else if (a == "--geometry" && i + 1 < argc && std::strcmp(argv[i + 1], "meshlets") == 0) geometry.geometry = GpuCulling::Geometry::Meshlets, ++i;
        else if (a == "--fallback") geometry.fallback = true;
        else {
            std::fprintf(stderr, "usage: render_bench [--check budgets.json] [--out file] [--reference file] "
                                 "[--frames N] [--only SCENE] [--geometry instances|auto|meshlets] [--fallback] | "
                                 "--calibrate | --evaluate results.json --check budgets.json\n");
            return 2;
        }
    }
    log::removeSink(log::stderrSink().get());   // the engine's logging stays in the ring
    if (calibrate) return calibrateReference(refPath);

    std::string budgetText;
    if (!checkPath.empty()) {
        budgetText = readText(checkPath);
        if (budgetText.empty()) {
            std::fprintf(stderr, "render_bench: cannot read %s\n", checkPath.c_str());
            return 2;
        }
    }
    int currentStep = 0;
    std::vector<Budget> budgets;
    if (!budgetText.empty()) {
        auto doc = ecs::parseJson(budgetText);
        if (!doc) {
            std::fprintf(stderr, "render_bench: %s is not JSON\n", checkPath.c_str());
            return 2;
        }
        budgets = readBudgets(doc.value(), currentStep);
    }
    const double maxSlowdown = jsonNumber(budgetText, "maxCalibrationSlowdown", 1.25);
    const double maxGpuSlowdown = jsonNumber(budgetText, "maxGpuCalibrationSlowdown", 1.25);
    const double maxExternal = jsonNumber(budgetText, "maxExternalCorePct", 50.0);

    Measurement m;
    if (!evaluatePath.empty()) {
        std::string why;
        const std::string text = readText(evaluatePath);
        auto read = text.empty() ? std::nullopt : readMeasurement(text, why);
        if (!read) {
            std::fprintf(stderr, "render_bench: cannot read results %s%s%s\n", evaluatePath.c_str(),
                         why.empty() ? "" : ": ", why.c_str());
            return 2;
        }
        m = std::move(*read);
        if (!outGiven) outPath.clear();   // never overwrite a real run's results with a fixture's
    } else {
#ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
        m = run(only, timed, geometry);
        const std::string ref = readText(refPath);
        m.cpuRef = bench::readReference(refPath);
        m.gpuRef = jsonNumber(ref, "gpuCalibrationMs", 0.0);
    }
    const Verdict v = evaluate(m, budgets, currentStep, maxSlowdown, maxGpuSlowdown, maxExternal);
    return report(m, budgets, !budgetText.empty(), currentStep, v, outPath);
}
