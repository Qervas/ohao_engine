#pragma once
// Sleeps until a steady-clock time to within about a tenth of a millisecond (the frame-rate cap).
// Windows' default sleep rounds to its 15.6 ms tick, so it waits on a high-resolution waitable timer there.
#include <chrono>

namespace ohao::runtime {

void sleepUntil(std::chrono::steady_clock::time_point t);

} // namespace ohao::runtime
