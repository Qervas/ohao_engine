"""Generates assets/audio/ambience/<bed>.wav, OHAO's own ambience beds (sub-project 11 step 3,
docs/specs/2026-10-28-audio-design.md, "Reverb zones": a zone's bed loops on the ambience bus while it
is the listener's). Synthesized here with a fixed seed, so the files are ours (MIT, as the repo) and
rebuild byte for byte:

    python tools/audio/make_ambience.py [out_dir]

    outdoor   wind: noise weighted toward the low mids, its level moving in slow gusts
    room      room tone: soft broadband noise over a faint ventilation hum (60 Hz and its harmonics)

Each bed is a seamless loop: built in the frequency domain (random phases, an inverse FFT), so the
signal is periodic over the file exactly, and every modulation completes whole cycles in it. Mono,
16-bit, 48 kHz (the engine's rate: decoded without resampling, the loop point stays clean), 8 s.
"""
import os
import sys
import wave

import numpy as np

RATE = 48000
LENGTH = 8.0
N = int(RATE * LENGTH)


def shaped_noise(rng, weight):
    """A periodic noise of N samples whose magnitude at frequency f is weight(f)."""
    f = np.fft.rfftfreq(N, 1.0 / RATE)
    mag = weight(np.maximum(f, 1.0))
    mag[0] = 0.0
    phase = rng.uniform(0.0, 2.0 * np.pi, f.shape)
    return np.fft.irfft(mag * np.exp(1j * phase), N)


def cycles(k, phase=0.0):
    """k whole cycles over the loop."""
    t = np.arange(N) / N
    return np.sin(2.0 * np.pi * k * t + phase)


def finish(x, peak):
    x = x * (peak / max(1e-12, np.max(np.abs(x))))
    return np.round(x * 32767.0).astype("<i2")


def outdoor(rng):
    # Wind: pinkish noise peaking around 250 Hz, rolling off above 1.2 kHz, little below 60 Hz.
    def weight(f):
        return (f / 250.0) ** 0.6 / (1.0 + (f / 250.0) ** 1.6) / (1.0 + (60.0 / f) ** 2) / (1.0 + (f / 1200.0) ** 2)
    x = shaped_noise(rng, weight)
    gusts = 1.0 + 0.35 * cycles(3, 0.4) + 0.2 * cycles(5, 1.9) + 0.1 * cycles(11, 4.2)   # whole cycles: seamless
    return finish(x * gusts, 0.5)


def room(rng):
    # Room tone: soft noise weighted to the low mids, and a ventilation hum at 60 Hz and harmonics
    # (480 cycles of 60 Hz in 8 s: seamless).
    def weight(f):
        return 1.0 / (1.0 + (f / 400.0) ** 1.2) / (1.0 + (40.0 / f) ** 2)
    noise = shaped_noise(rng, weight)
    noise *= 1.0 / max(1e-12, np.max(np.abs(noise)))
    t = np.arange(N) / RATE
    hum = 0.10 * np.sin(2 * np.pi * 60 * t) + 0.05 * np.sin(2 * np.pi * 120 * t + 0.7) + 0.025 * np.sin(2 * np.pi * 180 * t + 1.3)
    return finish(0.6 * noise + hum, 0.35)


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "assets", "audio", "ambience")
    os.makedirs(out, exist_ok=True)
    for name, make, seed in (("outdoor", outdoor, 1101), ("room", room, 1102)):
        rng = np.random.default_rng(seed)
        path = os.path.join(out, name + ".wav")
        write(path, make(rng))
        print(path)


if __name__ == "__main__":
    main()
