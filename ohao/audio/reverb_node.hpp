#pragma once
// The reverb on the bank's return bus (sub-project 11, docs/specs/2026-10-28-audio-design.md, OPEN
// ruling 10): Jezar's Freeverb (public domain), our own code. Per channel 8 lowpass-feedback combs in
// parallel, then 4 allpasses in series; the right channel's delays are the left's plus a stereo
// spread of 23 samples. The tunings are Freeverb's at 44.1 kHz, scaled to the engine's rate.
//
// A preset change crossfades: two cores run in parallel while their wet gains ramp linearly in
// opposite directions (the new one starts empty), then the old one stops. One core costs one
// reverb; a crossfade costs two.
//
// Deterministic: a fixed processing order (combs 0..7, then allpasses 0..3, left before right),
// single precision, denormals flushed to zero in every feedback store. No miniaudio here:
// AudioSystem wraps it in a node on the return (audio_system.cpp); the bench and the tests run it
// directly.
#include "audio/audio_bank.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ohao {

class Reverb {
public:
    explicit Reverb(uint32_t sampleRate);

    // The preset to fade to over `crossfadeFrames` (0: at once, the old tail dropped). A change
    // during a crossfade starts a new one from the current gains.
    void setPreset(const ReverbPreset& preset, uint32_t crossfadeFrames);
    // `frames` interleaved stereo frames in, the wet signal out (no dry: the return carries only
    // what was sent to it). `in` and `out` may not overlap.
    void process(const float* in, float* out, uint32_t frames);

    [[nodiscard]] bool crossfading() const noexcept { return m_fadeLeft > 0; }
    [[nodiscard]] uint32_t sampleRate() const noexcept { return m_rate; }

    // The RT60 a room size gives, from the combs' feedback at DC and their mean delay:
    // 60 dB over the time it takes -20·log10(g) dB per pass to add up to it.
    [[nodiscard]] static double designRt60(float room, uint32_t sampleRate);

private:
    struct Comb {
        std::vector<float> buffer;
        uint32_t at{0};
        float store{0.0f};
    };
    struct Allpass {
        std::vector<float> buffer;
        uint32_t at{0};
    };
    struct Core {
        std::array<Comb, 8> combL, combR;
        std::array<Allpass, 4> allL, allR;
        float feedback{0.0f}, damp1{0.0f}, damp2{1.0f}, wet{0.0f};
        float gain{0.0f};   // this core's share of the output (the crossfade)
        bool active{false};
        void clear();
        void set(const ReverbPreset& p);
    };

    void initCore(Core& c) const;

    uint32_t m_rate;
    std::array<Core, 2> m_cores;
    int m_current{0};                       // the core faded (or fading) in
    float m_fromGain{0.0f}, m_toGain{0.0f}; // the gains at the crossfade's start
    uint32_t m_fadeLen{0}, m_fadeLeft{0};
};

} // namespace ohao
