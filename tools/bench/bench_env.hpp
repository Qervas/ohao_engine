#pragma once
// What a benchmark run needs to know about the machine it ran on (sub-project 4 spec,
// "Measurement rules"): a calibration loop timed before and after the gated blocks, and the
// other processes that used CPU while it ran. A run that is >25% slower than the recorded
// calibration, or that shared the machine with other busy processes, is `noisy`: its
// numbers say nothing about the code and it neither passes nor fails a budget.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace ohao::bench {

// A fixed integer workload (xorshift, no memory traffic); best of three, in ms.
inline double calibrationMs() {
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t x = 88172645463325252ull;
        for (int i = 0; i < 20'000'000; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
        }
        const auto t1 = std::chrono::steady_clock::now();
        volatile uint64_t sink = x;
        (void)sink;
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

// Short probes taken once per round, beside the measured operations, so a slow round can
// be told from a slow machine: the CPU (a fixed integer loop, ~2 ms, no memory traffic) and
// the memory system (a read of every cache line of 64 MB, larger than any L3 in use: what
// another process's memory traffic or a cache-hungry neighbour slows down).
inline double cpuProbeMs() {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 2'000'000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    const auto t1 = std::chrono::steady_clock::now();
    volatile uint64_t sink = x;
    (void)sink;
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

inline double memoryProbeMs() {
    static std::vector<uint64_t> buffer((std::size_t{64} << 20) / sizeof(uint64_t), 1);   // touched once
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (std::size_t i = 0; i < buffer.size(); i += 8) sum += buffer[i];   // one load per 64-byte line
    const auto t1 = std::chrono::steady_clock::now();
    volatile uint64_t sink = sum;
    (void)sink;
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// The reference calibration written by `--calibrate`, or 0 when there is none.
inline double readReference(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0.0;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    const auto at = s.find("\"calibrationMs\":");
    if (at == std::string::npos) return 0.0;
    return std::atof(s.c_str() + at + 16);
}

inline bool writeReference(const std::string& path, double ms) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "{\"calibrationMs\":" << ms << "}\n";
    return static_cast<bool>(f);
}

struct BusyProcess {
    std::string name;
    double corePct = 0.0;   // CPU time over wall time, in percent of one core
};

// CPU time used by every other process between start() and stop(). Processes the bench
// started itself (a server it drives) are passed to exclude().
class LoadMonitor {
public:
    void exclude(uint32_t pid) { excluded_.push_back(pid); }
    void start() {
        t0_ = std::chrono::steady_clock::now();
        before_ = sample();
    }
    // Processes that used at least `minPct` of a core, busiest first; `total` gets the sum
    // over all other processes.
    std::vector<BusyProcess> stop(double minPct, double* total) {
        const double wall100ns =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count() * 1e7;
        const auto after = sample();
        std::vector<BusyProcess> busy;
        double sum = 0.0;
        for (const auto& a : after)
            for (const auto& b : before_)
                if (a.pid == b.pid && a.name == b.name && a.cpu100ns >= b.cpu100ns &&
                    std::find(excluded_.begin(), excluded_.end(), a.pid) == excluded_.end()) {
                    const double pct = wall100ns > 0 ? 100.0 * double(a.cpu100ns - b.cpu100ns) / wall100ns : 0.0;
                    sum += pct;
                    if (pct >= minPct) busy.push_back({a.name, pct});
                    break;
                }
        std::sort(busy.begin(), busy.end(), [](auto& x, auto& y) { return x.corePct > y.corePct; });
        if (total) *total = sum;
        return busy;
    }

private:
    struct Sample {
        uint32_t pid;
        std::string name;
        uint64_t cpu100ns;
    };
    static std::vector<Sample> sample() {
        std::vector<Sample> out;
#ifdef _WIN32
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        const DWORD self = GetCurrentProcessId();
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == self) continue;   // Idle, us
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            FILETIME c, e, k, u;
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                const uint64_t kt = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime;
                const uint64_t ut = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
                char name[MAX_PATH * 3] = {};
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof name, nullptr, nullptr);
                out.push_back({pe.th32ProcessID, name, kt + ut});
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
#endif
        return out;
    }

    std::chrono::steady_clock::time_point t0_{};
    std::vector<Sample> before_;
    std::vector<uint32_t> excluded_;
};

} // namespace ohao::bench
