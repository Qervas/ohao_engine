# Ambience beds

OHAO's own ambience beds for the slice (sub-project 11, `docs/specs/2026-10-28-audio-design.md`,
"Reverb zones"): the bed of the listener's reverb zone loops on the `ambience` bus, crossfaded over the
zone's 30-step blend when the zone changes. The slice's bank (`assets/audio/slice.audio.json`) names
them as the sounds `bed_outdoor` (preset `outdoor`) and `bed_room` (presets `room` and `corridor`).

- `outdoor.wav`: wind, noise weighted toward the low mids, its level moving in slow gusts.
- `room.wav`: room tone, soft broadband noise over a faint ventilation hum (60 Hz and its harmonics).

**Source:** synthesized by `tools/audio/make_ambience.py` with fixed seeds; it rebuilds them byte for
byte:

    python tools/audio/make_ambience.py [out_dir]

Each is a seamless loop: built in the frequency domain (random phases, an inverse FFT), so the signal is
periodic over the file, and every modulation completes whole cycles in it. Mono, 16-bit, 48 kHz (the
engine's rate: decoded without resampling), 8 s.

**License:** MIT, as the repository. No recorded or third-party audio.

A recording of your own goes under git-ignored `assets/local/audio/ambience/` with the same names; the
bank's folders put `assets/local/audio` first, so it plays instead, and the schedule does not change.
