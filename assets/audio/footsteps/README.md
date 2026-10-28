# Footsteps

OHAO's own footstep sounds (sub-project 6, `docs/specs/2026-10-26-player-physics-design.md`,
"Footsteps"): four variations per surface, `<surface>_<n>.wav` for `default`, `concrete`, `metal` and
`wood`. The slice's bank (`assets/audio/slice.audio.json`) names them as `footstep_<surface>` (files
`footsteps/<surface>_{n}`); the audio schedule picks the surface of the ground stepped on (`default`
for a surface without sounds) and the next variation in turn.

**Source:** synthesized by `tools/audio/make_footsteps.py` (a heel and a toe strike from filtered noise
and decaying partials, by surface, fixed seed); it rebuilds them byte for byte:

    python tools/audio/make_footsteps.py [out_dir]

Mono, 16-bit, 44.1 kHz, 0.3 s.

**License:** MIT, as the repository. No recorded or third-party audio.

**The local folder first.** Recordings for local listening (Somnabulate's included,
`tools/audio/stage_somnabulate_footsteps.py`) go under git-ignored `assets/local/audio/footsteps/` with
the same names, and are never committed. The bank's folders put `assets/local/audio` first: a surface
the local folder has is played from there only. Which files a machine has never changes the schedule:
a sound holds its voice for the bank's declared `holdMs`, not the file's length.
