#pragma once
// The job pool (step 4b, "The pool"): one per process, workers by core class, a work-stealing deque per thread and
// priority, tasks from per-thread slabs.
//
// - **Threads.** The thread that first uses the pool is the game thread: it has deques of its own, and it helps
//   with frame tasks while it waits. The workers are pinned by core class: the P-workers to the performance cores
//   but the game thread's and the render thread's (the first hardware threads of the first two physical cores),
//   the E-workers to the efficiency cores. Any other thread submits through one injection queue and sleeps in a
//   wait. `threads` is the game thread and the workers: 1 runs everything on the game thread, by the same path.
// - **Priorities.** Frame tasks are what a frame waits for (the step, physics, parallelFor). Background tasks (a
//   cook, an encoder) run on workers only, never on the game thread, so a long one never lands inside a wait; with
//   no workers, a background task runs at once on the thread that submits it.
// - **Sleep and wake.** An idle worker spins for `spinUs`, then sleeps on a futex. A submit wakes a sleeper only
//   when no worker of the class that can run it is spinning; a worker that leaves the spin with work while no
//   other spins wakes one more, so a burst spreads.
// - **Tasks** never throw and never block except in wait(), which helps. A task's last prerequisite to finish
//   pushes it onto that thread's deque.
#include "core/jobs/deque.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ohao::jobs {

enum class Priority : uint8_t { Frame, Background };

// Counts tasks not yet finished; wait() returns when it reaches 0. Its address must not change while tasks
// count on it.
struct Counter {
    std::atomic<uint32_t> value{0};
    std::atomic<uint32_t> sleepers{0};   // threads asleep on value
};

struct Slab;

struct Task {
    static constexpr std::size_t kPayload = 48;
    static constexpr int kInlineSuccessors = 4;
    using Fn = void (*)(Task&);

    Fn run{nullptr};
    Fn destroy{nullptr};   // the payload's destructor; null when it has none
    const char* name{""};  // for the profiler and the fault context
    Counter* counter{nullptr};
    std::atomic<uint32_t> prerequisites{0};
    Priority priority{Priority::Frame};
    uint8_t successorCount{0};
    Task* successors[kInlineSuccessors]{};
    std::vector<Task*>* moreSuccessors{nullptr};
    Slab* slab{nullptr};
    Task* nextFree{nullptr};
    alignas(16) std::byte payload[kPayload];

    template <class F> F& as() noexcept { return *std::launder(reinterpret_cast<F*>(payload)); }
};

enum class Pin : uint8_t {
    Class,   // each worker to its class's CPU set; the game thread to its CPU
    Cpu,     // each worker to one CPU of its class; the game thread to its CPU
    None,    // nothing pinned
};

struct Config {
    int threads{0};        // the game thread and the workers; 0: every worker the set below allows
    Pin pin{Pin::Class};
    bool smt{true};        // P-workers on both hardware threads of a core (false: the first of each)
    bool eFrame{true};     // E-workers run frame tasks (false: background only)
    uint32_t spinUs{50};   // how long an idle worker spins before it sleeps
};

// The defaults (measured in step 4b-1), with `threads` from the argument when it is positive, else OHAO_THREADS,
// and the rest from OHAO_JOBS ("pin=class|cpu|none,smt=on|off,e=frame|background,spin=US"), which exists to
// measure them.
[[nodiscard]] Config configFromEnvironment(int threads = 0);
// Sets the pool's configuration; before the pool's first use only (false and a warning after).
bool configure(const Config& config);
// The configuration in effect.
[[nodiscard]] Config config();

struct Stats {
    uint64_t run{0};          // tasks run
    uint64_t stolen{0};       // of them, taken from another thread's deque
    uint64_t overflowed{0};   // pushed on a full deque, so run at once
    uint64_t injected{0};     // submitted from a thread outside the pool
    uint64_t wakes{0};        // futex wakes sent
    uint64_t sleeps{0};       // times a worker slept
    uint64_t slabBlocks{0};   // task blocks allocated (256 tasks each)
};

// The game thread and the workers (starts the pool).
[[nodiscard]] int threadCount();
// The threads that run frame tasks: the game thread and the workers but the background-only ones (starts the pool).
[[nodiscard]] int frameThreadCount();
// The workers asleep now.
[[nodiscard]] int sleeping();
// What the workers and the game thread report, summed; reset by resetStats().
[[nodiscard]] Stats stats();
void resetStats();
// Pinned CPUs: the game thread's (-1: none) and each worker's set, for logs.
[[nodiscard]] std::string describe();

// A task from this thread's slab, with `fn` (a callable of at most 48 bytes) as its payload.
Task* allocate(const char* name, Priority priority, Task::Fn run, Task::Fn destroy);
template <class F> Task* makeTask(const char* name, Priority priority, F&& fn) {
    using Body = std::decay_t<F>;
    static_assert(sizeof(Body) <= Task::kPayload, "a task's callable holds at most 48 bytes");
    static_assert(alignof(Body) <= 16, "a task's callable is aligned to at most 16 bytes");
    Task* t = allocate(
        name, priority, [](Task& task) { task.as<Body>()(); },
        std::is_trivially_destructible_v<Body> ? nullptr : +[](Task& task) { task.as<Body>().~Body(); });
    ::new (static_cast<void*>(t->payload)) Body(std::forward<F>(fn));
    return t;
}
// `after` runs once `before` has finished (both not yet submitted).
void precede(Task* before, Task* after);
// Queues a task whose prerequisites are met (its counter already counts it).
void submit(Task* task);
// Returns when `counter` reaches 0: the game thread and the workers run frame tasks meanwhile, any other thread
// sleeps.
void wait(Counter& counter);
// The priority of the task this thread runs now (Frame outside any task): what work it forks takes by default.
[[nodiscard]] Priority currentPriority() noexcept;

// A thread of its own beside the pool, for a loop that blocks (on a condition, on a GPU fence) and so must never
// hold a worker: the render thread (render/render_thread.hpp). It runs `body` pinned to the CPU the pool keeps for
// the render thread (the first hardware thread of the second physical core; none when the pool pins nothing), runs
// no task, and a wait of its on the pool sleeps. Made on the game thread: it starts the pool there.
class Dedicated {
public:
    Dedicated(const char* name, std::function<void()> body);
    ~Dedicated();   // join()
    Dedicated(const Dedicated&) = delete;
    Dedicated& operator=(const Dedicated&) = delete;
    // Returns once `body` has returned.
    void join();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ohao::jobs
