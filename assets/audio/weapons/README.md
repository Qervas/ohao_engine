# Combat sounds

OHAO's own sounds for the combat slice (sub-project 7, `docs/specs/2026-10-27-combat-design.md`,
"Presentation"): `fire`, `enemy_fire`, `impact_default`, `impact_concrete`, `impact_metal`,
`impact_wood`, `death`, `reload`, `empty`; and the handling sounds of sub-project 10's clip
events: `mag_out`, `mag_in`, `bolt`, `body_fall`. (The player's `hurt` cue is gone: it masked the
enemy shot that caused it, Frank's ruling 22 in `docs/specs/2026-10-28-audio-design.md`.)

**Source:** synthesized by `tools/audio/make_weapons.py` (noise and decaying partials, fixed seeds;
it rebuilds them byte for byte). **License:** MIT, as the repository. No recorded or third-party
audio.

The audio schedule (`ecs/audio_schedule.hpp`) names them from the combat and animation logs, by the
slice's bank (`assets/audio/slice.audio.json`), and a window's player (`runtime/audio_player.hpp`)
plays them; `ohao_game --mute` silences them. A recording of your own under git-ignored
`assets/local/audio/weapons/` with the same name plays instead.
