// job_bench: what the job pool costs (docs/plans/2026-11-18-step4b-jobs.md, 4b-0 and 4b-1). CPU, headless; a tool
// run by hand, not a gate.
//
//   job_bench [--threads N] [--rounds R] [--out build/job_bench.json]
//   job_bench --stress N [--threads N]
//
// The measures, each R alternated rounds (3) after a warm-up, each round's median kept; the spread is the rounds'
// lowest to highest median:
//   spawn_to_run_us   an empty task pushed by the game thread to its first instruction on a worker that spins
//   steal_ns          per task, 4,001 empty tasks pushed by the game thread and drained from its deque by the
//                     workers (each a steal; contended when several workers spin)
//   wake_us           the same push with every worker asleep, to the task's first instruction (the futex wake)
//   loop_us           1,000 chunks of about 1 us of integer work, a plain loop on the game thread
//   parallel_for_us   the same 1,000 chunks through jobs::parallelFor (grain 1)
//   parallel_for_empty_us   1,000 empty chunks through parallelFor: its cost alone
// The game thread does not help in the first three (it spins on the task's flag), so a lost wake stalls them:
// every such wait gives up after 2 s, logs what it waited on, and the run fails.
//
// --stress N: N indices pass through one Deque, pushed and taken by the game thread in bursts of 1 to 8 while every
// worker steals; then N / 10 through parallelFor (grain 1). Each index must run exactly once: the count of those
// run twice and of those never run is printed, and either fails the run.
#include "common/harness.hpp"
#include "core/jobs/graph.hpp"
#include "core/jobs/pool.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace ohao;
using Clock = std::chrono::steady_clock;

namespace {

double us(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double p95(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(0.95 * double(v.size())))];
}

// Spins until `flag` is set, without helping. After 2 s: logs what it waited on and the pool's state, and exits 1.
void waitFlag(const std::atomic<int64_t>& flag, const char* measure, const char* task) {
    const auto until = Clock::now() + std::chrono::seconds(2);
    while (flag.load(std::memory_order_acquire) == 0) {
        if (Clock::now() > until) {
            const jobs::Stats s = jobs::stats();
            OHAO_LOG(Error, runtime,
                     "job_bench: {} waited 2 s on task '{}', which never ran; {} of {} workers asleep; {} tasks run, {} wakes "
                     "sent, {} sleeps",
                     measure, task, jobs::sleeping(), jobs::threadCount() - 1, s.run, s.wakes, s.sleeps);
            std::printf("{\"suite\":\"job_bench\",\"passed\":0,\"failed\":1,\"failures\":[%s]}\n",
                        test::jsonString(std::string(measure) + ": a wait stalled 2 s on " + task).c_str());
            std::fflush(stdout);
            std::_Exit(1);
        }
    }
}

// Until every worker sleeps (they spin first); false after 2 s.
bool allAsleep() {
    const int workers = jobs::threadCount() - 1;
    const auto until = Clock::now() + std::chrono::seconds(2);
    while (jobs::sleeping() < workers)
        if (Clock::now() > until) return false;
    return true;
}

// One push to a worker: microseconds from before the push to the task's first instruction.
double pushToRun(const char* measure) {
    std::atomic<int64_t> ran{0};
    const auto t0 = Clock::now();
    jobs::Task* t = jobs::makeTask("job_bench.empty", jobs::Priority::Frame,
                                   [&ran] { ran.store(Clock::now().time_since_epoch().count(), std::memory_order_release); });
    jobs::submit(t);
    waitFlag(ran, measure, "job_bench.empty");
    return us(t0, Clock::time_point(Clock::duration(ran.load())));
}

// A chunk's work: a dependent integer chain, out of line so the plain loop cannot vectorize across chunks.
[[gnu::noinline]] uint64_t work(uint64_t x, int n) {
    for (int i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    return x;
}

constexpr int kChunkIterations = 1500;   // about 1 us on a P-core

struct Round {
    double spawn{0}, steal{0}, wake{0}, loop{0}, pfor{0}, pforEmpty{0};
    double spawnP95{0}, wakeP95{0};
};

Round measure() {
    Round r;
    const bool workers = jobs::threadCount() > 1;
    if (workers) {
        std::vector<double> spawn;
        for (int i = 0; i < 2000; ++i) spawn.push_back(pushToRun("spawn_to_run"));
        r.spawn = median(spawn);
        r.spawnP95 = p95(spawn);

        // The workers drain 4,001 tasks from the game thread's deque; the game thread spins meanwhile.
        std::vector<double> steal;
        for (int rep = 0; rep < 20; ++rep) {
            static std::atomic<int> runs{0};
            runs = 0;
            std::vector<jobs::Task*> tasks;
            for (int i = 0; i < 4000; ++i)
                tasks.push_back(jobs::makeTask("job_bench.steal", jobs::Priority::Frame, [] { runs.fetch_add(1, std::memory_order_relaxed); }));
            std::atomic<int64_t> done{0};
            const auto t0 = Clock::now();
            for (jobs::Task* t : tasks) jobs::submit(t);
            jobs::Task* last = jobs::makeTask("job_bench.steal.last", jobs::Priority::Frame,
                                              [&done] { done.store(1, std::memory_order_release); });
            jobs::submit(last);
            waitFlag(done, "steal", "job_bench.steal.last");
            while (runs.load() < 4000) {}
            steal.push_back(us(t0, Clock::now()) * 1000.0 / 4001.0);
        }
        r.steal = median(steal);

        std::vector<double> wake;
        for (int i = 0; i < 2000; ++i) {
            if (!allAsleep()) {
                OHAO_LOG(Error, runtime, "job_bench: the workers did not all sleep within 2 s ({} of {})", jobs::sleeping(),
                         jobs::threadCount() - 1);
                std::_Exit(1);
            }
            // Asleep in the kernel, not about to be: a sleeper counts itself before its last look for work.
            const auto settle = Clock::now() + std::chrono::microseconds(300);
            while (Clock::now() < settle) {}
            wake.push_back(pushToRun("wake"));
        }
        r.wake = median(wake);
        r.wakeP95 = p95(wake);
    }
    std::vector<uint64_t> out(1000);
    std::vector<double> loop, pfor, empty;
    for (int rep = 0; rep < 200; ++rep) {
        auto t0 = Clock::now();
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = work(i + 1, kChunkIterations);
        loop.push_back(us(t0, Clock::now()));
        t0 = Clock::now();
        jobs::parallelFor(0, out.size(), 1, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) out[i] = work(i + 1, kChunkIterations);
        });
        pfor.push_back(us(t0, Clock::now()));
        t0 = Clock::now();
        jobs::parallelFor(0, out.size(), 1, [](std::size_t, std::size_t) {});
        empty.push_back(us(t0, Clock::now()));
    }
    volatile uint64_t sink = out[999];
    (void)sink;
    r.loop = median(loop);
    r.pfor = median(pfor);
    r.pforEmpty = median(empty);
    return r;
}

// The deque stress: `n` indices through one Deque owned by the game thread, every worker stealing.
std::pair<uint64_t, uint64_t> stressDeque(uint64_t n) {
    std::vector<std::atomic<uint8_t>> runs(n);
    auto deque = std::make_unique<jobs::Deque>();
    std::atomic<bool> finished{false};
    std::atomic<uint64_t> stolen{0};
    const auto ran = [&](jobs::Task* t) {
        runs[reinterpret_cast<uintptr_t>(t) - 1].fetch_add(1, std::memory_order_relaxed);
    };
    const int thieves = jobs::threadCount() - 1;
    jobs::Counter thieving;
    for (int i = 0; i < thieves; ++i) {
        thieving.value.fetch_add(1);
        jobs::Task* t = jobs::makeTask("job_bench.thief", jobs::Priority::Frame, [&] {
            uint64_t mine = 0;
            while (!finished.load(std::memory_order_acquire))
                if (jobs::Task* got = deque->steal()) {
                    ran(got);
                    ++mine;
                }
            stolen.fetch_add(mine);
        });
        t->counter = &thieving;
        jobs::submit(t);
    }
    uint32_t rng = 12345;
    for (uint64_t next = 0; next < n;) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const uint64_t burst = std::min<uint64_t>(1 + rng % 8, n - next);
        for (uint64_t k = 0; k < burst; ++k) {
            auto* fake = reinterpret_cast<jobs::Task*>(static_cast<uintptr_t>(next + 1));
            if (!deque->push(fake)) ran(fake);
            ++next;
        }
        while (jobs::Task* got = deque->take()) ran(got);
    }
    while (deque->sizeApprox() > 0)
        if (jobs::Task* got = deque->take()) ran(got);
    finished.store(true, std::memory_order_release);
    jobs::wait(thieving);
    uint64_t twice = 0, never = 0;
    for (auto& c : runs) {
        const uint8_t v = c.load(std::memory_order_relaxed);
        twice += v > 1;
        never += v == 0;
    }
    std::printf("  deque: %llu indices, %llu stolen by %d workers; run twice %llu, never %llu\n",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(stolen.load()), thieves,
                static_cast<unsigned long long>(twice), static_cast<unsigned long long>(never));
    return {twice, never};
}

std::pair<uint64_t, uint64_t> stressPool(uint64_t n) {
    std::vector<std::atomic<uint8_t>> runs(n);
    jobs::resetStats();
    jobs::parallelFor(0, n, 1, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) runs[i].fetch_add(1, std::memory_order_relaxed);
    });
    uint64_t twice = 0, never = 0;
    for (auto& c : runs) {
        const uint8_t v = c.load(std::memory_order_relaxed);
        twice += v > 1;
        never += v == 0;
    }
    const jobs::Stats s = jobs::stats();
    std::printf("  parallelFor: %llu chunks, %llu tasks run, %llu stolen, %llu overflowed; run twice %llu, never %llu\n",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(s.run),
                static_cast<unsigned long long>(s.stolen), static_cast<unsigned long long>(s.overflowed),
                static_cast<unsigned long long>(twice), static_cast<unsigned long long>(never));
    return {twice, never};
}

} // namespace

int main(int argc, char** argv) {
    int threads = 0, rounds = 3;
    uint64_t stress = 0;
    std::string outPath = "build/job_bench.json";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--rounds" && i + 1 < argc) rounds = std::max(1, std::atoi(argv[++i]));
        else if (a == "--stress" && i + 1 < argc) stress = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: job_bench [--threads N] [--rounds R] [--out file] | --stress N [--threads N]\n");
            return 2;
        }
    }
    jobs::configure(jobs::configFromEnvironment(threads));
    std::printf("job_bench: %s\n", jobs::describe().c_str());

    if (stress) {
        const auto t0 = Clock::now();
        const auto [dt, dn] = stressDeque(stress);
        const auto [pt, pn] = stressPool(std::max<uint64_t>(1, stress / 10));
        const bool ok = dt + dn + pt + pn == 0;
        std::printf("  %.1f s\n", us(t0, Clock::now()) / 1e6);
        std::printf("{\"suite\":\"job_bench.stress\",\"passed\":%d,\"failed\":%d,\"failures\":[%s]}\n", ok ? 1 : 0, ok ? 0 : 1,
                    ok ? "" : "\"an index ran twice or never\"");
        return ok ? 0 : 1;
    }

    (void)measure();   // the warm-up
    std::vector<Round> rs;
    for (int r = 0; r < rounds; ++r) rs.push_back(measure());
    const auto row = [&](const char* name, double Round::*m, double Round::*tail, const char* unit, nlohmann::json& out) {
        std::vector<double> v;
        for (const Round& r : rs) v.push_back(r.*m);
        const double lo = *std::min_element(v.begin(), v.end()), hi = *std::max_element(v.begin(), v.end());
        std::printf("  %-22s median %9.3f %s   rounds %.3f to %.3f", name, median(v), unit, lo, hi);
        nlohmann::json j = {{"median", median(v)}, {"lo", lo}, {"hi", hi}, {"unit", unit}};
        if (tail) {
            std::vector<double> t;
            for (const Round& r : rs) t.push_back(r.*tail);
            std::printf("   p95 %.3f", median(t));
            j["p95"] = median(t);
        }
        std::printf("\n");
        out[name] = j;
    };
    nlohmann::json out;
    out["pool"] = jobs::describe();
    out["threads"] = jobs::threadCount();
    if (jobs::threadCount() > 1) {
        row("spawn_to_run_us", &Round::spawn, &Round::spawnP95, "us", out);
        row("steal_ns", &Round::steal, nullptr, "ns", out);
        row("wake_us", &Round::wake, &Round::wakeP95, "us", out);
    }
    row("loop_us", &Round::loop, nullptr, "us", out);
    row("parallel_for_us", &Round::pfor, nullptr, "us", out);
    row("parallel_for_empty_us", &Round::pforEmpty, nullptr, "us", out);
    const jobs::Stats s = jobs::stats();
    std::printf("  pool: %llu tasks run, %llu stolen, %llu overflowed, %llu wakes, %llu sleeps, %llu task blocks\n",
                static_cast<unsigned long long>(s.run), static_cast<unsigned long long>(s.stolen),
                static_cast<unsigned long long>(s.overflowed), static_cast<unsigned long long>(s.wakes),
                static_cast<unsigned long long>(s.sleeps), static_cast<unsigned long long>(s.slabBlocks));
    out["stats"] = {{"run", s.run}, {"stolen", s.stolen}, {"overflowed", s.overflowed}, {"wakes", s.wakes},
                    {"sleeps", s.sleeps}, {"slabBlocks", s.slabBlocks}};
    if (const auto dir = std::filesystem::path(outPath).parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    std::ofstream(outPath, std::ios::binary) << out.dump(2) << '\n';
    std::printf("{\"suite\":\"job_bench\",\"passed\":1,\"failed\":0,\"failures\":[]}\n");
    return 0;
}
