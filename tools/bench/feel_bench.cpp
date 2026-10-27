// feel_bench -- input-to-present latency and frame pacing in a real window (sub-project 9 spec,
// "Performance budgets"). For each present mode: a visible window plays the play scene for a
// scripted minute (walking back and forth, firing bursts, the enemies fighting back) while a mouse
// movement is fed every 40-60 ms as a device's would be; the window's frame log gives each movement's
// latency (poll to the present of the first frame showing it in full) and each frame's time, phases
// and steps. A hidden window may never present, so the window is shown.
//
//   feel_bench [--check budgets.json] [--out build/feel_bench.json] [--seconds 60] [--warmup 2]
//              [--modes immediate,mailbox,fifo] [--csv-dir build] [--frames-ahead N]
//
// Per mode, feel_bench_<mode>.csv (every frame: time, steps, phases, the latency of each movement it
// showed) and feel_bench_<mode>_events.csv (the round's deaths, restarts, wins and reloads), on one
// clock. Budgets (tests/bench/feel_budgets.json): look_latency_ms (median) and frame_p99_over_median,
// each measured in the present mode it names and gated from the build step it names; the latency
// budget is gated only on a display of at least minRefreshHz with present wait, else it is reported
// pending. Other processes' CPU is recorded over each measured minute (dwm.exe aside): beside more than
// half a core, a gated budget is `noisy`. Exit 0: no gated budget missed; 1: one did on a quiet run,
// or a run measured nothing; 2: noisy (tools/check.py reruns it).

#include "common/harness.hpp"
#include "ecs/combat.hpp"
#include "ecs/scene_types.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "runtime/frame_log.hpp"
#include "runtime/live_host.hpp"

#include "bench_env.hpp"   // windows.h: after the engine's headers (its OPAQUE macro)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace ohao;
using namespace ohao::runtime;

namespace {

using Clock = std::chrono::steady_clock;

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

struct ModeRun {
    PresentMode want{PresentMode::Mailbox};
    PresentMode got{PresentMode::Mailbox};
    bool ran{false};
    int refreshHz{0};
    bool presentWait{false};
    double seconds{0.0};
    std::vector<FrameStamps> frames;   // after the warm-up
    // The round's turns (deaths, restarts, wins, reloads) by when their step ran: a spike in the
    // frame CSV read beside what the game did then.
    std::vector<std::pair<Clock::time_point, std::string>> events;
    std::optional<LatencyStats> latency;
    std::optional<PacingStats> pacing;
    // Other processes' CPU over the measured minute, the compositor (dwm.exe, which presents this
    // window) left out: a run beside busy processes is noisy (spec ruling 17).
    double externalCorePct{0.0};
    std::vector<bench::BusyProcess> busy;
};

InputEvent axis(const char* name, float value) {
    InputEvent e;
    e.type = InputEvent::Type::AxisSet;
    e.name = name;
    e.value = value;
    return e;
}

InputEvent action(const char* name, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
    e.name = name;
    return e;
}

// One present mode's scripted minute.
ModeRun run(PresentMode mode, double seconds, double warmup, int framesAhead) {
    ModeRun r;
    r.want = mode;
    LiveConfig cfg;
    cfg.window.title = "OHAO feel_bench";
    cfg.window.width = 1920;
    cfg.window.height = 1080;
    cfg.window.resizable = false;
    cfg.present = mode;
    cfg.timeMode = TimeMode::realtime;
    cfg.root = OHAO_SOURCE_DIR;
    cfg.frameLogCapacity = 1u << 20;
    cfg.framesAhead = framesAhead;
    LiveHost host;
    if (!host.initialize(cfg, engineTypes())) return r;
    const std::string scene = std::string(OHAO_SOURCE_DIR) + "/assets/scenes/blockout_play.scene.json";
    const std::string loaded = host.session().call("scene.load", "{\"path\":\"" + scene + "\"}");
    if (loaded.rfind("{\"result\":", 0) != 0) {
        std::fprintf(stderr, "feel_bench: the play scene did not load: %s\n", loaded.c_str());
        (void)host.shutdown();
        return r;
    }
    // The look ohao_game plays with: post effects on at exposure 1.
    if (auto* deferred = host.renderer().getDeferredRenderer()) {
        if (auto* pp = deferred->getPostProcessing()) {
            pp->setBloomEnabled(true);
            pp->setTAAEnabled(true);
            pp->setSSAOEnabled(true);
            pp->setExposure(1.0f);
        }
    }

    // The script, by time since the start: a mouse movement every 40 to 60 ms (alternating left and
    // right, so the view swings and stays in the room); forward 1.5 s, still 0.5 s, back 1.5 s,
    // still 0.5 s; fire held 1 s of every 3. The movements' spacing follows a golden-ratio sequence:
    // a fixed interval that is a multiple of the 1/60 s step (50 ms is three) would land every
    // movement at the same phase of the step clock and measure one waiting time only.
    const Clock::time_point start = Clock::now();
    int mouseSent = 0;
    double nextMouse = 0.0;
    float forward = -2.0f;
    bool firing = false;
    bench::LoadMonitor load;
    bool loadStarted = false;
    GameHooks hooks;
    hooks.update = [&](LiveHost& h, float, float) {
        const double t = std::chrono::duration<double>(Clock::now() - start).count();
        if (t >= warmup && !loadStarted) {
            load.start();
            loadStarted = true;
        }
        if (t >= warmup + seconds) {
            h.quit();
            return;
        }
        std::vector<InputEvent> events;
        while (nextMouse <= t) {
            InputEvent e;
            e.type = InputEvent::Type::MouseDelta;
            e.x = mouseSent % 2 ? 8.0f : -8.0f;
            events.push_back(e);
            ++mouseSent;
            const double u = std::fmod(mouseSent * 0.6180339887498949, 1.0);
            nextMouse += 0.040 + 0.020 * u;
        }
        const double c = std::fmod(t, 4.0);
        const float want = c < 1.5 ? 1.0f : c < 2.0 ? 0.0f : c < 3.5 ? -1.0f : 0.0f;
        if (want != forward) events.push_back(axis("move_forward", forward = want));
        const bool fire = std::fmod(t, 3.0) < 1.0;
        if (fire != firing) events.push_back(action("fire", firing = fire));
        h.feedEvents(events);
    };
    hooks.fixedUpdate = [&](LiveHost& h, float) {
        for (const ecs::CombatEvent& e : ecs::combatEvents(h.world()))
            if (e.kind != ecs::CombatEvent::Kind::Shot && e.kind != ecs::CombatEvent::Kind::Hit &&
                e.kind != ecs::CombatEvent::Kind::Damage)
                r.events.emplace_back(Clock::now(), ecs::combatEventKindName(e.kind));
    };
    (void)host.run(hooks);
    if (loadStarted) {
        double total = 0.0;
        for (bench::BusyProcess& p : load.stop(0.0, &total)) {
            if (p.name == "dwm.exe") {
                total -= p.corePct;
                continue;
            }
            if (p.corePct >= 5.0) r.busy.push_back(std::move(p));
        }
        r.externalCorePct = std::max(total, 0.0);
    }

    r.ran = true;
    r.got = host.renderer().presentMode();
    r.presentWait = host.renderer().presentWaitEnabled();
    r.refreshHz = host.window().refreshRate();
    r.seconds = seconds;
    const Clock::time_point measured = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(warmup));
    FrameLog kept(1u << 20);
    for (const FrameStamps& f : host.frameLog().frames())
        if (f.start >= measured) {
            r.frames.push_back(f);
            kept.record(f);
        }
    const int all = static_cast<int>(r.frames.size());
    r.latency = kept.latency(all);
    r.pacing = kept.pacing(all);
    (void)host.shutdown();
    return r;
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

void writeCsv(const std::filesystem::path& path, const ModeRun& r) {
    std::ofstream out(path, std::ios::binary);
    // A frame's phases (for reading a spike: frame_ms is the time since the previous frame began, so
    // the phase that took it is the previous row's): start to poll (the late input wait), poll to view
    // (the steps and sync), view to submit (recording), submit to present.
    out << "t_ms,frame_ms,steps,discarded,latency_ms,present_wait,poll_ms,view_ms,submit_ms,present_ms\n";
    if (r.frames.empty()) return;
    const Clock::time_point t0 = r.frames.front().start;
    for (const FrameStamps& f : r.frames) {
        const std::string t = std::to_string(ms(f.start - t0)), ft = std::to_string(f.frameMs);
        const std::string phases = std::to_string(ms(f.poll - f.start)) + ',' + std::to_string(ms(f.view - f.poll)) + ',' +
                                   std::to_string(ms(f.submit - f.view)) + ',' +
                                   (f.present ? std::to_string(ms(*f.present - f.submit)) : std::string());
        if (f.looks.empty() || !f.present) {
            out << t << ',' << ft << ',' << f.steps << ',' << f.discardedSteps << ",," << (f.presentWait ? 1 : 0) << ','
                << phases << '\n';
            continue;
        }
        for (const LookSample& l : f.looks)
            out << t << ',' << ft << ',' << f.steps << ',' << f.discardedSteps << ',' << ms(*f.present - l.poll) << ','
                << (f.presentWait ? 1 : 0) << ',' << phases << '\n';
    }
}

void writeEventsCsv(const std::filesystem::path& path, const ModeRun& r) {
    std::ofstream out(path, std::ios::binary);
    out << "t_ms,event\n";
    if (r.frames.empty()) return;
    const Clock::time_point t0 = r.frames.front().start;
    for (const auto& [at, name] : r.events)
        if (at >= t0) out << ms(at - t0) << ',' << name << '\n';
}

struct Verdict {
    std::string verdict{"reported"}, why;
    double value{0.0}, max{0.0};
    int from{0};
};

// A frame over this is a stall (the presentation engine holding the window's images; spec "As built
// (step 2)"): counted beside the pacing numbers.
constexpr double kStallMs = 100.0;
// Other processes using more than half a core in total make a run noisy (the other benches' rule).
constexpr double kMaxExternalCorePct = 50.0;

int stalls(const ModeRun& r) {
    return static_cast<int>(std::count_if(r.frames.begin(), r.frames.end(), [](const FrameStamps& f) { return f.frameMs > kStallMs; }));
}

} // namespace

int main(int argc, char** argv) {
    std::string checkPath, outPath = "build/feel_bench.json", csvDir = "build";
    double seconds = 60.0, warmup = 2.0;
    int framesAhead = -1;   // LiveConfig::framesAhead: the window's own choice
    std::vector<PresentMode> modes{PresentMode::Immediate, PresentMode::Mailbox, PresentMode::Fifo};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--csv-dir" && i + 1 < argc) csvDir = argv[++i];
        else if (a == "--frames-ahead" && i + 1 < argc) framesAhead = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::max(1.0, std::atof(argv[++i]));
        else if (a == "--warmup" && i + 1 < argc) warmup = std::max(0.0, std::atof(argv[++i]));
        else if (a == "--modes" && i + 1 < argc) {
            modes.clear();
            std::string list = argv[++i];
            for (std::size_t p = 0; p <= list.size();) {
                const std::size_t q = std::min(list.find(',', p), list.size());
                const auto m = presentModeFromName(list.substr(p, q - p));
                if (!m) {
                    std::fprintf(stderr, "feel_bench: --modes takes immediate, mailbox, fifo\n");
                    return 1;
                }
                modes.push_back(*m);
                p = q + 1;
            }
        } else {
            std::fprintf(stderr, "usage: feel_bench [--check budgets.json] [--out file] [--seconds N] [--warmup N] "
                                 "[--modes immediate,mailbox,fifo] [--csv-dir DIR] [--frames-ahead N]\n");
            return 1;
        }
    }
    nlohmann::json budgets = nlohmann::json::object();
    int currentStep = 0;
    if (!checkPath.empty()) {
        std::ifstream f(checkPath);
        const nlohmann::json doc = nlohmann::json::parse(f, nullptr, false);
        if (doc.is_discarded()) {
            std::fprintf(stderr, "feel_bench: %s is not JSON\n", checkPath.c_str());
            return 1;
        }
        budgets = doc.value("budgets", nlohmann::json::object());
        currentStep = doc.value("currentStep", 0);
    }

    std::printf("feel_bench: the play scene, %.0f s a present mode after %.0f s of warm-up\n", seconds, warmup);
    std::vector<ModeRun> runs;
    for (const PresentMode m : modes) {
        runs.push_back(run(m, seconds, warmup, framesAhead));
        const ModeRun& r = runs.back();
        if (!r.ran) {
            std::printf("  %-9s did not run (no window or GPU)\n", std::string(presentModeName(m)).c_str());
            continue;
        }
        std::error_code ec;
        std::filesystem::create_directories(csvDir, ec);
        writeCsv(std::filesystem::path(csvDir) / ("feel_bench_" + std::string(presentModeName(m)) + ".csv"), r);
        writeEventsCsv(std::filesystem::path(csvDir) / ("feel_bench_" + std::string(presentModeName(m)) + "_events.csv"), r);
        std::printf("  %-9s (surface gave %s, %d Hz, %s)\n", std::string(presentModeName(m)).c_str(),
                    std::string(presentModeName(r.got)).c_str(), r.refreshHz, r.presentWait ? "present wait" : "present call");
        if (r.latency)
            std::printf("    look latency  median %7.2f ms  p95 %7.2f ms  (%d movements; poll->consume %.2f, poll->view %.2f, "
                        "view->submit %.2f, submit->present %.2f)\n",
                        r.latency->medianMs, r.latency->p95Ms, r.latency->samples, r.latency->pollToConsumeMs,
                        r.latency->pollToViewMs, r.latency->viewToSubmitMs, r.latency->submitToPresentMs);
        if (r.pacing)
            std::printf("    frame time    median %7.3f ms  p95 %7.3f ms  p99 %7.3f ms  (p99/median %.2f; %d frames; "
                        "steps 0:%llu 1:%llu 2:%llu 3+:%llu; discarded %llu)\n",
                        r.pacing->medianMs, r.pacing->p95Ms, r.pacing->p99Ms,
                        r.pacing->medianMs > 0 ? r.pacing->p99Ms / r.pacing->medianMs : 0.0, r.pacing->frames,
                        static_cast<unsigned long long>(r.pacing->steps[0]), static_cast<unsigned long long>(r.pacing->steps[1]),
                        static_cast<unsigned long long>(r.pacing->steps[2]), static_cast<unsigned long long>(r.pacing->steps[3]),
                        static_cast<unsigned long long>(r.pacing->discardedSteps));
        std::printf("    stalls        %d frames over %.0f ms; other processes %.1f%% of a core (dwm.exe aside)%s\n",
                    stalls(r), kStallMs, r.externalCorePct, r.busy.empty() ? "" : ":");
        for (const auto& p : r.busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);
    }

    std::vector<std::string> failures;
    bool noisy = false;
    const auto find = [&](const nlohmann::json& b) -> const ModeRun* {
        const auto want = presentModeFromName(b.value("present", std::string("immediate")));
        for (const ModeRun& r : runs)
            if (want && r.want == *want && r.ran) return &r;
        return nullptr;
    };
    const auto judge = [&](const std::string& name, bool latency) {
        Verdict v;
        if (!budgets.contains(name)) return v;
        const nlohmann::json& b = budgets[name];
        v.max = b.value("max", 0.0);
        v.from = b.value("gatedFrom", 0);
        const ModeRun* r = find(b);
        if (!r || (latency ? !r->latency : !r->pacing)) {
            v.verdict = "not measured";
            v.why = "no run in the budget's present mode measured it";
            return v;
        }
        v.value = latency ? r->latency->medianMs : r->pacing->p99Ms / std::max(r->pacing->medianMs, 1e-9);
        if (currentStep < v.from) {
            v.verdict = "pending";
            v.why = "gated from step " + std::to_string(v.from);
        } else if (latency && (r->refreshHz < b.value("minRefreshHz", 0) || !r->presentWait)) {
            v.verdict = "pending";
            v.why = r->presentWait ? "the display runs at " + std::to_string(r->refreshHz) + " Hz" : "no present wait";
        } else if (r->externalCorePct > kMaxExternalCorePct) {
            v.verdict = "noisy";
            v.why = "other processes used " + std::to_string(static_cast<int>(r->externalCorePct)) + "% of a core";
            noisy = true;
        } else {
            v.verdict = v.value <= v.max ? "pass" : "fail";
            if (v.verdict == "fail") failures.push_back(name + ": " + std::to_string(v.value) + " over its budget of " + std::to_string(v.max));
        }
        return v;
    };
    const Verdict vl = judge("look_latency_ms", true);
    const Verdict vp = judge("frame_p99_over_median", false);
    for (const auto& [name, v] : {std::pair{"look_latency_ms", vl}, std::pair{"frame_p99_over_median", vp}})
        if (budgets.contains(name))
            std::printf("  %-22s %8.3f  budget %.3f, %s%s%s\n", name, v.value, v.max, v.verdict.c_str(),
                        v.why.empty() ? "" : ": ", v.why.c_str());

    nlohmann::json out;
    out["seconds"] = seconds;
    out["warmup"] = warmup;
    nlohmann::json modesJson = nlohmann::json::array();
    for (const ModeRun& r : runs) {
        nlohmann::json m = {{"present", std::string(presentModeName(r.want))}, {"ran", r.ran}};
        if (r.ran) {
            m["surfaceGave"] = std::string(presentModeName(r.got));
            m["refreshHz"] = r.refreshHz;
            m["presentWait"] = r.presentWait;
            m["frames"] = r.frames.size();
            if (r.latency)
                m["latency"] = {{"medianMs", r.latency->medianMs}, {"p95Ms", r.latency->p95Ms}, {"samples", r.latency->samples},
                                {"allPresentWait", r.latency->presentWait},
                                {"stages", {{"pollToConsumeMs", r.latency->pollToConsumeMs}, {"pollToViewMs", r.latency->pollToViewMs},
                                            {"viewToSubmitMs", r.latency->viewToSubmitMs},
                                            {"submitToPresentMs", r.latency->submitToPresentMs}}}};
            if (r.pacing)
                m["pacing"] = {{"medianMs", r.pacing->medianMs}, {"p95Ms", r.pacing->p95Ms}, {"p99Ms", r.pacing->p99Ms},
                               {"p99OverMedian", r.pacing->medianMs > 0 ? r.pacing->p99Ms / r.pacing->medianMs : 0.0},
                               {"steps", r.pacing->steps}, {"discardedSteps", r.pacing->discardedSteps}};
            m["stalls"] = stalls(r);
            m["externalCorePct"] = r.externalCorePct;
            nlohmann::json busy = nlohmann::json::array();
            for (const auto& p : r.busy) busy.push_back({{"name", p.name}, {"corePct", p.corePct}});
            m["busy"] = busy;
        }
        modesJson.push_back(m);
    }
    out["modes"] = modesJson;
    out["look_latency_ms"] = {{"value", vl.value}, {"max", vl.max}, {"verdict", vl.verdict}, {"why", vl.why}};
    out["frame_p99_over_median"] = {{"value", vp.value}, {"max", vp.max}, {"verdict", vp.verdict}, {"why", vp.why}};
    out["checked"] = !checkPath.empty();
    out["passed"] = failures.empty() && !noisy;
    out["noisy"] = noisy;
    out["failures"] = failures;
    if (const auto dir = std::filesystem::path(outPath).parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';

    bool measured = !runs.empty();
    for (const ModeRun& r : runs) measured = measured && r.ran && r.latency && r.pacing;
    if (!measured) failures.push_back("a present mode's run measured nothing (no window, no GPU, or no look input reached a frame)");
    for (const auto& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    // A noisy run (a gated budget measured beside busy processes) is neither a pass nor a regression:
    // exit 2, which tools/check.py reruns. A failure measured on a quiet run still fails.
    const bool noisyOnly = noisy && failures.empty();
    if (noisyOnly) failures.push_back("noisy run: other processes were busy (see above)");
    std::printf("%s (results in %s)\n", noisyOnly ? "NOISY" : failures.empty() ? "PASS" : "FAIL", outPath.c_str());
    std::string line = "{\"suite\":\"feel_bench\",\"passed\":" + std::to_string(failures.empty() ? 1 : 0) +
                       ",\"failed\":" + std::to_string(failures.empty() ? 0 : 1) + ",\"failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + test::jsonString(failures[i]);
    std::printf("%s]%s}\n", line.c_str(), noisyOnly ? ",\"noisy\":true" : "");
    return noisyOnly ? 2 : failures.empty() ? 0 : 1;
}
