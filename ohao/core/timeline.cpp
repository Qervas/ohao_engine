#include "core/timeline.hpp"

namespace ohao {

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

Tick Timeline::advance(Nanos wall) {
    if (running()) return m_clock.advance(wall);
    Tick t;
    t.frameDt = wall.count() > 0 ? wall : Nanos{0};
    return t;
}

void Timeline::setMode(TimeMode mode) noexcept {
    if (mode != m_mode) m_clock.reset();
    m_mode = mode;
}

void Timeline::setPaused(bool paused) noexcept {
    if (paused != m_paused) m_clock.reset();
    m_paused = paused;
}

} // namespace ohao
