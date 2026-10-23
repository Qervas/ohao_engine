#include "runtime/clock.hpp"
#include <numeric>

namespace ohao::runtime {

void FrameStats::record(double cpuMs, double gpuMs) {
    m_cpu.push_back(cpuMs);
    m_gpu.push_back(gpuMs);
    while (m_cpu.size() > m_window) m_cpu.pop_front();
    while (m_gpu.size() > m_window) m_gpu.pop_front();
    ++m_frames;
}

static double mean(const std::deque<double>& d) {
    return d.empty() ? 0.0 : std::accumulate(d.begin(), d.end(), 0.0) / static_cast<double>(d.size());
}

double FrameStats::avgCpuMs() const { return mean(m_cpu); }
double FrameStats::avgGpuMs() const { return mean(m_gpu); }

double FrameStats::fps() const {
    const double ms = avgCpuMs();
    return ms > 0.0 ? 1000.0 / ms : 0.0;
}

} // namespace ohao::runtime
