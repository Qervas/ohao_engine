#include "core/jobs/pool.hpp"

#include "core/jobs/topology.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ohao::jobs {

// A thread's free tasks. Only the owner pops; others return tasks through `returned`, a stack the owner takes
// whole when its own list is empty (so no pop races another, and no ABA). Slabs live as long as the process.
struct Slab {
    Task* free{nullptr};
    std::atomic<Task*> returned{nullptr};
    std::vector<std::unique_ptr<Task[]>> blocks;
};

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kBlockTasks = 256;
constexpr uint32_t kSleeping = 1u << 31;   // Counter::value: a thread sleeps on it

void pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// The futex calls touch nothing but the kernel's hash of the address, so a wake after the waiter has returned
// (and freed the word's memory) is harmless: at most a spurious wake of whoever waits there now, and every wait
// here loops.
void futexWait(std::atomic<uint32_t>& word, uint32_t expected) noexcept {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
#else
    word.wait(expected);
#endif
}

void futexWake(std::atomic<uint32_t>& word, int count) noexcept {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
#else
    if (count == 1) word.notify_one();
    else word.notify_all();
#endif
}

// A thread with deques: the game thread (index 0) or a worker.
struct alignas(64) Thread {
    Deque deques[2];          // by Priority
    bool frame{true};         // runs frame tasks
    bool background{false};   // runs background tasks
    int group{0};             // its sleep group (kP, kE)
    int index{0};
    std::vector<int> cpus;    // its affinity; empty: none
    uint32_t rng{1};
    std::atomic<uint64_t> run{0}, stolen{0}, overflowed{0}, sleeps{0};
    std::thread thread;
};

// The workers of one core class, for sleep and wake.
struct alignas(64) Group {
    std::atomic<uint32_t> word{0};
    std::atomic<uint32_t> sleepers{0};
    std::atomic<uint32_t> spinners{0};
    int members{0};
    bool frame{false};
};
constexpr int kP = 0, kE = 1;

thread_local Thread* tlsThread = nullptr;
thread_local Slab* tlsSlab = nullptr;
thread_local Priority tlsPriority = Priority::Frame;   // the running task's

std::mutex& slabsMutex() {
    static std::mutex m;
    return m;
}
std::atomic<uint64_t> g_slabBlocks{0}, g_injected{0}, g_wakes{0};

Slab& mySlab() {
    if (!tlsSlab) {
        auto* s = new Slab;   // lives as long as the process: tasks from it may still be returning
        std::lock_guard lock(slabsMutex());
        static auto* all = new std::vector<Slab*>;
        all->push_back(s);
        tlsSlab = s;
    }
    return *tlsSlab;
}

void release(Task* t) noexcept {
    if (t->destroy) t->destroy(*t);
    delete t->moreSuccessors;
    t->moreSuccessors = nullptr;
    Slab* s = t->slab;
    if (s == tlsSlab) {
        t->nextFree = s->free;
        s->free = t;
        return;
    }
    Task* head = s->returned.load(std::memory_order_relaxed);
    do {
        t->nextFree = head;
    } while (!s->returned.compare_exchange_weak(head, t, std::memory_order_release, std::memory_order_relaxed));
}

void pinTo(const std::vector<int>& cpus) {
#if defined(__linux__)
    if (cpus.empty()) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int c : cpus) CPU_SET(c, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
        OHAO_LOG(Warn, runtime, "jobs: could not pin a thread to {} CPUs", cpus.size());
#elif defined(_WIN32)
    if (cpus.empty()) return;
    DWORD_PTR mask = 0;   // processor group 0's (core/jobs/topology.hpp)
    for (const int c : cpus) mask |= DWORD_PTR{1} << c;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0)
        OHAO_LOG(Warn, runtime, "jobs: could not pin a thread to {} CPUs", cpus.size());
#else
    (void)cpus;
#endif
}

std::mutex g_configMutex;
Config g_config;
bool g_configured = false;
std::atomic<bool> g_started{false};

class Pool {
public:
    explicit Pool(const Config& config);
    void start();
    void stop();

    void push(Task* t);
    void execute(Thread* me, Task* t);
    void wait(Counter& c);
    [[nodiscard]] int threadCount() const noexcept { return static_cast<int>(m_threads.size()); }
    [[nodiscard]] int frameThreadCount() const noexcept {
        return 1 + m_groups[kP].members + (m_groups[kE].frame ? m_groups[kE].members : 0);
    }
    [[nodiscard]] int sleeping() const noexcept {
        return static_cast<int>(m_groups[kP].sleepers.load() + m_groups[kE].sleepers.load());
    }
    Stats stats() const;
    void resetStats();
    std::string describe() const;
    const Config& config() const noexcept { return m_config; }

private:
    Task* find(Thread* me);
    Task* fromInjected(Priority p);
    void notify(Priority p);
    void wake(Group& g);
    void workerLoop(Thread* me);
    void sleepOn(Counter& c);

    Config m_config;
    std::vector<std::unique_ptr<Thread>> m_threads;   // [0] the game thread
    Group m_groups[2];
    int m_workers{0};
    int m_gameCpu{-1};
    std::mutex m_injectMutex;
    std::deque<Task*> m_injected[2];
    std::atomic<uint32_t> m_injectedCount{0};
    std::atomic<bool> m_stopping{false};
};

Pool* g_pool = nullptr;
std::once_flag g_once;

Pool& pool() {
    std::call_once(g_once, [] {
        Config c;
        {
            std::lock_guard lock(g_configMutex);
            c = g_configured ? g_config : configFromEnvironment();
            g_config = c;
            g_started = true;
        }
        g_pool = new Pool(c);   // stopped and joined at exit, never deleted (a late task may still release to it)
        g_pool->start();
        std::atexit([] { g_pool->stop(); });
    });
    return *g_pool;
}

Pool::Pool(const Config& config) : m_config(config) {
    const Topology topo = readTopology();
    // The game thread's and the render thread's CPUs: the first hardware threads of the first two physical cores.
    std::vector<int> reserved(topo.pCoreFirsts.begin(), topo.pCoreFirsts.begin() + std::min<std::size_t>(2, topo.pCoreFirsts.size()));
    if (topo.pCoreFirsts.size() < 3) reserved.resize(std::min<std::size_t>(1, reserved.size()));   // keep a core for workers
    const auto isReserved = [&](int c) { return std::find(reserved.begin(), reserved.end(), c) != reserved.end(); };
    // P-worker CPUs, in the order workers take them: one thread of each free core, then (smt) their siblings.
    std::vector<int> pOrder;
    for (const int c : topo.pCoreFirsts)
        if (!isReserved(c)) pOrder.push_back(c);
    if (config.smt)
        for (const int c : topo.pCpus)
            if (!isReserved(c) && std::find(topo.pCoreFirsts.begin(), topo.pCoreFirsts.end(), c) == topo.pCoreFirsts.end())
                pOrder.push_back(c);
    std::vector<int> pSet = pOrder, eSet = topo.eCpus;
    std::sort(pSet.begin(), pSet.end());
    const int available = static_cast<int>(pOrder.size() + eSet.size());
    m_workers = config.threads > 0 ? std::min(config.threads - 1, available) : available;
    if (config.threads - 1 > available)
        OHAO_LOG(Warn, runtime, "jobs: {} threads asked, {} available (the game thread and {} workers)", config.threads,
                 available + 1, available);
    if (config.pin != Pin::None && !reserved.empty()) m_gameCpu = reserved.front();

    auto game = std::make_unique<Thread>();
    game->frame = true;
    game->background = false;
    game->index = 0;
    if (m_gameCpu >= 0) game->cpus = {m_gameCpu};
    m_threads.push_back(std::move(game));
    m_groups[kP].frame = true;
    m_groups[kE].frame = config.eFrame;
    for (int i = 0; i < m_workers; ++i) {
        auto w = std::make_unique<Thread>();
        const bool isP = i < static_cast<int>(pOrder.size());
        w->group = isP ? kP : kE;
        w->frame = isP || config.eFrame;
        w->background = true;
        w->index = i + 1;
        w->rng = 0x9e3779b9u * static_cast<uint32_t>(i + 2);
        const int cpu = isP ? pOrder[static_cast<std::size_t>(i)] : eSet[static_cast<std::size_t>(i) - pOrder.size()];
        if (config.pin == Pin::Class) w->cpus = isP ? pSet : eSet;
        else if (config.pin == Pin::Cpu) w->cpus = {cpu};
        ++m_groups[w->group].members;
        m_threads.push_back(std::move(w));
    }
}

void Pool::start() {
    Thread* game = m_threads.front().get();
    tlsThread = game;
    pinTo(game->cpus);
    for (std::size_t i = 1; i < m_threads.size(); ++i) {
        Thread* w = m_threads[i].get();
        w->thread = std::thread([this, w] {
            tlsThread = w;
            pinTo(w->cpus);
#if defined(__linux__)
            char name[16];
            std::snprintf(name, sizeof name, "ohao-%c%d", w->group == kP ? 'p' : 'e', w->index);
            pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
            wchar_t name[16];
            std::swprintf(name, 16, L"ohao-%c%d", w->group == kP ? L'p' : L'e', w->index);
            SetThreadDescription(GetCurrentThread(), name);
#endif
            workerLoop(w);
        });
    }
    OHAO_LOG(Debug, runtime, "jobs: {}", describe());
}

void Pool::stop() {
    m_stopping.store(true, std::memory_order_seq_cst);
    for (Group& g : m_groups) {
        g.word.fetch_add(1, std::memory_order_seq_cst);
        futexWake(g.word, INT_MAX);
    }
    for (std::size_t i = 1; i < m_threads.size(); ++i) {
        Thread* w = m_threads[i].get();
        if (!w->thread.joinable()) continue;
        if (tlsThread == w) w->thread.detach();   // exit() called from a task on this worker
        else w->thread.join();
    }
}

void Pool::push(Task* t) {
    const auto p = static_cast<std::size_t>(t->priority);
    if (t->priority == Priority::Background && m_workers == 0) {   // nothing else would run it
        execute(tlsThread, t);
        return;
    }
    Thread* me = tlsThread;
    if (me) {
        if (!me->deques[p].push(t)) {
            me->overflowed.fetch_add(1, std::memory_order_relaxed);
            execute(me, t);
            return;
        }
    } else {
        std::lock_guard lock(m_injectMutex);
        m_injected[p].push_back(t);
        m_injectedCount.fetch_add(1, std::memory_order_relaxed);
        g_injected.fetch_add(1, std::memory_order_relaxed);
    }
    notify(t->priority);
}

void Pool::wake(Group& g) {
    g.word.fetch_add(1, std::memory_order_seq_cst);
    futexWake(g.word, 1);
    g_wakes.fetch_add(1, std::memory_order_relaxed);
}

void Pool::notify(Priority p) {
    // Pairs with a sleeper's fence between announcing itself and its last look for work: either it sees the task
    // or this sees it.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const int order[2] = {p == Priority::Frame ? kP : kE, p == Priority::Frame ? kE : kP};
    const auto capable = [&](const Group& g) { return g.members > 0 && (p == Priority::Background || g.frame); };
    for (const int i : order)
        if (capable(m_groups[i]) && m_groups[i].spinners.load(std::memory_order_relaxed) > 0) return;
    for (const int i : order)
        if (capable(m_groups[i]) && m_groups[i].sleepers.load(std::memory_order_relaxed) > 0) {
            wake(m_groups[i]);
            return;
        }
}

Task* Pool::fromInjected(Priority p) {
    if (m_injectedCount.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard lock(m_injectMutex);
    auto& q = m_injected[static_cast<std::size_t>(p)];
    if (q.empty()) return nullptr;
    Task* t = q.front();
    q.pop_front();
    m_injectedCount.fetch_sub(1, std::memory_order_relaxed);
    return t;
}

// The next task for `me`: its own deques (newest first), the injection queue, then a steal (oldest first) from
// the others, starting at a random one. Frame before background.
Task* Pool::find(Thread* me) {
    if (me->frame)
        if (Task* t = me->deques[0].take()) return t;
    if (me->background)
        if (Task* t = me->deques[1].take()) return t;
    if (me->frame)
        if (Task* t = fromInjected(Priority::Frame)) return t;
    if (me->background)
        if (Task* t = fromInjected(Priority::Background)) return t;
    const std::size_t n = m_threads.size();
    if (n < 2) return nullptr;
    me->rng ^= me->rng << 13;
    me->rng ^= me->rng >> 17;
    me->rng ^= me->rng << 5;
    const std::size_t first = me->rng % n;
    for (int p = 0; p < 2; ++p) {
        if (p == 0 ? !me->frame : !me->background) continue;
        for (std::size_t k = 0; k < n; ++k) {
            Thread* victim = m_threads[(first + k) % n].get();
            if (victim == me) continue;
            if (Task* t = victim->deques[p].steal()) {
                me->stolen.fetch_add(1, std::memory_order_relaxed);
                return t;
            }
        }
    }
    return nullptr;
}

void Pool::execute(Thread* me, Task* t) {
    const Priority outer = tlsPriority;
    tlsPriority = t->priority;
    t->run(*t);
    tlsPriority = outer;
    if (me) me->run.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < t->successorCount; ++i)
        if (t->successors[i]->prerequisites.fetch_sub(1, std::memory_order_acq_rel) == 1) push(t->successors[i]);
    if (t->moreSuccessors)
        for (Task* s : *t->moreSuccessors)
            if (s->prerequisites.fetch_sub(1, std::memory_order_acq_rel) == 1) push(s);
    Counter* c = t->counter;
    release(t);
    // Nothing of the counter is read after this: once it reaches 0 its owner may free it.
    if (c && c->value.fetch_sub(1, std::memory_order_acq_rel) == (kSleeping | 1u)) futexWake(c->value, INT_MAX);
}

void Pool::sleepOn(Counter& c) {
    for (;;) {
        uint32_t v = c.value.load(std::memory_order_acquire);
        if ((v & ~kSleeping) == 0) return;
        if (!(v & kSleeping) && !c.value.compare_exchange_weak(v, v | kSleeping, std::memory_order_acq_rel)) continue;
        futexWait(c.value, v | kSleeping);
    }
}

void Pool::wait(Counter& c) {
    Thread* me = tlsThread;
    const auto done = [&] { return (c.value.load(std::memory_order_acquire) & ~kSleeping) == 0; };
    if (!me) {
        sleepOn(c);
        return;
    }
    const auto spin = std::chrono::microseconds(m_config.spinUs);
    while (!done()) {
        if (Task* t = find(me)) {
            execute(me, t);
            continue;
        }
        Task* t = nullptr;
        const auto until = Clock::now() + spin;
        while (!done() && !(t = find(me)) && Clock::now() < until) pause();
        if (t) {
            execute(me, t);
            continue;
        }
        if (!done()) sleepOn(c);
    }
}

void Pool::workerLoop(Thread* me) {
    Group& g = m_groups[me->group];
    const auto spin = std::chrono::microseconds(m_config.spinUs);
    while (!m_stopping.load(std::memory_order_relaxed)) {
        if (Task* t = find(me)) {
            execute(me, t);
            continue;
        }
        g.spinners.fetch_add(1, std::memory_order_seq_cst);
        Task* t = nullptr;
        const auto until = Clock::now() + spin;
        do {
            if ((t = find(me)) || m_stopping.load(std::memory_order_relaxed)) break;
            pause();
        } while (Clock::now() < until);
        const uint32_t left = g.spinners.fetch_sub(1, std::memory_order_seq_cst) - 1;
        if (t) {
            // It leaves the spin with work while no other spins: wake one more, so a burst spreads.
            if (left == 0 && g.sleepers.load(std::memory_order_relaxed) > 0) wake(g);
            execute(me, t);
            continue;
        }
        const uint32_t word = g.word.load(std::memory_order_seq_cst);
        g.sleepers.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);   // pairs with notify's
        t = find(me);
        if (t || m_stopping.load(std::memory_order_relaxed)) {
            g.sleepers.fetch_sub(1, std::memory_order_seq_cst);
            if (t) execute(me, t);
            continue;
        }
        me->sleeps.fetch_add(1, std::memory_order_relaxed);
        futexWait(g.word, word);
        g.sleepers.fetch_sub(1, std::memory_order_seq_cst);
    }
}

Stats Pool::stats() const {
    Stats s;
    for (const auto& t : m_threads) {
        s.run += t->run.load(std::memory_order_relaxed);
        s.stolen += t->stolen.load(std::memory_order_relaxed);
        s.overflowed += t->overflowed.load(std::memory_order_relaxed);
        s.sleeps += t->sleeps.load(std::memory_order_relaxed);
    }
    s.injected = g_injected.load(std::memory_order_relaxed);
    s.wakes = g_wakes.load(std::memory_order_relaxed);
    s.slabBlocks = g_slabBlocks.load(std::memory_order_relaxed);
    return s;
}

void Pool::resetStats() {
    for (const auto& t : m_threads) {
        t->run = 0;
        t->stolen = 0;
        t->overflowed = 0;
        t->sleeps = 0;
    }
    g_injected = 0;
    g_wakes = 0;
    g_slabBlocks = 0;
}

std::string Pool::describe() const {
    const auto list = [](const std::vector<int>& cpus) {
        std::string s;
        for (std::size_t i = 0; i < cpus.size(); ++i) {
            // Runs as ranges: 4-15.
            std::size_t j = i;
            while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
            if (!s.empty()) s += ',';
            s += std::to_string(cpus[i]);
            if (j > i) s += '-' + std::to_string(cpus[j]);
            i = j;
        }
        return s.empty() ? std::string("any") : s;
    };
    std::string s = std::to_string(m_threads.size()) + " threads: the game thread on " +
                    (m_gameCpu >= 0 ? "CPU " + std::to_string(m_gameCpu) : std::string("any CPU"));
    s += ", " + std::to_string(m_groups[kP].members) + " P-workers";
    if (m_groups[kP].members) s += " on " + list(m_threads[1]->cpus.size() > 1 || m_config.pin != Pin::Cpu ? m_threads[1]->cpus : std::vector<int>{});
    s += ", " + std::to_string(m_groups[kE].members) + " E-workers" + (m_config.eFrame ? "" : " (background only)");
    if (m_groups[kE].members) s += " on " + list(m_threads[1 + static_cast<std::size_t>(m_groups[kP].members)]->cpus);
    s += "; pin " + std::string(m_config.pin == Pin::Class ? "class" : m_config.pin == Pin::Cpu ? "cpu" : "none") +
         ", spin " + std::to_string(m_config.spinUs) + " us";
    return s;
}

} // namespace

Config configFromEnvironment(int threads) {
    Config c;
    if (threads > 0) {
        c.threads = threads;
    } else if (const char* env = std::getenv("OHAO_THREADS"); env && *env) {
        c.threads = std::max(0, std::atoi(env));
    }
    if (const char* env = std::getenv("OHAO_JOBS"); env && *env) {
        std::string_view s(env);
        while (!s.empty()) {
            const std::size_t comma = std::min(s.find(','), s.size());
            const std::string_view item = s.substr(0, comma);
            const std::size_t eq = item.find('=');
            const std::string_view k = item.substr(0, eq), v = eq == std::string_view::npos ? "" : item.substr(eq + 1);
            if (k == "pin" && (v == "class" || v == "cpu" || v == "none"))
                c.pin = v == "class" ? Pin::Class : v == "cpu" ? Pin::Cpu : Pin::None;
            else if (k == "smt" && (v == "on" || v == "off")) c.smt = v == "on";
            else if (k == "e" && (v == "frame" || v == "background")) c.eFrame = v == "frame";
            else if (k == "spin" && !v.empty()) c.spinUs = static_cast<uint32_t>(std::atoi(std::string(v).c_str()));
            else OHAO_LOG(Warn, runtime, "jobs: OHAO_JOBS: '{}' is not pin=class|cpu|none, smt=on|off, e=frame|background or spin=US", item);
            s.remove_prefix(std::min(comma + 1, s.size()));
        }
    }
    return c;
}

bool configure(const Config& config) {
    std::lock_guard lock(g_configMutex);
    if (g_started) {
        OHAO_LOG(Warn, runtime, "jobs: the pool already runs; its configuration stays");
        return false;
    }
    g_config = config;
    g_configured = true;
    return true;
}

Config config() {
    {
        std::lock_guard lock(g_configMutex);
        if (g_started) return g_config;
    }
    return configFromEnvironment();
}

int threadCount() { return pool().threadCount(); }
int frameThreadCount() { return pool().frameThreadCount(); }
int sleeping() { return pool().sleeping(); }
Stats stats() { return pool().stats(); }
void resetStats() { pool().resetStats(); }
std::string describe() { return pool().describe(); }

Task* allocate(const char* name, Priority priority, Task::Fn run, Task::Fn destroy) {
    Slab& s = mySlab();
    if (!s.free) s.free = s.returned.exchange(nullptr, std::memory_order_acquire);
    if (!s.free) {
        auto block = std::make_unique<Task[]>(kBlockTasks);
        for (int i = 0; i < kBlockTasks; ++i) {
            block[i].slab = &s;
            block[i].nextFree = i + 1 < kBlockTasks ? &block[i + 1] : nullptr;
        }
        s.free = &block[0];
        s.blocks.push_back(std::move(block));
        g_slabBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    Task* t = s.free;
    s.free = t->nextFree;
    t->nextFree = nullptr;
    t->run = run;
    t->destroy = destroy;
    t->name = name;
    t->counter = nullptr;
    t->prerequisites.store(0, std::memory_order_relaxed);
    t->priority = priority;
    t->successorCount = 0;
    t->moreSuccessors = nullptr;
    return t;
}

void precede(Task* before, Task* after) {
    after->prerequisites.fetch_add(1, std::memory_order_relaxed);
    if (before->successorCount < Task::kInlineSuccessors) {
        before->successors[before->successorCount++] = after;
        return;
    }
    if (!before->moreSuccessors) before->moreSuccessors = new std::vector<Task*>;
    before->moreSuccessors->push_back(after);
}

void submit(Task* task) { pool().push(task); }

void wait(Counter& counter) { pool().wait(counter); }

Priority currentPriority() noexcept { return tlsPriority; }

} // namespace ohao::jobs
