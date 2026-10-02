#pragma once
#include <chrono>
#include <cstdint>
#include <deque>

namespace ohao::runtime {

using Nanos = std::chrono::nanoseconds;

struct ClockConfig {
    Nanos fixedStep{16'666'667};   // 1/60 s
    int   maxSteps{5};             // per frame; the rest is discarded
};

struct Tick {
    Nanos frameDt{0};      // clamped to >= 0
    int   steps{0};        // fixed updates to run this frame
    float alpha{0.0f};     // leftover / fixedStep, in [0,1): render interpolation
    Nanos discarded{0};    // time dropped this frame by the maxSteps clamp
};

// Fixed-step accumulator in integer nanoseconds. Floating seconds drift:
// 60 frames of 1/60 s must be exactly 60 steps, always.
class FixedStepClock {
public:
    explicit FixedStepClock(ClockConfig cfg = {}) : m_cfg(cfg) {}

    Tick advance(Nanos frameDt);

    [[nodiscard]] float fixedDtSeconds() const noexcept {
        return std::chrono::duration<float>(m_cfg.fixedStep).count();
    }
    [[nodiscard]] uint64_t totalSteps() const noexcept { return m_totalSteps; }
    [[nodiscard]] Nanos    totalDiscarded() const noexcept { return m_totalDiscarded; }

private:
    ClockConfig m_cfg;
    Nanos       m_accumulator{0};
    uint64_t    m_totalSteps{0};
    Nanos       m_totalDiscarded{0};
};

// Rolling average of frame times over the last `window` frames.
class FrameStats {
public:
    explicit FrameStats(size_t window = 120) : m_window(window) {}
    void record(double cpuMs, double gpuMs);
    [[nodiscard]] double   avgCpuMs() const;
    [[nodiscard]] double   avgGpuMs() const;
    [[nodiscard]] double   fps() const;
    [[nodiscard]] uint64_t frames() const noexcept { return m_frames; }

private:
    size_t m_window;
    std::deque<double> m_cpu, m_gpu;
    uint64_t m_frames{0};
};

} // namespace ohao::runtime
