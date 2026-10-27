#include "runtime/frame_log.hpp"

#include <algorithm>
#include <cmath>

namespace ohao::runtime {

namespace {

double ms(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// The newest `n` records (at least one, at most all).
template <class Frames>
auto newestFrom(const Frames& frames, int n) {
    const std::size_t take = std::min(frames.size(), static_cast<std::size_t>(std::max(n, 1)));
    return frames.end() - static_cast<std::ptrdiff_t>(take);
}

} // namespace

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double rank = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(rank));
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (values[hi] - values[lo]) * (rank - static_cast<double>(lo));
}

void FrameLog::record(FrameStamps frame) {
    m_frames.push_back(std::move(frame));
    while (m_frames.size() > m_capacity) m_frames.pop_front();
}

bool FrameLog::present(uint64_t frame, FrameTime at, bool waited) {
    // Presents resolve in order, mostly for the newest frames: search from the back.
    for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it) {
        if (it->frame != frame) continue;
        it->present = at;
        it->presentWait = waited;
        return true;
    }
    return false;
}

bool FrameLog::consumed(uint64_t frame, FrameTime poll, FrameTime at) {
    for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it) {
        if (it->frame != frame) continue;
        for (LookSample& l : it->looks)
            if (l.poll == poll && !l.consumed) {
                l.consume = at;
                l.consumed = true;
                return true;
            }
        return false;
    }
    return false;
}

std::optional<LatencyStats> FrameLog::latency(int frames) const {
    if (m_frames.empty()) return std::nullopt;
    std::vector<double> total, consume, view, submit, present;
    LatencyStats st;
    st.presentWait = true;
    for (auto it = newestFrom(m_frames, frames); it != m_frames.end(); ++it) {
        if (!it->present || it->looks.empty()) continue;
        ++st.frames;
        st.presentWait = st.presentWait && it->presentWait;
        for (const LookSample& l : it->looks) {
            total.push_back(ms(*it->present - l.poll));
            if (l.consumed) consume.push_back(ms(l.consume - l.poll));
            view.push_back(ms(it->view - l.poll));
            submit.push_back(ms(it->submit - it->view));
            present.push_back(ms(*it->present - it->submit));
        }
    }
    if (total.empty()) return std::nullopt;
    st.samples = static_cast<int>(total.size());
    st.medianMs = percentile(total, 0.5);
    st.p95Ms = percentile(total, 0.95);
    st.pollToConsumeMs = percentile(consume, 0.5);
    st.pollToViewMs = percentile(view, 0.5);
    st.viewToSubmitMs = percentile(submit, 0.5);
    st.submitToPresentMs = percentile(present, 0.5);
    return st;
}

std::optional<PacingStats> FrameLog::pacing(int frames) const {
    if (m_frames.empty()) return std::nullopt;
    PacingStats st;
    std::vector<double> times;
    for (auto it = newestFrom(m_frames, frames); it != m_frames.end(); ++it) {
        times.push_back(it->frameMs);
        ++st.steps[static_cast<std::size_t>(std::clamp(it->steps, 0, 3))];
        st.discardedSteps += static_cast<uint64_t>(std::max(it->discardedSteps, 0));
    }
    st.frames = static_cast<int>(times.size());
    st.medianMs = percentile(times, 0.5);
    st.p95Ms = percentile(times, 0.95);
    st.p99Ms = percentile(times, 0.99);
    return st;
}

} // namespace ohao::runtime
