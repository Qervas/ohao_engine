# OHAO Physics API

Parent: docs/API.md | Search: docs/INDEX.md

---

## #simulation

```gdscript
vp.play_physics()               # start simulation
vp.pause_physics()              # freeze
vp.step_physics()               # single step (while paused)
vp.stop_physics()               # reset to t=0
vp.set_physics_speed(0.5)       # slow motion; 1.0 = normal
```

---

## #bodies

Physics bodies are created through GDScript helpers, not directly on OhaoViewport.

```gdscript
# One-liner factory (most common)
var body = Ohao.make_physics_body(OhaoConst.BODY_DYNAMIC, OhaoConst.SHAPE_BOX, 10.0)

# Or use OhaoPhysicsHelpers
var player = OhaoPhysicsHelpers.player_body(80.0)   # capsule, 80 kg
var wall   = OhaoPhysicsHelpers.static_box(size)
```

Body types (OhaoConst.BODY_*):  DYNAMIC=0  STATIC=1  KINEMATIC=2
Shape types (OhaoConst.SHAPE_*): BOX=0  SPHERE=1  CAPSULE=2  MESH=3

OhaoPhysicsBody methods (on the body node):
```gdscript
body.set_body_type(OhaoConst.BODY_KINEMATIC)
body.set_shape_type(OhaoConst.SHAPE_CAPSULE)
body.set_mass(80.0)
body.set_friction(0.5)
body.set_restitution(0.3)
body.set_gravity_enabled(true)
body.set_linear_velocity(Vector3(0, 5, 0))
body.set_angular_velocity(Vector3.ZERO)
body.apply_force(Vector3(0, 100, 0), Vector3.ZERO)
body.apply_impulse(Vector3(0, 50, 0), Vector3.ZERO)
body.apply_torque(Vector3(0, 10, 0))
body.add_to_physics_world()
body.remove_from_physics_world()
```

Actor transform (by name, on OhaoViewport):
```gdscript
vp.set_actor_position("Box", Vector3(0, 2, 0))
vp.set_actor_rotation("Box", Vector3(0, 45, 0))   # degrees
vp.set_actor_scale("Box", Vector3(2, 2, 2))
```

---

## #layers

16 named collision layers. Default objects self-assign based on body type.

```gdscript
# Prevent two layers from colliding
vp.set_layer_collision(OhaoConst.LAYER_PLAYER, OhaoConst.LAYER_TRIGGER, false)

# Named layers (OhaoConst.LAYER_*)
# DEFAULT=0  STATIC=1  DYNAMIC=2  KINEMATIC=3  CHARACTER=4
# TRIGGER=5  DEBRIS=6  PROJECTILE=7  VEHICLE=8  RAGDOLL=9
# TERRAIN=10 WATER=11  USER_0..3=12-15  ALL=0xFFFF
```

---

## #queries

```gdscript
# Single closest hit
var hit = vp.cast_ray(origin, direction, 100.0, OhaoConst.LAYER_ALL)
if hit.hit:
    print(hit.position, hit.normal, hit.body_handle, hit.fraction)

# All hits along ray
var hits = vp.cast_ray_all(origin, direction, 100.0, OhaoConst.LAYER_ALL)

# Overlap shapes
var bodies = vp.overlap_sphere(center, radius, OhaoConst.LAYER_ALL)
var bodies = vp.overlap_box(center, half_extents, rotation_deg, OhaoConst.LAYER_ALL)
```

Typical raycast from camera:
```gdscript
var origin = vp.get_camera_position()
var dir    = vp.get_camera_forward()
var hit    = vp.cast_ray(origin, dir, 50.0, OhaoConst.LAYER_DYNAMIC)
```

---

## #constraints

All create methods return a handle (int). Returns -1 on failure.

```gdscript
# Fixed — weld two bodies together
var h = vp.create_constraint_fixed(body1, body2, anchor_world)

# Hinge — rotates around one axis (door, wheel)
var h = vp.create_constraint_hinge(body1, body2, anchor, axis,
                                    limit_min_deg, limit_max_deg)

# Slider — slides along one axis (piston, drawer)
var h = vp.create_constraint_slider(body1, body2, axis, limit_min, limit_max)

# Point — ball-and-socket (chain link, rope node)
var h = vp.create_constraint_point(body1, body2, anchor1, anchor2)

# Distance — keep bodies within min..max distance
var h = vp.create_constraint_distance(body1, body2, anchor1, anchor2,
                                       min_dist, max_dist)

# Cone — twist axis with cone angle limit (shoulder, neck)
var h = vp.create_constraint_cone(body1, body2, anchor, twist_axis,
                                   half_cone_angle_rad)

# Manage
vp.destroy_constraint(h)
vp.set_constraint_enabled(h, false)

# Motor — spin a hinge or slide a slider
vp.set_constraint_motor(h, true, speed_rad_per_s, max_force)

# Limits — adjust after creation
vp.set_constraint_limits(h, min_val, max_val)
```

Ragdoll skeleton example:
```gdscript
var spine = vp.create_constraint_cone(torso, pelvis, mid_pt, Vector3.UP, 0.3)
var l_shoulder = vp.create_constraint_cone(l_arm, torso, l_pt, Vector3.RIGHT, 1.0)
var r_shoulder = vp.create_constraint_cone(r_arm, torso, r_pt, Vector3.LEFT, 1.0)
var l_knee = vp.create_constraint_hinge(l_shin, l_thigh, knee_pt, Vector3.RIGHT, 0, 2.6)
```

---

## #breaking

Set a force/torque threshold (N·s impulse per step). Exceeded → auto-destroyed.

```gdscript
# Set thresholds after creation (0 = unbreakable)
vp.set_constraint_breaking(h, max_force_ns, max_torque_ns)

# Poll each physics frame for what snapped
func _physics_process(_delta):
    var broken = vp.get_and_clear_broken_constraints()
    for handle in broken:
        emit_signal("constraint_snapped", handle)
```

Typical values: weak wood joint ≈ 50, steel bolt ≈ 5000, unbreakable = 0.

---

## #grab

Mouse-spring grab: creates a kinematic ghost + POINT constraint, drag it each frame.

```gdscript
var _grab_token := -1

func _unhandled_input(event):
    if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
        if event.pressed:
            var origin = vp.get_camera_position()
            var dir    = vp.get_camera_forward()
            var hit    = vp.cast_ray(origin, dir, 20.0, OhaoConst.LAYER_DYNAMIC)
            if hit.hit:
                _grab_token = vp.grab_body(hit.body_handle, hit.position)
        else:
            if _grab_token >= 0:
                vp.release_grab(_grab_token)
                _grab_token = -1

func _physics_process(_delta):
    if _grab_token >= 0:
        # Move grab point to fixed distance in front of camera
        var target = vp.get_camera_position() + vp.get_camera_forward() * 4.0
        vp.move_grab(_grab_token, target)

# Throw with velocity
func throw():
    if _grab_token >= 0:
        var vel = vp.get_camera_forward() * 15.0
        vp.throw_grab(_grab_token, vel)
        _grab_token = -1
```

API:
```gdscript
var token = vp.grab_body(body_handle, world_pos)  # returns token int
vp.move_grab(token, new_world_pos)                # call every frame
vp.release_grab(token)                            # drop
vp.throw_grab(token, velocity_vector)             # fling + release
```

---

## #character

Virtual character controller — handles stairs, slopes, step height.

```gdscript
# Create
var ch = vp.create_character(
    position,          # Vector3 spawn pos
    0.3,               # capsule radius
    1.8,               # capsule height
    50.0,              # max slope degrees
    80.0               # mass kg
)

# Each physics frame
var gravity = Vector3(0, -9.81, 0)
var move_input = Vector3(x, 0, z)   # from WASD, normalized
vp.update_character(ch, delta, gravity, move_input)

# Query state
var state = vp.get_character_state(ch)
# state.position       Vector3
# state.velocity       Vector3
# state.ground_normal  Vector3
# state.ground_state   int  (OhaoConst.GROUND_*)
# state.ground_body    int  body handle or -1

# Ground states (OhaoConst.GROUND_*)
# ON_GROUND=0  ON_STEEP=1  NOT_SUPPORTED=2  IN_AIR=3

# Move
vp.set_character_position(ch, pos)
vp.set_character_rotation(ch, rotation_deg)
vp.set_character_velocity(ch, vel)

vp.destroy_character(ch)
```

---

## #contacts

Contact events require a C++ IContactListener. From GDScript, use overlap queries
or the physics backend tests as reference. Direct GDScript contact callbacks are
not yet exposed — use `cast_ray_all` + frame comparison as a workaround.
