#include "core/jobs/deque.hpp"

namespace ohao::jobs {

namespace {
constexpr int64_t kMask = Deque::kCapacity - 1;
static_assert((Deque::kCapacity & kMask) == 0, "the capacity is a power of two");
} // namespace

bool Deque::push(Task* task) noexcept {
    const int64_t b = m_bottom.load(std::memory_order_relaxed);
    const int64_t t = m_top.load(std::memory_order_acquire);
    if (b - t >= kCapacity) return false;
    m_slots[b & kMask].store(task, std::memory_order_relaxed);
    m_bottom.store(b + 1, std::memory_order_release);   // publishes the slot (and the task) to a steal's acquire
    return true;
}

Task* Deque::take() noexcept {
    const int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(b, std::memory_order_relaxed);
    // The store of bottom must be seen before top is read (a store-load order, the one x86 reorders): without it a
    // thief and the owner can both take the last tasks.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = m_top.load(std::memory_order_relaxed);
    if (t > b) {   // empty
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return nullptr;
    }
    Task* task = m_slots[b & kMask].load(std::memory_order_relaxed);
    if (t == b) {   // the last one: race the thieves for it
        if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) task = nullptr;
        m_bottom.store(b + 1, std::memory_order_relaxed);
    }
    return task;
}

Task* Deque::steal() noexcept {
    int64_t t = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const int64_t b = m_bottom.load(std::memory_order_acquire);
    if (t >= b) return nullptr;
    Task* task = m_slots[t & kMask].load(std::memory_order_relaxed);
    if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) return nullptr;
    return task;
}

int64_t Deque::sizeApprox() const noexcept {
    const int64_t b = m_bottom.load(std::memory_order_relaxed);
    const int64_t t = m_top.load(std::memory_order_relaxed);
    return b > t ? b - t : 0;
}

} // namespace ohao::jobs
