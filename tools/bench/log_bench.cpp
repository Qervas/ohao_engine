// log_bench: the logger's budgets (sub-project 4 spec, "Performance / Budgets", step 4).
//
//   log_bench [--check tests/bench/serve_budgets.json] [--out build/log_bench.json]
//             [--reference build/serve_bench_reference.json] [--calibrate]
//
//   log_below_threshold   OHAO_LOG below the capture threshold, ns per call    (<= 5)
//   log_ring_80           OHAO_LOG into the ring, an 80-character message       (<= 300)
//
// Medians of 50 blocks after 5 warm-ups, p95 reported, high priority, no sinks (the ring
// only; the stderr sink's cost is the console's). Each run times a fixed CPU loop before
// and after the blocks and records every other process that used CPU meanwhile: a run more
// than 25% slower than the reference calibration (written by --calibrate on a quiet
// machine), or one that shared the machine with busy processes, is `noisy` (exit 2), not a
// pass or a failure. Exit 1 when a median is over its budget.
#include "core/log.hpp"
#include "common/harness.hpp"
#include "bench_env.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ohao;

namespace {

struct Stats {
    double median = 0, p95 = 0;
};

Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return {v[v.size() / 2], v[std::min(v.size() - 1, (v.size() * 95) / 100)]};
}

// ns per call over `blocks` blocks of `calls` calls, after `warm` warm-up blocks.
template <class F>
std::vector<double> measure(int warm, int blocks, int calls, F&& body) {
    std::vector<double> out;
    for (int b = 0; b < warm + blocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; ++i) body(b * calls + i);
        const auto t1 = std::chrono::steady_clock::now();
        if (b >= warm) out.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / calls);
    }
    return out;
}

double jsonNumber(const std::string& text, const std::string& key, double fallback) {
    const auto at = text.find("\"" + key + "\"");
    if (at == std::string::npos) return fallback;
    const auto colon = text.find(':', at);
    return colon == std::string::npos ? fallback : std::atof(text.c_str() + colon + 1);
}

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    std::string checkPath, outPath = "build/log_bench.json", refPath = "build/serve_bench_reference.json";
    bool calibrate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check" && i + 1 < argc) checkPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--reference" && i + 1 < argc) refPath = argv[++i];
        else if (a == "--calibrate") calibrate = true;
        else {
            std::fprintf(stderr, "usage: log_bench [--check budgets.json] [--out file] [--reference file] [--calibrate]\n");
            return 2;
        }
    }
    if (calibrate) {
        const double ms = bench::calibrationMs();
        if (!bench::writeReference(refPath, ms)) {
            std::fprintf(stderr, "log_bench: cannot write %s\n", refPath.c_str());
            return 2;
        }
        std::printf("calibration %.3f ms written to %s\n", ms, refPath.c_str());
        return 0;
    }

    double budgetBelow = 0, budgetRing = 0, maxSlowdown = 1.25, maxExternal = 50.0;
    if (!checkPath.empty()) {
        const std::string b = readText(checkPath);
        if (b.empty()) {
            std::fprintf(stderr, "log_bench: cannot read %s\n", checkPath.c_str());
            return 2;
        }
        budgetBelow = jsonNumber(b, "log_below_threshold", 0);
        budgetRing = jsonNumber(b, "log_ring_80", 0);
        maxSlowdown = jsonNumber(b, "maxCalibrationSlowdown", maxSlowdown);
        maxExternal = jsonNumber(b, "maxExternalCorePct", maxExternal);
    }

    log::removeSink(log::stderrSink().get());
    log::setLevel(log::Level::Info);
    log::clear();

    bench::LoadMonitor monitor;
    monitor.start();
    const double cal0 = bench::calibrationMs();

    const auto below = measure(5, 50, 1'000'000, [](int i) {
        OHAO_LOG(Debug, render, "below the threshold {} {}", i, 0.5);
    });
    const auto ring = measure(5, 50, 20'000, [](int i) {
        OHAO_LOG(Info, render, "deferred: pass shadow_csm cascade 2 draws 1834 tris 572910 gpu ms {:5.3f} id {:05}",
                 0.5 + (i % 100) * 0.001, i % 100000);
    });

    const double cal1 = bench::calibrationMs();
    double external = 0.0;
    const auto busy = monitor.stop(5.0, &external);

    const auto last = log::read(log::stats().newest - 1, log::Level::Info, std::nullopt, 1);
    const std::size_t msgLen = last.lines.empty() ? 0 : last.lines[0].msg.size();
    log::clear();
    log::addSink(log::stderrSink());

    const Stats sb = stats(below), sr = stats(ring);
    const double ref = bench::readReference(refPath);
    const double slow = ref > 0 ? std::max(cal0, cal1) / ref : 0.0;
    const bool calibrated = ref > 0;
    const bool noisyCal = calibrated && slow > maxSlowdown;
    const bool noisyLoad = external > maxExternal;
    const bool noisy = noisyCal || noisyLoad;

    std::printf("log_bench (sub-project 4, step 4)\n");
    char refText[96] = "none (run --calibrate on a quiet machine)";
    if (calibrated) std::snprintf(refText, sizeof refText, "%.3f ms (x%.2f)", ref, slow);
    std::printf("    calibration  before %.3f ms  after %.3f ms  reference %s\n", cal0, cal1, refText);
    std::printf("    other processes  %.1f%% of a core in total%s\n", external, busy.empty() ? "" : ":");
    for (const auto& p : busy) std::printf("        %-40s %6.1f%%\n", p.name.c_str(), p.corePct);

    int passed = 0, failed = 0;
    std::vector<std::string> failures;
    struct Op {
        const char* name;
        Stats s;
        double budget;
    };
    const Op ops[] = {{"log_below_threshold", sb, budgetBelow}, {"log_ring_80", sr, budgetRing}};
    std::string json = "{\"bench\":\"log_bench\",\"ops\":[";
    for (std::size_t i = 0; i < std::size(ops); ++i) {
        const Op& op = ops[i];
        const bool gated = op.budget > 0;
        const bool pass = !gated || op.s.median <= op.budget;
        if (gated && !noisy) {
            if (pass) ++passed;
            else {
                ++failed;
                failures.push_back(std::string(op.name) + ": median " + std::to_string(op.s.median) + " ns over " +
                                   std::to_string(op.budget) + " ns");
            }
        }
        char budget[32] = "-";
        if (gated) std::snprintf(budget, sizeof budget, "%g ns", op.budget);
        std::printf("    %-22s median %8.2f ns   p95 %8.2f ns   budget %-8s %s\n", op.name, op.s.median, op.s.p95,
                    budget, noisy ? "noisy" : (pass ? "ok" : "OVER"));
        json += std::string(i ? "," : "") + "{\"name\":\"" + op.name + "\",\"medianNs\":" + std::to_string(op.s.median) +
                ",\"p95Ns\":" + std::to_string(op.s.p95) + ",\"budgetNs\":" + std::to_string(op.budget) + "}";
    }
    std::printf("    ring message length %zu (the budget is for 80)\n", msgLen);
    if (msgLen != 80) {
        ++failed;
        failures.push_back("the ring message is " + std::to_string(msgLen) + " characters, not 80");
    }
    const char* verdict = noisy ? "noisy" : (failed ? "over" : "pass");
    json += "],\"verdict\":\"" + std::string(verdict) + "\",\"calibration\":{\"beforeMs\":" + std::to_string(cal0) +
            ",\"afterMs\":" + std::to_string(cal1) + ",\"referenceMs\":" + std::to_string(ref) +
            "},\"externalLoad\":{\"totalCorePct\":" + std::to_string(external) + ",\"busy\":[";
    for (std::size_t i = 0; i < busy.size(); ++i)
        json += std::string(i ? "," : "") + "{\"name\":" + test::jsonString(busy[i].name) +
                ",\"corePct\":" + std::to_string(busy[i].corePct) + "}";
    json += "]}}\n";
    if (std::ofstream f(outPath, std::ios::binary | std::ios::trunc); f) f << json;

    if (noisy) {
        std::printf("    NOISY: %s%s; the numbers above are not a verdict on the code\n",
                    noisyCal ? "calibration over the reference by more than 25%" : "",
                    noisyLoad ? (noisyCal ? ", and other processes were busy" : "other processes were busy") : "");
        failures.push_back("noisy run (see the calibration and other processes above)");
        test::finish("log_bench", passed, failed + 1, failures);
        return 2;
    }
    return test::finish("log_bench", passed, failed, failures);
}
