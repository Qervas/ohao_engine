# Combat sounds

OHAO's own sounds for the combat slice (sub-project 7, `docs/specs/2026-10-27-combat-design.md`,
"Presentation"): `fire`, `enemy_fire`, `impact_default`, `impact_concrete`, `impact_metal`,
`impact_wood`, `hurt`, `death`, `reload`, `empty`.

**Source:** synthesized by `tools/audio/make_weapons.py` (noise and decaying partials, fixed seeds;
it rebuilds them byte for byte). **License:** MIT, as the repository. No recorded or third-party
audio.

The window host plays them from the combat log (`runtime/combat_sounds.hpp`); `ohao_game --mute`
silences them.
