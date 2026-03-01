# OHAO GDScript Layer

Parent: docs/API.md | Search: docs/INDEX.md

The GDScript layer sits above the C++ GDExtension. Always prefer these helpers
over calling OhaoViewport methods directly — they handle viewport lookup,
type conversion, and common patterns.

---

## #autoload

`Ohao` is a global autoload singleton. Access from any GDScript:

```gdscript
# Get the viewport (cached, searches tree automatically)
var vp = Ohao.viewport(self)   # pass any node in the scene tree

# Subsystem accessors
Ohao.scene()      # → OhaoSceneBuilder
Ohao.settings()   # → OhaoSettings (effect catalog)

# Factories
Ohao.make_mesh(mesh_type, color, scale, metallic, roughness)  # → OhaoMeshInstance
Ohao.make_physics_body(body_type, shape_type, mass)           # → OhaoPhysicsBody

# Audio shortcuts
Ohao.play_sfx(file, vol)
Ohao.play_sfx_at(file, pos, vol)
Ohao.play_music(file, vol)
Ohao.play_ambient_at(file, pos, vol)
Ohao.stop_sound(handle)

# UI
Ohao.create_settings_panel(root_node)  # spawns F2-toggled settings UI
Ohao.enter_game_mode()
Ohao.exit_game_mode()
```

---

## #constants

`OhaoConst` — never use magic numbers.

```gdscript
# Body types
OhaoConst.BODY_DYNAMIC     # 0
OhaoConst.BODY_STATIC      # 1
OhaoConst.BODY_KINEMATIC   # 2

# Shape types
OhaoConst.SHAPE_BOX        # 0
OhaoConst.SHAPE_SPHERE     # 1
OhaoConst.SHAPE_CAPSULE    # 2
OhaoConst.SHAPE_MESH       # 3

# Mesh types (for OhaoMeshInstance)
OhaoConst.MESH_CUBE        # 0
OhaoConst.MESH_SPHERE      # 1
OhaoConst.MESH_CYLINDER    # 2
OhaoConst.MESH_PLANE       # 3

# Camera
OhaoConst.CAMERA_FPS       # 0
OhaoConst.CAMERA_ORBIT     # 1

# Tonemapping
OhaoConst.TONEMAP_ACES         # 0
OhaoConst.TONEMAP_REINHARD     # 1
OhaoConst.TONEMAP_UNCHARTED2   # 2
OhaoConst.TONEMAP_NEUTRAL      # 3

# Audio categories
OhaoConst.AUDIO_SFX        # 0
OhaoConst.AUDIO_MUSIC      # 1
OhaoConst.AUDIO_AMBIENT    # 2

# Input mode
OhaoConst.INPUT_EDITOR     # 0
OhaoConst.INPUT_GAME       # 1

# Collision layers (use with layerMask params)
OhaoConst.LAYER_DEFAULT    # 0
OhaoConst.LAYER_STATIC     # 1
OhaoConst.LAYER_DYNAMIC    # 2
OhaoConst.LAYER_KINEMATIC  # 3
OhaoConst.LAYER_CHARACTER  # 4
OhaoConst.LAYER_TRIGGER    # 5
OhaoConst.LAYER_DEBRIS     # 6
OhaoConst.LAYER_PROJECTILE # 7
OhaoConst.LAYER_VEHICLE    # 8
OhaoConst.LAYER_RAGDOLL    # 9
OhaoConst.LAYER_TERRAIN    # 10
OhaoConst.LAYER_WATER      # 11
OhaoConst.LAYER_ALL        # 0xFFFF

# Constraint types
OhaoConst.CONSTRAINT_FIXED    # 0
OhaoConst.CONSTRAINT_POINT    # 1
OhaoConst.CONSTRAINT_HINGE    # 2
OhaoConst.CONSTRAINT_SLIDER   # 3
OhaoConst.CONSTRAINT_CONE     # 4
OhaoConst.CONSTRAINT_DISTANCE # 5
OhaoConst.CONSTRAINT_SIX_DOF  # 6

# Ground states (from get_character_state)
OhaoConst.GROUND_ON_GROUND     # 0
OhaoConst.GROUND_ON_STEEP      # 1
OhaoConst.GROUND_NOT_SUPPORTED # 2
OhaoConst.GROUND_IN_AIR        # 3

# Particle types
OhaoConst.PARTICLE_MUZZLE_FLASH  # 0
OhaoConst.PARTICLE_IMPACT_SPARK  # 1
OhaoConst.PARTICLE_EXPLOSION     # 2
OhaoConst.PARTICLE_SMOKE         # 3
```

---

## #physics-body

```gdscript
# One-liner
var body = Ohao.make_physics_body(OhaoConst.BODY_DYNAMIC, OhaoConst.SHAPE_BOX, 10.0)
add_child(body)
body.add_to_physics_world()

# Named factory helpers (OhaoPhysicsHelpers)
var player  = OhaoPhysicsHelpers.player_body(80.0)   # kinematic capsule
var crate   = OhaoPhysicsHelpers.dynamic_box(mass, size)
var floor   = OhaoPhysicsHelpers.static_box(size)
```

See docs/physics.md#bodies for full OhaoPhysicsBody method list.

---

## #mesh-helpers

```gdscript
# Self-managing visual (auto-creates + syncs + cleans up actor)
var vis = Ohao.make_mesh(OhaoConst.MESH_CUBE, Color.RED, Vector3(1,1,1), 0.0, 0.8)
parent_node.add_child(vis)   # actor name = parent node name

# Manual
var vis := OhaoMeshInstance.new()
vis.mesh_type = OhaoConst.MESH_SPHERE
vis.mesh_color = Color.BLUE
vis.mesh_scale = Vector3(0.3, 0.3, 0.3)
vis.actor_name_override = "my_ball"
add_child(vis)
```

---

## #input-mode

```gdscript
# Switch between editor (mouse visible, gizmos on) and game (captured mouse)
Ohao.enter_game_mode()          # equivalent to vp.set_input_mode(OhaoConst.INPUT_GAME)
Ohao.exit_game_mode()           # equivalent to vp.set_input_mode(OhaoConst.INPUT_EDITOR)
vp.set_input_mode(OhaoConst.INPUT_GAME)
vp.get_input_mode()             # → int
```

In GAME mode: editor overlays hidden, gizmo off, grid off, selection cleared.
In EDITOR mode: full editor UI active.

---

## #fps-pattern

Minimal FPS player skeleton:

```gdscript
extends Node

var vp: OhaoViewport
var _yaw   := 0.0
var _pitch := 0.0
var _speed := 5.0

func _ready():
    vp = Ohao.viewport(self)
    Ohao.enter_game_mode()
    Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func _unhandled_input(event):
    if event is InputEventMouseMotion:
        _yaw   -= event.relative.x * 0.002
        _pitch -= event.relative.y * 0.002
        _pitch  = clamp(_pitch, -1.5, 1.5)

func _physics_process(delta):
    # Build movement vector
    var dir = Vector3.ZERO
    if Input.is_action_pressed("ui_up"):    dir.z -= 1
    if Input.is_action_pressed("ui_down"):  dir.z += 1
    if Input.is_action_pressed("ui_left"):  dir.x -= 1
    if Input.is_action_pressed("ui_right"): dir.x += 1
    if dir.length() > 0:
        dir = (Transform3D(Basis(Vector3.UP, _yaw)) * dir).normalized()

    global_position += dir * _speed * delta

    # Sync C++ camera
    vp.set_camera_position(global_position + Vector3(0, 1.6, 0))
    var cpp_yaw = -rad_to_deg(_yaw) - 90.0
    vp.set_camera_rotation_deg(rad_to_deg(_pitch), cpp_yaw)
```

---

## #particles

```gdscript
vp.spawn_particles(position, OhaoConst.PARTICLE_EXPLOSION)
vp.spawn_particles(position, OhaoConst.PARTICLE_MUZZLE_FLASH)
vp.spawn_particles(position, OhaoConst.PARTICLE_IMPACT_SPARK)
vp.spawn_particles(position, OhaoConst.PARTICLE_SMOKE)

# With direction (e.g. muzzle facing forward)
vp.spawn_particles_directed(position, OhaoConst.PARTICLE_MUZZLE_FLASH, forward_dir)
```

---

## #selection

```gdscript
# Editor mode only — click to select
var actor_name = vp.pick_object_at(get_viewport().get_mouse_position())
var selected   = vp.get_selected_actor_name()
```

---

## #settings-panel

```gdscript
# Spawn runtime settings UI (F2 to toggle)
var panel = Ohao.create_settings_panel(get_tree().root)

# Or via scene builder
Ohao.scene().build(vp, {"template": "arena", "settings_panel": true})
```
