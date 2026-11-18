#pragma once
// A work-stealing deque (Chase and Lev, "Dynamic Circular Work-Stealing Deque", SPAA 2005) with the C11 orderings
// Le, Pop, Cohen and Zappa Nardelli proved correct ("Correct and Efficient Work-Stealing for Weak Memory Models",
// PPoPP 2013). One owner thread pushes and takes at the bottom; any thread steals at the top.
//
// The capacity is fixed: push fails on a full deque and the caller runs the task itself, which is always correct
// for fork-join work and leaves no buffer to reclaim (a growable deque must keep old buffers until no thief reads
// them).
#include <atomic>
#include <cstdint>

namespace ohao::jobs {

struct Task;

class Deque {
public:
    static constexpr int64_t kCapacity = 4096;

    // Owner only. False when full (nothing pushed).
    bool push(Task* task) noexcept;
    // Owner only. The newest task, or null when empty.
    Task* take() noexcept;
    // Any thread. The oldest task, or null when empty or when another thread won it.
    Task* steal() noexcept;
    // Any thread; a snapshot that may be stale by the time it is read.
    [[nodiscard]] int64_t sizeApprox() const noexcept;

private:
    alignas(64) std::atomic<int64_t> m_top{0};
    alignas(64) std::atomic<int64_t> m_bottom{0};
    alignas(64) std::atomic<Task*> m_slots[kCapacity]{};
};

} // namespace ohao::jobs
