"""Generates assets/audio/weapons/<name>.wav, OHAO's own combat sounds (sub-project 7 step 4,
docs/specs/2026-10-27-combat-design.md, "Presentation"). Synthesized here from noise and decaying
partials with fixed seeds, so the files are ours (MIT, as the repo) and rebuild byte for byte:

    python tools/audio/make_weapons.py [out_dir]

    fire             the player's rifle: a sharp crack over a low thump and a short noise tail
    enemy_fire       an enemy's: the same family, darker and a little softer
    impact_<surface> a round striking default, concrete, metal or wood
    hurt             the player hit: a dull body thump
    death            a heavier thump with a falling tone
    reload           magazine out, magazine in, a bolt pull
    empty            the dry click of an empty magazine
    mag_out          handling (animation events): the magazine released and pulled out
    mag_in           a magazine slid in and seated
    bolt             the bolt pulled back and let go
    body_fall        a body hitting the floor

Mono, 16-bit, 44.1 kHz.
"""
import os
import sys
import wave

import numpy as np

RATE = 44100


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
    return np.where(u < 0.0, 0.0, np.where(u < attack, u / attack, np.exp(-(u - attack) / decay)))


def timeline(length):
    return np.arange(int(RATE * length)) / RATE


def finish(x, length, level=0.85):
    t = timeline(length)
    x = x * np.clip((length - t) / 0.02, 0.0, 1.0)   # no click at the end
    x *= level / max(1e-9, np.max(np.abs(x)))
    return np.round(x * 32767.0).astype("<i2")


def shot(seed, dark):
    rng = np.random.default_rng(seed)
    length = 0.45
    t = timeline(length)
    crack = bandpass(rng.standard_normal(t.size), 1800.0 * (0.7 if dark else 1.0), 9000.0 * (0.6 if dark else 1.0))
    crack *= envelope(t, 0.0, 0.0008, 0.012)
    body = lowpass(rng.standard_normal(t.size), 1200.0 if dark else 2200.0) * envelope(t, 0.0, 0.002, 0.06)
    thump = np.sin(2 * np.pi * (70.0 if dark else 90.0) * t) * envelope(t, 0.0, 0.002, 0.05)
    tail = lowpass(rng.standard_normal(t.size), 600.0) * envelope(t, 0.02, 0.01, 0.16) * 0.5
    return finish(crack * (1.0 if dark else 1.6) + body * 1.4 + thump * 1.2 + tail, length, 0.75 if dark else 0.9)


def impact(surface, seed):
    rng = np.random.default_rng(seed)
    length = 0.25
    t = timeline(length)
    noise = rng.standard_normal(t.size)
    if surface == "metal":
        hit = bandpass(noise, 1500.0, 8000.0) * envelope(t, 0.0, 0.0005, 0.006)
        ring = sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)) * g
                   for f, g in ((1830.0, 0.5), (2970.0, 0.35), (4410.0, 0.2), (6050.0, 0.1)))
        x = hit * 1.3 + ring * envelope(t, 0.0, 0.0005, 0.07) * 0.6
    elif surface == "wood":
        hit = bandpass(noise, 400.0, 3000.0) * envelope(t, 0.0, 0.001, 0.012)
        knock = np.sin(2 * np.pi * 240.0 * t) * 0.8 + np.sin(2 * np.pi * 520.0 * t) * 0.3
        x = hit * 1.2 + knock * envelope(t, 0.0, 0.001, 0.035)
    elif surface == "concrete":
        hit = bandpass(noise, 600.0, 7000.0) * envelope(t, 0.0, 0.0005, 0.01)
        debris = bandpass(rng.standard_normal(t.size), 2500.0, 9000.0) * envelope(t, 0.01, 0.005, 0.05) * 0.4
        x = hit * 1.5 + debris + np.sin(2 * np.pi * 120.0 * t) * envelope(t, 0.0, 0.001, 0.02) * 0.5
    else:   # default: a dull thud with a little grit
        hit = lowpass(noise, 1800.0) * envelope(t, 0.0, 0.001, 0.02)
        x = hit * 1.6 + np.sin(2 * np.pi * 150.0 * t) * envelope(t, 0.0, 0.001, 0.03) * 0.6
    return finish(x, length, 0.8)


def hurt():
    rng = np.random.default_rng(71)
    length = 0.3
    t = timeline(length)
    thump = np.sin(2 * np.pi * (95.0 - 30.0 * t) * t) * envelope(t, 0.0, 0.003, 0.07)
    smack = lowpass(rng.standard_normal(t.size), 900.0) * envelope(t, 0.0, 0.002, 0.03)
    return finish(thump * 1.2 + smack * 1.5, length, 0.85)


def death():
    rng = np.random.default_rng(83)
    length = 0.8
    t = timeline(length)
    freq = 220.0 * np.exp(-2.2 * t)
    tone = np.sin(2 * np.pi * np.cumsum(freq) / RATE) * envelope(t, 0.0, 0.01, 0.25) * 0.6
    fall = lowpass(rng.standard_normal(t.size), 500.0) * envelope(t, 0.28, 0.005, 0.12) * 1.4
    thud = np.sin(2 * np.pi * 60.0 * t) * envelope(t, 0.28, 0.003, 0.09)
    return finish(tone + fall + thud, length, 0.85)


def click(rng, t, start, bright, level):
    noise = rng.standard_normal(t.size)
    c = bandpass(noise, 1500.0 * bright, 7000.0 * bright) * envelope(t, start, 0.0004, 0.005)
    return (c + np.sin(2 * np.pi * 900.0 * bright * t) * envelope(t, start, 0.0005, 0.008) * 0.4) * level


def reload():
    rng = np.random.default_rng(97)
    length = 0.9
    t = timeline(length)
    out = click(rng, t, 0.02, 0.8, 1.0) + click(rng, t, 0.05, 1.0, 0.5)
    slide = bandpass(rng.standard_normal(t.size), 1200.0, 5000.0) * envelope(t, 0.12, 0.05, 0.05) * 0.25
    seat = click(rng, t, 0.42, 0.7, 1.2) + click(rng, t, 0.45, 1.1, 0.6)
    bolt = click(rng, t, 0.68, 1.2, 0.9) + click(rng, t, 0.76, 0.9, 1.1)
    return finish(out + slide + seat + bolt, length, 0.8)


def empty():
    rng = np.random.default_rng(101)
    length = 0.12
    t = timeline(length)
    return finish(click(rng, t, 0.005, 1.1, 1.0), length, 0.7)


# Handling sounds (sub-project 10 step 2), each played on its animation event (the enemy graph's
# reload and death clips, assets/anim/enemy.animgraph.json).

def mag_out():
    rng = np.random.default_rng(103)
    length = 0.3
    t = timeline(length)
    latch = click(rng, t, 0.01, 0.8, 1.0) + click(rng, t, 0.035, 1.0, 0.45)
    slide = bandpass(rng.standard_normal(t.size), 1200.0, 5000.0) * envelope(t, 0.05, 0.04, 0.05) * 0.3
    return finish(latch + slide, length, 0.75)


def mag_in():
    rng = np.random.default_rng(107)
    length = 0.3
    t = timeline(length)
    slide = bandpass(rng.standard_normal(t.size), 1000.0, 4500.0) * envelope(t, 0.0, 0.04, 0.03) * 0.25
    seat = click(rng, t, 0.07, 0.7, 1.2) + click(rng, t, 0.095, 1.1, 0.6)
    return finish(slide + seat, length, 0.8)


def bolt():
    rng = np.random.default_rng(109)
    length = 0.3
    t = timeline(length)
    back = click(rng, t, 0.01, 1.2, 0.9)
    rattle = bandpass(rng.standard_normal(t.size), 2000.0, 7000.0) * envelope(t, 0.02, 0.01, 0.03) * 0.3
    home = click(rng, t, 0.11, 0.9, 1.1) + click(rng, t, 0.13, 1.3, 0.5)
    return finish(back + rattle + home, length, 0.8)


def body_fall():
    rng = np.random.default_rng(113)
    length = 0.6
    t = timeline(length)
    thud = np.sin(2 * np.pi * (58.0 - 12.0 * t) * t) * envelope(t, 0.0, 0.004, 0.09)
    cloth = lowpass(rng.standard_normal(t.size), 700.0) * envelope(t, 0.0, 0.003, 0.05) * 1.3
    gear = bandpass(rng.standard_normal(t.size), 1500.0, 6000.0) * envelope(t, 0.015, 0.002, 0.04) * 0.35
    settle = lowpass(rng.standard_normal(t.size), 400.0) * envelope(t, 0.16, 0.01, 0.07) * 0.6
    return finish(thud * 1.3 + cloth + gear + settle, length, 0.85)


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("assets", "audio", "weapons")
    os.makedirs(out, exist_ok=True)
    sounds = {
        "fire": shot(11, False),
        "enemy_fire": shot(13, True),
        "impact_default": impact("default", 21),
        "impact_concrete": impact("concrete", 23),
        "impact_metal": impact("metal", 29),
        "impact_wood": impact("wood", 31),
        "hurt": hurt(),
        "death": death(),
        "reload": reload(),
        "empty": empty(),
        "mag_out": mag_out(),
        "mag_in": mag_in(),
        "bolt": bolt(),
        "body_fall": body_fall(),
    }
    for name, samples in sounds.items():
        write(os.path.join(out, f"{name}.wav"), samples)
    print(f"wrote {len(sounds)} combat sounds to {out}")


if __name__ == "__main__":
    main()
