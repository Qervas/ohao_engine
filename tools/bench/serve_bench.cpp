// serve_bench: the protocol's budgets (sub-project 4 spec, "Performance / Budgets").
//
//   serve_bench [--check tests/bench/serve_budgets.json] [--out build/serve_bench.json]
//               [--reference build/serve_bench_reference.json] [--rounds N] [--cpu-only]
//   serve_bench --calibrate        writes the reference calibration (on a quiet machine)
//
// The server is `ohao serve`, a child process on pipes, exactly as an MCP client runs it,
// with bench10k loaded and 100 of its entities spinning. Gated (medians):
//   world_info_rpc           world.info round trip over stdio                    <= 0.15 ms
//   transport_overhead       that round trip minus the in-process Session::call  <= 0.10 ms
//   world_query_one_rpc      world.query, one entity, one projected field         <= 0.20 ms
//   world_apply_100_rpc      world.apply of 100 field.set                         <= 0.40 ms
//   world_step_1_rpc         world.step of 1 frame                                <= 0.50 ms
//   log_below_threshold      OHAO_LOG below the threshold, per call               <= 5 ns
//   log_ring_80              OHAO_LOG into the ring, an 80-character message      <= 300 ns
//   tools_list_bytes         the tools/list result                                <= 24 KB
// Recorded, not gated: process start to the initialize reply, scene.load of bench10k over
// stdio, the initialize and instructions sizes.
//
// Method (spec "Measurement rules"): 5 warm-up and 50 timed rounds; in each round every
// operation runs one block (several calls; the sample is the per-call mean), in a fixed
// interleaved order, so a slow phase of the machine lands on all of them. The in-process
// call runs right beside the round trip it is subtracted from, and the overhead is the
// median of the per-round differences. The server's page faults per call are reported
// beside each round trip. A calibration loop runs before and after the rounds, and every
// other process's CPU time is recorded: a run more than 25% slower than the reference, or
// beside busy processes, is `noisy` (exit 2), neither a pass nor a failure. Exit 1 when a
// gated median is over its budget or a reply is wrong.
//
// GPU budgets (the window host) arrive with the window host (step 9); --cpu-only is
// accepted now and changes nothing.
#include "api/session.hpp"
#include "bench_env.hpp"
#include "bench_world.hpp"
#include "common/harness.hpp"
#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "reflect/json_writer.hpp"

#ifdef _WIN32
#include <psapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace ohao;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Stats {
    double median{0}, p95{0};
};

Stats stats(std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * (v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95)};
}

std::string hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

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

// ---- the server as a child process on pipes --------------------------------------------------

#ifdef _WIN32
class Child {
public:
    ~Child() { close(); }

    bool start(const std::wstring& exe, const std::wstring& args, const fs::path& cwd, const fs::path& errLog) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE inRead = nullptr, outWrite = nullptr;
        if (!CreatePipe(&m_outRead, &outWrite, &sa, 1 << 20) || !CreatePipe(&inRead, &m_inWrite, &sa, 1 << 20))
            return false;
        SetHandleInformation(m_outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_inWrite, HANDLE_FLAG_INHERIT, 0);
        HANDLE err = CreateFileW(errLog.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = err;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\" " + args;
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | HIGH_PRIORITY_CLASS, nullptr, cwd.wstring().c_str(), &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
        if (!ok) return false;
        CloseHandle(pi.hThread);
        m_proc = pi.hProcess;
        m_pid = pi.dwProcessId;
        return true;
    }

    void send(const std::string& line) {
        DWORD n = 0;
        WriteFile(m_inWrite, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    }

    // The next line from the server (without '\n'); empty at end of output.
    std::string readLine() {
        for (;;) {
            const auto nl = m_buf.find('\n', m_scan);
            if (nl != std::string::npos) {
                std::string line = m_buf.substr(0, nl);
                m_buf.erase(0, nl + 1);
                m_scan = 0;
                return line;
            }
            m_scan = m_buf.size();
            char chunk[1 << 16];
            DWORD n = 0;
            if (!ReadFile(m_outRead, chunk, sizeof chunk, &n, nullptr) || n == 0) return {};
            m_buf.append(chunk, n);
        }
    }

    std::string call(const std::string& line) {
        send(line);
        return readLine();
    }

    uint64_t pageFaults() const {
        PROCESS_MEMORY_COUNTERS c{};
        c.cb = sizeof c;
        return m_proc && GetProcessMemoryInfo(m_proc, &c, sizeof c) ? c.PageFaultCount : 0;
    }
    uint32_t pid() const { return m_pid; }

    // Closes stdin and waits for the exit code (-1: none).
    int close() {
        if (m_inWrite) {
            CloseHandle(m_inWrite);
            m_inWrite = nullptr;
        }
        int code = -1;
        if (m_proc) {
            if (WaitForSingleObject(m_proc, 30000) == WAIT_OBJECT_0) {
                DWORD c = 0;
                GetExitCodeProcess(m_proc, &c);
                code = static_cast<int>(c);
            } else {
                TerminateProcess(m_proc, 99);
            }
            CloseHandle(m_proc);
            m_proc = nullptr;
        }
        if (m_outRead) {
            CloseHandle(m_outRead);
            m_outRead = nullptr;
        }
        return code;
    }

private:
    HANDLE m_proc{nullptr}, m_inWrite{nullptr}, m_outRead{nullptr};
    DWORD m_pid{0};
    std::string m_buf;
    std::size_t m_scan{0};
};
#endif

std::string request(int id, std::string_view method, std::string_view params) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"" + std::string(method) +
           "\",\"params\":" + std::string(params) + "}\n";
}

struct Op {
    std::string name;
    std::string unit;                 // "ms" or "ns"
    int calls{1};                     // per block
    // Runs one block of `calls` calls, the first numbered `first`; the loop is inside, so
    // the harness adds no per-call cost (it would show in a 1 ns operation).
    std::function<void(int first, int calls)> run;
    std::vector<double> samples;      // per-call mean of each block, in `unit`
    std::vector<double> faults;       // the server's page faults per call (round trips)
    bool serverFaults{false};
};

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    std::fprintf(stderr, "serve_bench: the child-process client is Windows-only for now\n");
    return 2;
#else
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    const fs::path root = OHAO_SOURCE_DIR;
    std::string checkPath, outPath = "build/serve_bench.json", refPath = "build/serve_bench_reference.json";
    int rounds = 50;
    bool calibrate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--rounds" && i + 1 < argc) rounds = std::max(5, std::atoi(argv[++i]));
        else if (a == "--calibrate") calibrate = true;
        else if (a == "--cpu-only") {}
        else {
            std::fprintf(stderr, "usage: serve_bench [--check budgets.json] [--out file] [--reference file] "
                                 "[--rounds N] [--cpu-only] | --calibrate\n");
            return 2;
        }
    }
    if (calibrate) {
        const double c = bench::calibrationMs();
        if (!bench::writeReference(refPath, c)) {
            std::fprintf(stderr, "serve_bench: cannot write %s\n", refPath.c_str());
            return 2;
        }
        std::printf("calibration %.3f ms written to %s\n", c, refPath.c_str());
        return 0;
    }

    std::string budgets;
    if (!checkPath.empty()) {
        budgets = readText(checkPath);
        if (budgets.empty()) {
            std::fprintf(stderr, "serve_bench: cannot read %s\n", checkPath.c_str());
            return 2;
        }
    }
    const double maxSlowdown = jsonNumber(budgets, "maxCalibrationSlowdown", 1.25);
    const double maxExternal = jsonNumber(budgets, "maxExternalCorePct", 50.0);
    log::removeSink(log::stderrSink().get());   // the bench's own logging stays in the ring

    // bench10k with 100 spinners: written once for the server, loaded the same way in-process.
    const fs::path dir = root / "build" / "serve_bench";
    fs::create_directories(dir);
    {
        ecs::World w;
        bench::buildBenchWorld(w, 10'000, 2026);
        auto text = ecs::saveScene(w, engineTypes(), ecs::SaveOptions{reflect::JsonLayout::Compact});
        std::ofstream(dir / "bench10k.scene.json", std::ios::binary | std::ios::trunc) << text.value();
    }
    ecs::World world;
    api::Session session(world, engineTypes());
    session.setHostKind(api::HostKind::Headless);
    const std::string sceneParams = R"({"path":"build/serve_bench/bench10k.scene.json"})";
    if (session.call("scene.load", R"({"path":")" + (dir / "bench10k.scene.json").generic_string() + "\"}")
            .rfind("{\"result\"", 0) != 0) {
        std::fprintf(stderr, "serve_bench: bench10k did not load in-process\n");
        return 1;
    }
    std::vector<std::pair<uint64_t, uint64_t>> order;   // (creation seq, id)
    for (auto [e, id, co] : world.registry().view<const ecs::EntityId, const ecs::CreationOrder>().each())
        order.emplace_back(co.seq, world.idOf(e));
    std::sort(order.begin(), order.end());
    std::string spin = R"({"commands":[)";
    for (int i = 0; i < 100; ++i)
        spin += std::string(i ? "," : "") + R"({"op":"component.set","id":")" + hex16(order[i].second) +
                R"(","type":"Spin","value":{"degreesPerSecond":45}})";
    spin += "]}";
    const auto applyParams = [&](float x) {
        std::string s = R"({"commands":[)";
        char v[32];
        std::snprintf(v, sizeof v, "%g", x);
        for (int i = 0; i < 100; ++i)
            s += std::string(i ? "," : "") + R"({"op":"field.set","id":")" + hex16(order[100 + i].second) +
                 R"(","path":"/Transform/position","value":[)" + v + ",0,0]}";
        return s + "]}";
    };
    const std::string queryParams = R"({"ids":[")" + hex16(order[500].second) + R"("],"fields":["/Transform/position"]})";
    if (session.call("world.apply", spin).rfind("{\"result\"", 0) != 0) {
        std::fprintf(stderr, "serve_bench: the spinners did not apply in-process\n");
        return 1;
    }

    // The server.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path ohaoExe = fs::path(self).parent_path() / "ohao.exe";
    Child server;
    const auto t0 = Clock::now();
    if (!server.start(ohaoExe.wstring(), L"serve --out build/serve_bench/out --log-level warn", root,
                      dir / "server.stderr.txt")) {
        std::fprintf(stderr, "serve_bench: cannot start %s\n", ohaoExe.string().c_str());
        return 2;
    }
    const std::string init =
        server.call(request(1, "initialize", R"({"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"serve_bench"}})"));
    const double startMs = ms(Clock::now() - t0);
    server.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    const std::string tools = server.call(request(2, "tools/list", "{}"));
    const auto t1 = Clock::now();
    const std::string loaded = server.call(request(3, "scene.load", sceneParams));
    const double loadMs = ms(Clock::now() - t1);
    const std::string spun = server.call(request(4, "world.apply", spin));
    int failures = 0;
    std::vector<std::string> failureText;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) {
            ++failures;
            failureText.push_back(what);
            std::printf("    WRONG: %s\n", what.c_str());
        }
    };
    expect(init.find("\"protocolVersion\":\"2025-11-25\"") != std::string::npos, "initialize answers");
    expect(loaded.find("\"entities\":10002") != std::string::npos, "scene.load of bench10k over stdio");
    expect(spun.find("\"applied\":100") != std::string::npos, "the spinners apply over stdio");
    // The tools/list result as sent (compact JSON), and the instructions string.
    std::size_t toolsBytes = 0, instrBytes = 0;
    if (auto doc = ecs::parseJson(tools))
        if (const auto* result = doc.value().member(doc.value().root(), "result")) {
            reflect::JsonWriter w(1 << 15);
            reflect::writeJson(w, doc.value(), *result);
            toolsBytes = w.str().size();
        }
    if (auto doc = ecs::parseJson(init))
        if (const auto* result = doc.value().member(doc.value().root(), "result"))
            if (const auto* text = doc.value().member(*result, "instructions")) instrBytes = text->text.size();
    expect(toolsBytes > 0, "tools/list answers");

    // ---- the rounds --------------------------------------------------------------------------
    const std::string infoReq = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"world.info\"}\n";
    const std::string applyA = applyParams(1.0f), applyB = applyParams(2.0f);
    const std::string queryReq = request(8, "world.query", queryParams);
    const std::string applyReqA = request(9, "world.apply", applyA), applyReqB = request(9, "world.apply", applyB);
    const std::string stepReq = request(10, "world.step", R"({"frames":1})");
    std::string last;
    std::vector<Op> ops;
    const auto each = [](auto fn) {
        return [fn](int first, int n) mutable {
            for (int i = first; i < first + n; ++i) fn(i);
        };
    };
    ops.push_back({"world_info_rpc", "ms", 20, each([&](int) { last = server.call(infoReq); }), {}, {}, true});
    ops.push_back({"world_info_inproc", "ms", 20, each([&](int) { last = session.call("world.info"); })});
    ops.push_back({"world_query_one_rpc", "ms", 20, each([&](int) { last = server.call(queryReq); }), {}, {}, true});
    ops.push_back({"world_apply_100_rpc", "ms", 10,
                   each([&](int k) { last = server.call(k % 2 ? applyReqA : applyReqB); }), {}, {}, true});
    ops.push_back({"world_step_1_rpc", "ms", 10, each([&](int) { last = server.call(stepReq); }), {}, {}, true});
    ops.push_back({"log_below_threshold", "ns", 1'000'000, [](int first, int n) {
                       for (int i = first; i < first + n; ++i) OHAO_LOG(Debug, render, "below the threshold {} {}", i, 0.5);
                   }});
    ops.push_back({"log_ring_80", "ns", 20'000, [](int first, int n) {
                       for (int i = first; i < first + n; ++i)
                           OHAO_LOG(Info, render,
                                    "deferred: pass shadow_csm cascade 2 draws 1834 tris 572910 gpu ms {:5.3f} id {:05}",
                                    0.5 + (i % 100) * 0.001, i % 100000);
                   }});

    bench::LoadMonitor monitor;
    monitor.exclude(server.pid());
    monitor.start();
    const double cal0 = bench::calibrationMs();
    int calls = 0;
    for (int r = 0; r < 5 + rounds; ++r) {
        for (Op& op : ops) {
            const uint64_t f0 = op.serverFaults ? server.pageFaults() : 0;
            const auto s = Clock::now();
            op.run(calls, op.calls);
            calls += op.calls;
            const double total = std::chrono::duration<double, std::nano>(Clock::now() - s).count();
            if (r < 5) continue;
            op.samples.push_back((op.unit == "ms" ? total / 1e6 : total) / op.calls);
            if (op.serverFaults) op.faults.push_back(static_cast<double>(server.pageFaults() - f0) / op.calls);
        }
    }
    const double cal1 = bench::calibrationMs();
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);

    // Verify after timing: a benchmark of broken code fails.
    expect(server.call(infoReq).find("\"entities\":10002") != std::string::npos, "world.info answers bench10k");
    expect(server.call(queryReq).find("\"count\":1") != std::string::npos, "world.query finds the one entity");
    expect(server.call(applyReqA).find("\"applied\":100") != std::string::npos, "world.apply applies 100");
    expect(server.call(stepReq).find("\"frame\":") != std::string::npos, "world.step steps");
    const int exitCode = server.close();
    expect(exitCode == 0, "the server exits 0 when stdin closes (exit " + std::to_string(exitCode) + ")");

    // Transport overhead: per round, the round trip minus the in-process call beside it.
    std::vector<double> overhead;
    for (std::size_t i = 0; i < ops[0].samples.size(); ++i) overhead.push_back(ops[0].samples[i] - ops[1].samples[i]);

    // ---- the verdict ---------------------------------------------------------------------------
    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    const bool noisyCal = ref > 0 && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisy = noisyCal || noisyLoad;

    struct Gate {
        std::string name, unit, key;
        Stats s;
        std::vector<double> faults;
    };
    std::vector<Gate> gates = {
        {"world_info_rpc", "ms", "world_info_rpc_ms", stats(ops[0].samples), ops[0].faults},
        {"transport_overhead", "ms", "transport_overhead_ms", stats(overhead), {}},
        {"world_query_one_rpc", "ms", "world_query_one_rpc_ms", stats(ops[2].samples), ops[2].faults},
        {"world_apply_100_rpc", "ms", "world_apply_100_rpc_ms", stats(ops[3].samples), ops[3].faults},
        {"world_step_1_rpc", "ms", "world_step_1_rpc_ms", stats(ops[4].samples), ops[4].faults},
        {"log_below_threshold", "ns", "log_below_threshold_ns", stats(ops[5].samples), {}},
        {"log_ring_80", "ns", "log_ring_80_ns", stats(ops[6].samples), {}},
        {"tools_list_bytes", "B", "tools_list_bytes", {static_cast<double>(toolsBytes), static_cast<double>(toolsBytes)}, {}},
    };

    std::printf("serve_bench (sub-project 4, step 6): ohao serve as a child process, bench10k, %d rounds\n", rounds);
    char refText[96] = "none (run serve_bench --calibrate on a quiet machine)";
    if (ref > 0) std::snprintf(refText, sizeof refText, "%.3f ms (x%.2f)", ref, slow);
    std::printf("    calibration  before %.3f ms  after %.3f ms  reference %s\n", cal0, cal1, refText);
    std::printf("    other processes  %.1f%% of a core in total%s\n", external, busy.empty() ? "" : ":");
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);
    std::printf("    in-process world.info median %.4f ms (the overhead's baseline)\n", stats(ops[1].samples).median);

    int passed = 0, failed = 0;
    std::vector<std::string> failureList = failureText;
    reflect::JsonWriter json(4096);
    json.beginObject();
    json.key("bench"); json.string("serve_bench");
    json.key("ops");
    json.beginArray();
    for (const Gate& g : gates) {
        const double budget = jsonNumber(budgets, g.key, 0.0);
        const bool gated = budget > 0;
        const bool pass = !gated || g.s.median <= budget;
        if (gated && !noisy) {
            if (pass) ++passed;
            else {
                ++failed;
                failureList.push_back(g.name + ": median " + std::to_string(g.s.median) + " " + g.unit + " over " +
                                      std::to_string(budget) + " " + g.unit);
            }
        }
        char budgetText[32] = "-";
        if (gated) std::snprintf(budgetText, sizeof budgetText, "%g %s", budget, g.unit.c_str());
        char faultText[48] = "";
        if (!g.faults.empty()) std::snprintf(faultText, sizeof faultText, "   server faults/call %.2f", stats(g.faults).median);
        if (g.unit == "B")
            std::printf("    %-22s %8.0f B                         budget %-10s %s\n", g.name.c_str(), g.s.median,
                        budgetText, noisy ? "noisy" : (pass ? "ok" : "OVER"));
        else
            std::printf("    %-22s median %9.4f %s  p95 %9.4f %s  budget %-10s %s%s\n", g.name.c_str(), g.s.median,
                        g.unit.c_str(), g.s.p95, g.unit.c_str(), budgetText, noisy ? "noisy" : (pass ? "ok" : "OVER"),
                        faultText);
        json.beginObject();
        json.key("name"); json.string(g.name);
        json.key("unit"); json.string(g.unit);
        json.key("median"); json.float32(static_cast<float>(g.s.median));
        json.key("p95"); json.float32(static_cast<float>(g.s.p95));
        json.key("budget"); json.float32(static_cast<float>(budget));
        if (!g.faults.empty()) { json.key("serverFaultsPerCall"); json.float32(static_cast<float>(stats(g.faults).median)); }
        json.endObject();
    }
    json.endArray();
    std::printf("    recorded: process start to initialize %.1f ms; scene.load bench10k over stdio %.1f ms; "
                "initialize reply %zu B (instructions %zu B)\n",
                startMs, loadMs, init.size(), instrBytes);
    failed += failures;
    const char* verdict = noisy ? "noisy" : (failed ? "over" : "pass");
    json.key("verdict"); json.string(verdict);
    json.key("recorded");
    json.beginObject();
    json.key("startToInitializeMs"); json.float32(static_cast<float>(startMs));
    json.key("sceneLoadMs"); json.float32(static_cast<float>(loadMs));
    json.key("initializeBytes"); json.uint64(init.size());
    json.key("instructionsBytes"); json.uint64(instrBytes);
    json.key("inprocWorldInfoMs"); json.float32(static_cast<float>(stats(ops[1].samples).median));
    json.endObject();
    json.key("calibration");
    json.beginObject();
    json.key("beforeMs"); json.float32(static_cast<float>(cal0));
    json.key("afterMs"); json.float32(static_cast<float>(cal1));
    json.key("referenceMs"); json.float32(static_cast<float>(ref));
    json.endObject();
    json.key("externalLoad");
    json.beginObject();
    json.key("totalCorePct"); json.float32(static_cast<float>(external));
    json.key("busy");
    json.beginArray();
    for (const auto& p : busy) {
        json.beginObject();
        json.key("name"); json.string(p.name);
        json.key("corePct"); json.float32(static_cast<float>(p.corePct));
        json.endObject();
    }
    json.endArray();
    json.endObject();
    json.endObject();
    if (std::ofstream f(outPath, std::ios::binary | std::ios::trunc); f) f << json.str() << "\n";

    if (noisy) {
        std::printf("    NOISY: %s%s; the numbers above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%" : "",
                    noisyLoad ? (noisyCal ? ", and other processes were busy" : "other processes were busy") : "");
        failureList.push_back("noisy run (see the calibration and other processes above)");
    }
    // The shared summary line; "noisy" lets tools/check.py rerun a noisy run.
    std::string line = "{\"suite\":\"serve_bench\",\"passed\":" + std::to_string(passed) +
                       ",\"failed\":" + std::to_string(failed + (noisy ? 1 : 0)) + ",\"failures\":[";
    for (std::size_t i = 0; i < failureList.size(); ++i)
        line += (i ? "," : "") + test::jsonString(failureList[i]);
    line += "]";
    if (noisy) line += ",\"noisy\":true";
    line += "}";
    std::printf("Results: %d/%d passed\n%s\n", passed, passed + failed, line.c_str());
    if (failures) return 1;
    return noisy ? 2 : (failed ? 1 : 0);
#endif
}
