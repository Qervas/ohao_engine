#include "gpu/vulkan/present_waiter.hpp"

#include "gpu/vulkan/swapchain.hpp"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ohao {

namespace {

// About a quarter of a millisecond: the stamp's resolution. Windows' default sleep is ~1 ms or
// more; a high-resolution waitable timer (Windows 10 1803+) keeps the poll near its period.
class PollSleeper {
public:
    PollSleeper() {
#ifdef _WIN32
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    }
    ~PollSleeper() {
#ifdef _WIN32
        if (m_timer) CloseHandle(m_timer);
#endif
    }
    void sleep() {
#ifdef _WIN32
        if (m_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -2500;   // 100 ns units, relative: 0.25 ms
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(m_timer, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }

private:
#ifdef _WIN32
    HANDLE m_timer{nullptr};
#endif
};

} // namespace

void PresentWaiter::start(VkDevice device, Swapchain& swapchain, PFN_vkWaitForPresentKHR wait) {
    stop();
    m_device = device;
    m_swapchain = &swapchain;
    m_wait = wait;
    m_stop = false;
    if (m_wait) m_thread = std::thread([this] { run(); });
}

void PresentWaiter::stop() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_resolved.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lock(m_mutex);
    m_pending.clear();
}

void PresentWaiter::presented(uint64_t frame, uint64_t presentId) {
    // A timing statistic (render.stats' latency), never an input to a frame.
    const Clock::time_point callReturned = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    if (!m_wait || presentId == 0) {
        m_done.push_back({frame, callReturned, false});
        m_resolvedUpTo = std::max(m_resolvedUpTo, frame);
        m_resolved.notify_all();
        return;
    }
    m_pending.push_back({frame, presentId, m_swapchain->generation(), callReturned});
    m_wake.notify_one();
}

std::vector<PresentStamp> PresentWaiter::take() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_done, {});
}

void PresentWaiter::resolve(const Pending& p, Clock::time_point at, bool waited) {
    std::lock_guard lock(m_mutex);
    if (!m_pending.empty() && m_pending.front().id == p.id) m_pending.pop_front();
    m_done.push_back({p.frame, at, waited});
    m_resolvedUpTo = std::max(m_resolvedUpTo, p.frame);
    if (waited) m_confirmedUpTo = std::max(m_confirmedUpTo, p.frame);
    m_resolved.notify_all();
}

bool PresentWaiter::waitResolved(uint64_t frame, std::chrono::microseconds timeout) {
    std::unique_lock lock(m_mutex);
    m_resolved.wait_for(lock, timeout, [&] { return m_stop || m_resolvedUpTo >= frame; });
    return m_confirmedUpTo >= frame;
}

void PresentWaiter::run() {
    PollSleeper sleeper;
    for (;;) {
        Pending p;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || !m_pending.empty(); });
            if (m_stop) return;
            p = m_pending.front();
        }
        VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
        {
            // A present id belongs to the swapchain it was presented on: after a rebuild it is
            // never answered, so it keeps its call stamp.
            std::lock_guard host(m_swapchain->hostMutex());
            if (m_swapchain->valid() && m_swapchain->generation() == p.generation)
                r = m_wait(m_device, m_swapchain->handle(), p.id, 0);
        }
        const Clock::time_point now = std::chrono::steady_clock::now();
        if (r == VK_SUCCESS) {
            resolve(p, now, true);
        } else if (r != VK_TIMEOUT || now - p.call > kGiveUp) {
            resolve(p, p.call, false);
        } else {
            sleeper.sleep();
        }
    }
}

} // namespace ohao
