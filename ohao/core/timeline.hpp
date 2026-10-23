#pragma once
// The time model (sub-project 4 spec, "Time modes"): one stepped-frame counter, the single
// time axis for input, recordings, hashes and logs, and the rule for when the world steps.
//
//   lockstep   the world advances only when asked (world.step, a replay); wall time is
//              ignored.
//   realtime   whole fixed steps from wall time, as a game runs. Paused, it behaves as
//              lockstep; resuming continues from the current state, never catching up on
//              the time spent paused.
//
// CPU-only and clock-free: the host measures wall time and passes it in, so the model is
// deterministic and testable without a window.
#include <chrono>
#include <cstdint>
#include <optional>

namespace ohao {

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
    // Drops the leftover time: the next step comes a whole fixed step from now.
    void reset() noexcept { m_accumulator = Nanos{0}; }
    void setFixedStep(Nanos step) noexcept { m_cfg.fixedStep = step; }

    [[nodiscard]] Nanos fixedStep() const noexcept { return m_cfg.fixedStep; }
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

enum class TimeMode : uint8_t { lockstep, realtime };

class Timeline {
public:
    explicit Timeline(TimeMode mode = TimeMode::lockstep, ClockConfig cfg = {}) : m_mode(mode), m_clock(cfg) {}

    [[nodiscard]] TimeMode mode() const noexcept { return m_mode; }
    [[nodiscard]] bool paused() const noexcept { return m_paused; }
    // Steps run so far (by the clock or on request): the frame number.
    [[nodiscard]] uint64_t frame() const noexcept { return m_frame; }
    [[nodiscard]] Nanos fixedStep() const noexcept { return m_clock.fixedStep(); }
    [[nodiscard]] float fixedDt() const noexcept { return m_clock.fixedDtSeconds(); }
    // The clock steps the world by itself: realtime and not paused. Otherwise steps are
    // run on request (world.step).
    [[nodiscard]] bool running() const noexcept { return m_mode == TimeMode::realtime && !m_paused; }
    // Frames per second the host presents at, when it measures it (window hosts).
    [[nodiscard]] std::optional<float> fps() const noexcept { return m_fps; }

    // One host frame of wall time. Running: the fixed steps due (the caller runs them and
    // reports them with stepped()). Otherwise no steps, and the time is dropped.
    Tick advance(Nanos wall);
    void stepped(uint64_t steps) noexcept { m_frame += steps; }
    // A restored or reloaded world carries its own frame number.
    void setFrame(uint64_t frame) noexcept { m_frame = frame; }

    // Switching mode or pausing never runs or drops a step: the clock's leftover time is
    // discarded, so the first step after resuming comes a whole fixed step later.
    void setMode(TimeMode mode) noexcept;
    void setPaused(bool paused) noexcept;
    void setFixedStep(Nanos step) noexcept { m_clock.setFixedStep(step); }
    void setFps(std::optional<float> fps) noexcept { m_fps = fps; }

private:
    TimeMode m_mode;
    bool m_paused{false};
    uint64_t m_frame{0};
    FixedStepClock m_clock;
    std::optional<float> m_fps;
};

} // namespace ohao
