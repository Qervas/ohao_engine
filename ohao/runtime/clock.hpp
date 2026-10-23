#pragma once
// The fixed-step clock and the time model live in core/timeline.hpp (the API shares
// them); this is the window host's frame statistics.
#include "core/timeline.hpp"

#include <cstdint>
#include <deque>

namespace ohao::runtime {

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
