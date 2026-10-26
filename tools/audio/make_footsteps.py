"""Generates assets/audio/footsteps/<surface>_<n>.wav, OHAO's own footstep sounds (sub-project 6 step 4,
docs/specs/2026-10-26-player-physics-design.md, "Footsteps"). Synthesized here from noise and decaying
partials with a fixed seed, so the files are ours (MIT, as the repo) and rebuild byte for byte:

    python tools/audio/make_footsteps.py [out_dir]

Each step is a heel strike and a softer toe strike 60-90 ms later: a burst of filtered noise for the
contact and, by surface, a body: concrete a low thump and a dry scuff, metal ringing partials, wood a
hollow knock, default a soft dull pat. Four variations per surface (pitch, timing and level vary).
Mono, 16-bit, 44.1 kHz, 0.3 s.
"""
import os
import sys
import wave

import numpy as np

RATE = 44100
LENGTH = 0.3
SURFACES = ("default", "concrete", "metal", "wood")
VARIATIONS = 4


def lowpass(x, cutoff):
    """One-pole low-pass at `cutoff` Hz."""
    a = np.exp(-2.0 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1.0 - a) * v + a * acc
        y[i] = acc
    return y


def bandpass(x, lo, hi):
    return lowpass(x, hi) - lowpass(x, lo)


def envelope(t, start, attack, decay):
    """0 before `start`, a linear attack, then exponential decay with time constant `decay` (s)."""
    u = t - start
    env = np.where(u < 0.0, 0.0, np.where(u < attack, u / attack, np.exp(-(u - attack) / decay)))
    return env


def strike(rng, t, start, surface, level):
    noise = rng.standard_normal(t.size)
    jitter = rng.uniform(0.92, 1.08)
    if surface == "concrete":
        contact = bandpass(noise, 300.0 * jitter, 3200.0 * jitter) * envelope(t, start, 0.002, 0.018)
        body = np.sin(2 * np.pi * 85.0 * jitter * t) * envelope(t, start, 0.003, 0.035) * 0.9
        scuff = bandpass(rng.standard_normal(t.size), 2000.0, 6000.0) * envelope(t, start + 0.012, 0.01, 0.03) * 0.35
        sound = contact * 1.3 + body + scuff
    elif surface == "metal":
        contact = bandpass(noise, 800.0, 6000.0) * envelope(t, start, 0.001, 0.008)
        ring = sum(np.sin(2 * np.pi * f * jitter * t + rng.uniform(0, 6.28)) * g
                   for f, g in ((620.0, 0.5), (1270.0, 0.35), (2310.0, 0.22), (3480.0, 0.12)))
        sound = contact * 1.2 + ring * envelope(t, start, 0.001, 0.11) * 0.55
    elif surface == "wood":
        contact = bandpass(noise, 250.0 * jitter, 1800.0 * jitter) * envelope(t, start, 0.002, 0.02)
        knock = (np.sin(2 * np.pi * 190.0 * jitter * t) * 0.8 + np.sin(2 * np.pi * 410.0 * jitter * t) * 0.35)
        sound = contact * 1.1 + knock * envelope(t, start, 0.002, 0.045)
    else:   # default: a soft, dull pat
        contact = lowpass(noise, 1100.0 * jitter) * envelope(t, start, 0.003, 0.025)
        body = np.sin(2 * np.pi * 110.0 * jitter * t) * envelope(t, start, 0.004, 0.03) * 0.5
        sound = contact * 2.0 + body
    return sound * level


def footstep(surface, n):
    rng = np.random.default_rng(1000 * (SURFACES.index(surface) + 1) + n)
    t = np.arange(int(RATE * LENGTH)) / RATE
    heel = rng.uniform(0.004, 0.012)
    toe = heel + rng.uniform(0.06, 0.09)
    x = strike(rng, t, heel, surface, rng.uniform(0.85, 1.0)) + strike(rng, t, toe, surface, rng.uniform(0.35, 0.55))
    fade = np.clip((LENGTH - t) / 0.03, 0.0, 1.0)   # no click at the end
    x *= fade
    x *= 0.8 / max(1e-9, np.max(np.abs(x)))
    return np.round(x * 32767.0).astype("<i2")


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("assets", "audio", "footsteps")
    os.makedirs(out, exist_ok=True)
    for surface in SURFACES:
        for n in range(1, VARIATIONS + 1):
            write(os.path.join(out, f"{surface}_{n}.wav"), footstep(surface, n))
    print(f"wrote {len(SURFACES) * VARIATIONS} footsteps to {out}")


if __name__ == "__main__":
    main()
