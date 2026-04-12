# OHAO Scene API

Parent: docs/API.md | Search: docs/INDEX.md

---

## #builder

The scene builder is the primary way to create scenes. Pass a dict to `build()`.

```gdscript
var vp = Ohao.viewport(self)

Ohao.scene().build(vp, {
    "template":  "arena",       # arena | corridor | outdoor
    "rendering": "horror",      # horror | cyberpunk | bright | cinematic | fps_action | minimal
    "settings_panel": false,    # true → spawns F2-toggled settings UI

    "objects": [
        # type: cube | sphere | plane | cylinder
        {"type": "cube",   "name": "Box",      "pos": Vector3(0, 1, 0),
         "scale": Vector3(1,1,1), "color": Color.RED},
        {"type": "sphere", "name": "Ball",     "pos": Vector3(2, 1, 0),
         "scale": Vector3(0.5, 0.5, 0.5)},
        {"type": "plane",  "name": "Floor",    "pos": Vector3.ZERO,
         "scale": Vector3(10, 1, 10), "color": Color(0.3, 0.3, 0.3)},

        # Optional: material
        {"type": "cube", "name": "Wall", "pos": Vector3(5, 0, 0),
         "material": "stone"},              # named preset
        {"type": "cube", "name": "Dirt", "pos": Vector3(-5, 0, 0),
         "material": "ai:muddy ground"},   # AI-generated texture
    ],

    "lights": [
        # type: directional | point
        {"type": "directional", "name": "Sun",
         "pos": Vector3(0,10,0), "dir": Vector3(-1,-1,-1),
         "color": Color.WHITE, "intensity": 1.0},
        {"type": "point", "name": "Lamp",
         "pos": Vector3(3, 4, 0),
         "color": Color(1.0, 0.9, 0.7), "intensity": 2.0, "range": 15.0},
    ],
})
```

---

## #sync

```gdscript
# Clear everything
vp.clear_scene()

# Manual add (without builder)
vp.add_cube("Box", pos, rot_deg, scale, color)
vp.add_sphere("Ball", pos, rot_deg, scale, color)
vp.add_plane("Floor", pos, rot_deg, scale, color)
vp.add_cylinder("Pillar", pos, rot_deg, scale, color)
vp.add_directional_light("Sun", pos, direction, color, intensity)
vp.add_point_light("Lamp", pos, color, intensity, range)

# Must call after manual adds
vp.finish_sync()

# Import from file
vp.import_model("res://assets/models/my_mesh.obj")

# Sync Godot scene tree → OHAO (for MeshInstance3D nodes)
vp.sync_from_godot(get_tree().root)
```

---

## #actors

```gdscript
# Query
vp.has_actor("Box")                   # bool
vp.remove_actor("Box")

# Transform (by actor name)
vp.set_actor_position("Box", Vector3(0, 2, 0))
vp.set_actor_rotation("Box", Vector3(0, 45, 0))  # euler degrees
vp.set_actor_scale("Box",    Vector3(2, 2, 2))
```

---

## #mesh-instance

`OhaoMeshInstance` — a Godot Node3D that auto-creates/syncs/destroys its OHAO actor.
Add as a child node; it manages itself.

```gdscript
# Factory (returns OhaoMeshInstance node)
var mesh = Ohao.make_mesh(OhaoConst.MESH_CUBE, Color.BLUE,
                          Vector3(1,1,1), 0.0, 0.8)
add_child(mesh)   # auto-creates OHAO actor on _ready()

# Or configure manually
var visual := OhaoMeshInstance.new()
visual.mesh_type = OhaoConst.MESH_SPHERE
visual.mesh_color = Color(0.8, 0.2, 0.2)
visual.mesh_scale = Vector3(0.5, 1.8, 0.5)
visual.metallic   = 0.0
visual.roughness  = 0.5
add_child(visual)

# Queries
visual.is_synced()
visual.get_resolved_actor_name()

# Mesh types (OhaoConst.MESH_*)
# CUBE=0  SPHERE=1  CYLINDER=2  PLANE=3
```

Actor name = parent node name by default (override with `actor_name_override`).
Syncs transform every `_process()`. Removes actor on `queue_free()`.

---

## #camera

```gdscript
# Mode
vp.set_camera_mode(OhaoConst.CAMERA_FPS)    # 0
vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)  # 1

# Controls
vp.set_mouse_sensitivity(0.1)
vp.set_move_speed(5.0)

# Position / rotation
vp.set_camera_position(Vector3(0, 2, 5))
vp.get_camera_position()   # → Vector3
vp.set_camera_rotation_deg(pitch, yaw)
vp.get_camera_forward()    # → Vector3 unit direction
vp.focus_on_scene()        # auto-fit all actors in view
```

FPS camera sync from GDScript (required for player-controlled FPS):
```gdscript
# In _physics_process — sync after moving player node
vp.set_camera_position(player.global_position + Vector3(0, 1.6, 0))
vp.set_camera_rotation_deg(camera_node.rotation.x * 180/PI, yaw_deg)
```

Yaw conversion: `cpp_yaw = -rad_to_deg(godot_yaw) - 90.0`
