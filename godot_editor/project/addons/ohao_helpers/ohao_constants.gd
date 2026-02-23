class_name OhaoConst
## Named constants for OHAO Engine. Replaces magic numbers everywhere.

# Body types (OhaoPhysicsBody)
const BODY_DYNAMIC := 0
const BODY_STATIC := 1
const BODY_KINEMATIC := 2

# Shape types (OhaoPhysicsBody)
const SHAPE_BOX := 0
const SHAPE_SPHERE := 1
const SHAPE_CAPSULE := 2
const SHAPE_MESH := 3

# Camera modes (OhaoViewport)
const CAMERA_FPS := 0
const CAMERA_ORBIT := 1

# Render modes (OhaoViewport)
const RENDER_FORWARD := 0
const RENDER_DEFERRED := 1

# Tonemap operators (OhaoViewport)
const TONEMAP_ACES := 0
const TONEMAP_REINHARD := 1
const TONEMAP_UNCHARTED2 := 2
const TONEMAP_NEUTRAL := 3

# Particle types (OhaoViewport)
const PARTICLE_MUZZLE_FLASH := 0
const PARTICLE_IMPACT_SPARK := 1
const PARTICLE_EXPLOSION := 2
const PARTICLE_SMOKE := 3

# Gizmo modes (OhaoViewport)
const GIZMO_TRANSLATE := 0
const GIZMO_ROTATE := 1
const GIZMO_SCALE := 2

# Input modes (OhaoViewport)
const INPUT_EDITOR := 0
const INPUT_GAME := 1

# Audio categories (OhaoViewport)
const AUDIO_SFX := 0
const AUDIO_MUSIC := 1
const AUDIO_AMBIENT := 2

# Bundled sound effect paths (relative to res://sounds/)
const SFX_GUNSHOT := "sfx/gunshot.wav"
const SFX_EXPLOSION := "sfx/explosion.wav"
const SFX_IMPACT := "sfx/impact.wav"
const SFX_FOOTSTEP := "sfx/footstep.wav"
const SFX_RELOAD := "sfx/reload.wav"
const SFX_CLICK := "sfx/click.wav"
const SFX_PICKUP := "sfx/pickup.wav"
const SFX_HURT := "sfx/hurt.wav"
const SFX_JUMP := "sfx/jump.wav"
const SFX_LAND := "sfx/land.wav"
const SFX_DOOR_OPEN := "sfx/door_open.wav"
const SFX_DOOR_CLOSE := "sfx/door_close.wav"
const SFX_SWITCH := "sfx/switch.wav"
const SFX_WHOOSH := "sfx/whoosh.wav"
const SFX_RICOCHET := "sfx/ricochet.wav"
const MUSIC_AMBIENT_DARK := "music/ambient_dark.wav"
const MUSIC_AMBIENT_CALM := "music/ambient_calm.wav"
const MUSIC_COMBAT := "music/combat.wav"
const AMBIENT_WIND := "ambient/wind.wav"
const AMBIENT_RAIN := "ambient/rain.wav"
const AMBIENT_DRIP := "ambient/drip.wav"
const AMBIENT_HUM := "ambient/hum_buzz.wav"
const AMBIENT_CRICKETS := "ambient/crickets.wav"
