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

# Mesh types (OhaoMeshInstance)
const MESH_CUBE := 0
const MESH_SPHERE := 1
const MESH_CYLINDER := 2
const MESH_PLANE := 3

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

# Effect stability (OhaoSettings)
const EFFECT_STABLE := 0
const EFFECT_EXPERIMENTAL := 1

# Collision layers (OhaoViewport)
const LAYER_DEFAULT := 0
const LAYER_STATIC := 1
const LAYER_DYNAMIC := 2
const LAYER_KINEMATIC := 3
const LAYER_CHARACTER := 4
const LAYER_TRIGGER := 5
const LAYER_DEBRIS := 6
const LAYER_PROJECTILE := 7
const LAYER_VEHICLE := 8
const LAYER_RAGDOLL := 9
const LAYER_TERRAIN := 10
const LAYER_WATER := 11
const LAYER_USER_0 := 12
const LAYER_USER_1 := 13
const LAYER_USER_2 := 14
const LAYER_USER_3 := 15
const LAYER_ALL := 0xFFFF

# Constraint types
const CONSTRAINT_FIXED := 0
const CONSTRAINT_POINT := 1
const CONSTRAINT_HINGE := 2
const CONSTRAINT_SLIDER := 3
const CONSTRAINT_CONE := 4
const CONSTRAINT_DISTANCE := 5
const CONSTRAINT_SIX_DOF := 6

# Character ground state
const GROUND_ON_GROUND := 0
const GROUND_ON_STEEP := 1
const GROUND_NOT_SUPPORTED := 2
const GROUND_IN_AIR := 3

# Radial impulse falloff types (apply_radial_impulse)
const FALLOFF_LINEAR := 0      # strength * (1 - dist/radius)
const FALLOFF_QUADRATIC := 1   # strength * (1 - dist/radius)^2
const FALLOFF_CONSTANT := 2    # full strength regardless of distance

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
