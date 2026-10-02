#include "runtime/clock.hpp"
#include <numeric>

namespace ohao::runtime {

Tick FixedStepClock::advance(Nanos frameDt) {
    Tick t;
    t.frameDt = frameDt.count() > 0 ? frameDt : Nanos{0};
    m_accumulator += t.frameDt;

    const Nanos step = m_cfg.fixedStep;
    int64_t due = m_accumulator / step;
    if (due > m_cfg.maxSteps) {
        // Drop whole steps beyond the clamp; keep the sub-step remainder so the
        // next frame's alpha is still meaningful.
        const Nanos excess = (due - m_cfg.maxSteps) * step;
        m_accumulator -= excess;
        t.discarded = excess;
        m_totalDiscarded += excess;
        due = m_cfg.maxSteps;
    }
    m_accumulator -= due * step;
    t.steps = static_cast<int>(due);
    m_totalSteps += static_cast<uint64_t>(due);
    t.alpha = static_cast<float>(static_cast<double>(m_accumulator.count()) /
                                 static_cast<double>(step.count()));
    return t;
}

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
