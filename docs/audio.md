# OHAO Audio API

Parent: docs/API.md | Search: docs/INDEX.md

Backend: miniaudio (WAV/MP3/FLAC, 3D spatialization). Bundled CC0 sounds in
`godot_editor/project/sounds/{sfx,music,ambient}/`.

---

## #quick-start

```gdscript
# Simplest — use Ohao autoload helpers
Ohao.play_sfx("res://sounds/sfx/explosion.wav")
Ohao.play_music("res://sounds/music/theme.wav")
Ohao.play_sfx_at("res://sounds/sfx/impact.wav", hit_position)

var handle = Ohao.play_sfx("footstep.wav", 0.8)  # volume 0..1
Ohao.stop_sound(handle)
```

---

## #full-api

All methods on OhaoViewport (vp):

```gdscript
# Play — returns sound handle (int) for control
var h = vp.play_sound(path, category, loop, volume)
var h = vp.play_sound_at(path, position, category, loop, volume)
# category: OhaoConst.AUDIO_SFX=0  AUDIO_MUSIC=1  AUDIO_AMBIENT=2

# Control by handle
vp.stop_sound(h)
vp.pause_sound(h)
vp.resume_sound(h)
vp.set_sound_volume(h, 0.5)
vp.set_sound_position(h, world_pos)   # update 3D position while playing

# Volume mixing
vp.set_category_volume(OhaoConst.AUDIO_SFX, 0.8)
vp.get_category_volume(OhaoConst.AUDIO_SFX)
vp.set_master_volume(1.0)
vp.get_master_volume()

# Bulk control
vp.stop_category(OhaoConst.AUDIO_MUSIC)
vp.pause_category(OhaoConst.AUDIO_SFX)
vp.resume_category(OhaoConst.AUDIO_SFX)
vp.stop_all_sounds()
```

---

## #3d-audio

3D positioning: pass a world position to `play_sound_at()` or `play_sfx_at()`.
The audio listener is synced from the C++ camera automatically every frame.

```gdscript
# Enemy footstep at enemy position
var h = Ohao.play_sfx_at("res://sounds/sfx/footstep.wav", enemy.global_position)

# Update position while playing (moving emitter)
func _process(delta):
    vp.set_sound_position(engine_sound_handle, engine_node.global_position)
```

---

## #bundled-sounds

Pre-bundled CC0 sounds (Kenney pack):
```
sounds/sfx/     — footstep_*.wav, impact_*.wav, shoot_*.wav, pickup.wav
sounds/music/   — ambient_loop.wav, action.wav
sounds/ambient/ — wind.wav, crowd.wav, rain.wav
```

---

## #notes

- WAV format recommended (OGG requires stb_vorbis — not built in by default)
- Resolve `res://` paths before passing to C++ audio system
- Volume = base_vol × category_vol × master_vol
- Audio listener auto-syncs from C++ camera regardless of input mode
