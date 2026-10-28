#include "audio/reverb_node.hpp"

#include <algorithm>
#include <cmath>

namespace ohao {

namespace {

// Freeverb's tunings at 44.1 kHz (Jezar, public domain).
constexpr std::array<uint32_t, 8> kCombTuning{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<uint32_t, 4> kAllpassTuning{556, 441, 341, 225};
constexpr uint32_t kStereoSpread = 23;
constexpr float kFixedGain = 0.015f;   // the input into the combs (8 of them in parallel)
constexpr float kScaleWet = 3.0f;
constexpr float kScaleDamp = 0.4f;
constexpr float kScaleRoom = 0.28f;
constexpr float kOffsetRoom = 0.7f;
constexpr float kAllpassFeedback = 0.5f;

uint32_t scaled(uint32_t samples44k, uint32_t rate) {
    return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(double(samples44k) * rate / 44100.0)));
}

// Denormals in a feedback loop cost a hundredfold on x86: a decaying tail is flushed to zero long
// before it would reach them.
inline float flush(float x) { return std::fabs(x) < 1e-25f ? 0.0f : x; }

inline float comb(std::vector<float>& buf, uint32_t& at, float& store, float input, float feedback, float damp1,
                  float damp2) {
    const float out = buf[at];
    store = flush(out * damp2 + store * damp1);
    buf[at] = flush(input + store * feedback);
    if (++at >= buf.size()) at = 0;
    return out;
}

inline float allpass(std::vector<float>& buf, uint32_t& at, float input) {
    const float delayed = buf[at];
    buf[at] = flush(input + delayed * kAllpassFeedback);
    if (++at >= buf.size()) at = 0;
    return delayed - input;
}

} // namespace

void Reverb::Core::clear() {
    for (auto* combs : {&combL, &combR})
        for (Comb& c : *combs) {
            std::fill(c.buffer.begin(), c.buffer.end(), 0.0f);
            c.at = 0;
            c.store = 0.0f;
        }
    for (auto* alls : {&allL, &allR})
        for (Allpass& a : *alls) {
            std::fill(a.buffer.begin(), a.buffer.end(), 0.0f);
            a.at = 0;
        }
}

void Reverb::Core::set(const ReverbPreset& p) {
    feedback = std::clamp(p.room, 0.0f, 1.0f) * kScaleRoom + kOffsetRoom;
    damp1 = std::clamp(p.damp, 0.0f, 1.0f) * kScaleDamp;
    damp2 = 1.0f - damp1;
    wet = std::clamp(p.wet, 0.0f, 1.0f) * kScaleWet;   // width 1: each channel its own combs
}

Reverb::Reverb(uint32_t sampleRate) : m_rate(sampleRate == 0 ? 48000 : sampleRate) {
    for (Core& c : m_cores) initCore(c);
}

void Reverb::initCore(Core& c) const {
    for (std::size_t i = 0; i < kCombTuning.size(); ++i) {
        c.combL[i].buffer.assign(scaled(kCombTuning[i], m_rate), 0.0f);
        c.combR[i].buffer.assign(scaled(kCombTuning[i] + kStereoSpread, m_rate), 0.0f);
    }
    for (std::size_t i = 0; i < kAllpassTuning.size(); ++i) {
        c.allL[i].buffer.assign(scaled(kAllpassTuning[i], m_rate), 0.0f);
        c.allR[i].buffer.assign(scaled(kAllpassTuning[i] + kStereoSpread, m_rate), 0.0f);
    }
}

void Reverb::setPreset(const ReverbPreset& preset, uint32_t crossfadeFrames) {
    if (crossfadeFrames == 0) {
        Core& c = m_cores[static_cast<std::size_t>(m_current)];
        Core& other = m_cores[static_cast<std::size_t>(1 - m_current)];
        c.clear();
        c.set(preset);
        c.gain = 1.0f;
        c.active = true;
        other.active = false;
        other.gain = 0.0f;
        m_fadeLeft = m_fadeLen = 0;
        return;
    }
    // The new preset goes into the quieter core, cleared; the louder one fades out from where it is.
    const int incoming = m_fadeLeft > 0 ? (m_cores[0].gain <= m_cores[1].gain ? 0 : 1) : 1 - m_current;
    Core& in = m_cores[static_cast<std::size_t>(incoming)];
    Core& out = m_cores[static_cast<std::size_t>(1 - incoming)];
    in.clear();
    in.set(preset);
    in.gain = 0.0f;
    in.active = true;
    m_fromGain = out.active ? out.gain : 0.0f;
    m_toGain = 0.0f;
    m_current = incoming;
    m_fadeLen = m_fadeLeft = crossfadeFrames;
}

void Reverb::process(const float* in, float* out, uint32_t frames) {
    std::fill(out, out + std::size_t(frames) * 2, 0.0f);
    const uint32_t fadeDone = m_fadeLen - m_fadeLeft;   // frames of the crossfade already played
    for (int k = 0; k < 2; ++k) {
        Core& c = m_cores[static_cast<std::size_t>(k)];
        if (!c.active) continue;
        const bool incoming = k == m_current;
        for (uint32_t i = 0; i < frames; ++i) {
            float gain = c.gain;
            if (m_fadeLeft > 0) {
                const float p = std::min(1.0f, float(fadeDone + i + 1) / float(m_fadeLen));
                gain = incoming ? m_toGain + (1.0f - m_toGain) * p : m_fromGain * (1.0f - p);
            }
            const float input = (in[i * 2] + in[i * 2 + 1]) * kFixedGain;
            float l = 0.0f, r = 0.0f;
            for (std::size_t j = 0; j < c.combL.size(); ++j) {
                l += comb(c.combL[j].buffer, c.combL[j].at, c.combL[j].store, input, c.feedback, c.damp1, c.damp2);
                r += comb(c.combR[j].buffer, c.combR[j].at, c.combR[j].store, input, c.feedback, c.damp1, c.damp2);
            }
            for (std::size_t j = 0; j < c.allL.size(); ++j) {
                l = allpass(c.allL[j].buffer, c.allL[j].at, l);
                r = allpass(c.allR[j].buffer, c.allR[j].at, r);
            }
            const float g = gain * c.wet;
            out[i * 2] += l * g;
            out[i * 2 + 1] += r * g;
        }
    }
    if (m_fadeLeft > 0) {
        const uint32_t used = std::min(frames, m_fadeLeft);
        m_fadeLeft -= used;
        Core& incomingCore = m_cores[static_cast<std::size_t>(m_current)];
        Core& outgoingCore = m_cores[static_cast<std::size_t>(1 - m_current)];
        const float p = std::min(1.0f, float(m_fadeLen - m_fadeLeft) / float(m_fadeLen));
        incomingCore.gain = m_toGain + (1.0f - m_toGain) * p;
        outgoingCore.gain = m_fromGain * (1.0f - p);
        if (m_fadeLeft == 0) {
            incomingCore.gain = 1.0f;
            outgoingCore.gain = 0.0f;
            outgoingCore.active = false;
        }
    }
}

double Reverb::designRt60(float room, uint32_t sampleRate) {
    const double g = double(std::clamp(room, 0.0f, 1.0f)) * kScaleRoom + kOffsetRoom;
    double mean = 0.0;
    for (const uint32_t d : kCombTuning) mean += d;
    mean = mean / kCombTuning.size() / 44100.0;   // seconds: the delays scale with the rate
    (void)sampleRate;
    return 60.0 * mean / (-20.0 * std::log10(g));
}

} // namespace ohao
