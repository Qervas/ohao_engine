#include "core/jobs/graph.hpp"

namespace ohao::jobs {

Graph::~Graph() {
    // Tasks added but never run: their payloads destroyed, their memory back to the slab (through a run-less task).
    for (Task* t : m_tasks) {
        t->run = [](Task&) {};
        t->prerequisites.store(0, std::memory_order_relaxed);
        t->successorCount = 0;
        delete t->moreSuccessors;
        t->moreSuccessors = nullptr;
        Counter c;
        c.value.store(1, std::memory_order_relaxed);
        t->counter = &c;
        t->priority = Priority::Frame;
        submit(t);
        wait(c);
    }
}

void Graph::precede(Node before, Node after) { jobs::precede(m_tasks[before], m_tasks[after]); }

Counter& Graph::run() {
    m_done.value.fetch_add(static_cast<uint32_t>(m_tasks.size()), std::memory_order_relaxed);
    for (Task* t : m_tasks) t->counter = &m_done;
    // Roots last-to-first, so the thread that runs them takes the first root first (a deque's newest).
    std::vector<Task*> roots;
    for (Task* t : m_tasks)
        if (t->prerequisites.load(std::memory_order_relaxed) == 0) roots.push_back(t);
    m_tasks.clear();
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) submit(*it);
    return m_done;
}

namespace detail {

namespace {

// Splits its chunks [first, last) in halves, pushing the upper half each time, until one is left, which it runs:
// log2(chunks) pushes on the way down, and a thief always takes the largest half there is.
void runRange(const ForLoop& loop, Counter& counter, uint32_t first, uint32_t last) {
    while (last - first > 1) {
        const uint32_t mid = first + (last - first) / 2;
        counter.value.fetch_add(1, std::memory_order_relaxed);
        Task* t = makeTask(loop.name, Priority::Frame, [&loop, &counter, mid, last] { runRange(loop, counter, mid, last); });
        t->counter = &counter;
        submit(t);
        last = mid;
    }
    const std::size_t lo = loop.begin + static_cast<std::size_t>(first) * loop.grain;
    const std::size_t hi = std::min(loop.end, lo + loop.grain);
    loop.call(loop.fn, lo, hi);
}

} // namespace

void parallelFor(const ForLoop& loop) {
    Counter counter;
    runRange(loop, counter, 0, loop.chunks);
    wait(counter);
}

} // namespace detail

} // namespace ohao::jobs
