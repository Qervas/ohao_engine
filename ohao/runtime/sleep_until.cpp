#include "runtime/sleep_until.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <thread>
#endif

namespace ohao::runtime {

void sleepUntil(std::chrono::steady_clock::time_point t) {
    const auto left = t - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
#ifdef _WIN32
    // One timer per thread; CREATE_WAITABLE_TIMER_HIGH_RESOLUTION needs Windows 10 1803.
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count()));
#else
    std::this_thread::sleep_until(t);
#endif
}

} // namespace ohao::runtime
