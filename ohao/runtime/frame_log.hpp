#pragma once
// A window's frame timing (sub-project 9 spec, "Latency: the definition" and "Frame pacing"):
// each frame's stamps on one steady clock, the look input it is the first to show in full, and
// the steps it ran, kept for the newest kCapacity frames. Latency is a look movement's poll to
// the present of the first frame showing it in full; pacing is the frame time's spread and the
// steps a frame ran. The window host records, render.stats and feel_bench read.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ohao::runtime {

using FrameTime = std::chrono::steady_clock::time_point;

// One look movement: when it was polled (or injected) and when the step that applied it ran. With
// the look lead a frame shows a movement before a step consumes it: `consumed` is false until
// FrameLog::consumed fills the step in.
struct LookSample {
    FrameTime poll{};
    FrameTime consume{};
    bool consumed{true};
};

// Late input sampling (spec "Present mode and late input sampling"): the frame whose present a host
// waits for before it polls the next frame's input, so at most `framesAhead` submitted frames are not
// yet presented when it polls; 0: none to wait for (too few frames yet, or a negative framesAhead:
// unbounded).
[[nodiscard]] constexpr uint64_t presentToAwait(uint64_t submitted, int framesAhead) noexcept {
    if (framesAhead < 0 || submitted <= static_cast<uint64_t>(framesAhead)) return 0;
    return submitted - static_cast<uint64_t>(framesAhead);
}

struct FrameStamps {
    uint64_t frame{0};   // the renderer's submitted-frame number
    FrameTime start{};   // the loop iteration began
    FrameTime poll{};    // the window's events were polled
    FrameTime view{};    // the scene view was resolved (the camera this frame draws)
    FrameTime submit{};  // the frame's command buffers were submitted
    std::optional<FrameTime> present;   // filled when known (FrameLog::present)
    bool presentWait{false};            // the present came from the presentation engine
    std::vector<LookSample> looks;      // look this frame is the first to show in full
    double frameMs{0.0};   // wall time since the previous frame began
    int steps{0};          // simulation steps the frame ran
    int discardedSteps{0}; // steps the clock's cap dropped this frame
};

struct LatencyStats {
    int frames{0};    // presented frames that carried look
    int samples{0};   // look movements measured
    double medianMs{0.0}, p95Ms{0.0};
    bool presentWait{false};   // every sample's present came from present wait
    // Each stage's median over the samples: poll to the consuming step, poll to the frame's
    // view, the frame's view to its submit, its submit to its present.
    double pollToConsumeMs{0.0}, pollToViewMs{0.0}, viewToSubmitMs{0.0}, submitToPresentMs{0.0};
};

struct PacingStats {
    int frames{0};
    double medianMs{0.0}, p95Ms{0.0}, p99Ms{0.0};   // frame time
    std::array<uint64_t, 4> steps{};                 // frames that ran 0, 1, 2, 3+ steps
    uint64_t discardedSteps{0};
};

// The p-th quantile (p in [0, 1]) by linear interpolation between closest ranks (the median of
// an even count is the mean of the middle two); 0 for no values.
[[nodiscard]] double percentile(std::vector<double> values, double p);

class FrameLog {
public:
    static constexpr std::size_t kCapacity = 1024;   // a window's default: render.stats' reach

    explicit FrameLog(std::size_t capacity = kCapacity) : m_capacity(capacity < 1 ? 1 : capacity) {}
    void record(FrameStamps frame);
    // A recorded frame's present; false when the frame is not (or no longer) in the log.
    bool present(uint64_t frame, FrameTime at, bool waited);
    // The step that consumed a look a recorded frame showed before it (polled at `poll`).
    bool consumed(uint64_t frame, FrameTime poll, FrameTime at);
    // Over the newest `frames` records: nullopt when none of them is a presented frame with look.
    [[nodiscard]] std::optional<LatencyStats> latency(int frames) const;
    // Over the newest `frames` records: nullopt when there are none.
    [[nodiscard]] std::optional<PacingStats> pacing(int frames) const;
    [[nodiscard]] std::size_t size() const noexcept { return m_frames.size(); }
    [[nodiscard]] const std::deque<FrameStamps>& frames() const noexcept { return m_frames; }
    void clear() noexcept { m_frames.clear(); }

private:
    std::size_t m_capacity;
    std::deque<FrameStamps> m_frames;
};

} // namespace ohao::runtime
