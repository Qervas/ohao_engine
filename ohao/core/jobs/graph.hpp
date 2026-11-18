#pragma once
// The job pool's API for work (step 4b): a task graph, a parallel loop and a background task.
//
//   jobs::Graph g;
//   const auto a = g.add("physics.broad", [&] { ... });
//   const auto b = g.add("physics.narrow", [&] { ... });
//   g.precede(a, b);
//   jobs::wait(g.run());
//
//   jobs::parallelFor(0, n, 64, [&](std::size_t lo, std::size_t hi) { ... });   // returns when all ran
//
// parallelFor's chunks are [begin + k * grain, begin + (k + 1) * grain), cut by the range and the grain alone, so a
// body that writes per index gives the same result at any thread count; reductions are the caller's, in index order.
// At one thread it runs the same tasks on the calling thread.
#include "core/jobs/pool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace ohao::jobs {

class Graph {
public:
    using Node = uint32_t;
    Graph() = default;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    ~Graph();   // tasks never run are freed

    template <class F> Node add(const char* name, F&& fn, Priority priority = Priority::Frame) {
        m_tasks.push_back(makeTask(name, priority, std::forward<F>(fn)));
        return static_cast<Node>(m_tasks.size() - 1);
    }
    // `after` runs once `before` has finished.
    void precede(Node before, Node after);
    // Submits every task; the counter reaches 0 when all have run. The graph is empty again and may be reused
    // once the counter is 0; it must outlive the counter's wait.
    Counter& run();

private:
    std::vector<Task*> m_tasks;
    Counter m_done;
};

namespace detail {
// One parallelFor's loop, on its caller's stack; each task holds a pointer to it and the chunks it covers.
struct ForLoop {
    void* fn;
    void (*call)(void*, std::size_t, std::size_t);
    std::size_t begin, end, grain;
    uint32_t chunks;
    const char* name;
};
void parallelFor(const ForLoop& loop);
} // namespace detail

// fn(lo, hi) for each chunk of [begin, end); returns when every chunk ran.
template <class F> void parallelFor(std::size_t begin, std::size_t end, std::size_t grain, F&& fn, const char* name = "parallelFor") {
    if (end <= begin) return;
    grain = std::max<std::size_t>(grain, 1);
    using Body = std::remove_reference_t<F>;
    const std::size_t chunks = (end - begin + grain - 1) / grain;
    const detail::ForLoop loop{const_cast<void*>(static_cast<const void*>(&fn)),
                               [](void* f, std::size_t lo, std::size_t hi) { (*static_cast<Body*>(f))(lo, hi); },
                               begin, end, grain, static_cast<uint32_t>(chunks), name};
    detail::parallelFor(loop);
}

// fn() on a worker, at background priority (at once on this thread when the pool has no workers). `done`, when
// given, counts it.
template <class F> void background(const char* name, F&& fn, Counter* done = nullptr) {
    Task* t = makeTask(name, Priority::Background, std::forward<F>(fn));
    if (done) {
        done->value.fetch_add(1, std::memory_order_relaxed);
        t->counter = done;
    }
    submit(t);
}

} // namespace ohao::jobs
