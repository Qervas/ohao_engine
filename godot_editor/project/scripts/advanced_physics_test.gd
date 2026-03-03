extends Control
## Advanced Physics Test Suite
##
## 8 demos exercising every Jolt Physics feature wired through OhaoViewport.
##
## Controls:
##   1-8   - Switch demo
##   P     - Play/Pause physics
##   R     - Reset current demo
##   TAB   - Toggle editor/game camera
##   Demo-specific keys shown in HUD

@onready var vp: OhaoViewport = $OhaoViewport

var current_demo: int = 0
var current_demo_name: String = ""
var physics_paused: bool = true

# Tracking
var active_actors: Array[String] = []
var passive_actors: Array[String] = []
var visual_actors: Array[String] = []

# Demo-specific state
var _hinge_handle: int = -1
var _hinge_motor_on: bool = false
var _hinge_timer: float = 0.0

var _slider_handle: int = -1
var _slider_motor_on: bool = true
var _slider_dir: float = 1.0
var _slider_timer: float = 0.0

var _pendulum_handles: Array[int] = []

var _constraint_handles: Array[int] = []

var _layer_collision_on: bool = true

var _ray_hit_pos: Vector3 = Vector3.ZERO
var _ray_hit_normal: Vector3 = Vector3.ZERO
var _ray_hit_handle: int = -1
var _ray_angle: float = 0.0
var _overlap_count: int = 0
var _ray_all_hits: Array = []

var _char_handle: int = -1
var _char_yaw: float = 0.0
var _char_pitch: float = 0.0
var _char_velocity: Vector3 = Vector3.ZERO
var _char_ground_state: int = 3  # IN_AIR
var _char_jump_requested: bool = false

var _projectile_count: int = 0

var _log_lines: Array[String] = []

func _ready() -> void:
	if not vp:
		push_error("OhaoViewport not found!")
		return
	call_deferred("_init_scene")

func _init_scene() -> void:
	vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)
	vp.set_camera_position(Vector3(0, 8, 16))
	vp.set_camera_rotation_deg(-20, 0)

	_setup_rendering()
	_demo_hinge_door()
	_log("Press 1-8 for demos, P to play physics.")

func _setup_rendering() -> void:
	vp.set_ssao_enabled(true)
	vp.set_bloom_enabled(true)
	vp.set_bloom_intensity(0.3)
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)

# =============================================================================
# FACTORIES
# =============================================================================

func _make_static_box(aname: String, pos: Vector3, scl: Vector3, color: Color,
		restitution: float = 0.0, friction: float = 0.8) -> void:
	vp.add_cube(aname, pos, Vector3.ZERO, scl, color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_STATIC)
	vp.set_actor_mass(aname, 0.0)
	vp.set_actor_friction(aname, friction)
	vp.set_actor_restitution(aname, restitution)
	vp.sync_actor_physics_shape(aname)
	passive_actors.append(aname)

func _make_dynamic_box(aname: String, pos: Vector3, scl: Vector3, color: Color,
		mass: float = 2.0, restitution: float = 0.2, friction: float = 0.5) -> void:
	vp.add_cube(aname, pos, Vector3.ZERO, scl, color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_DYNAMIC)
	vp.set_actor_mass(aname, mass)
	vp.set_actor_friction(aname, friction)
	vp.set_actor_restitution(aname, restitution)
	vp.sync_actor_physics_shape(aname)
	active_actors.append(aname)

func _make_dynamic_sphere(aname: String, pos: Vector3, radius: float, color: Color,
		mass: float = 1.0, restitution: float = 0.6, friction: float = 0.3) -> void:
	var d := radius * 2.0
	vp.add_sphere(aname, pos, Vector3.ZERO, Vector3(d, d, d), color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_DYNAMIC)
	vp.set_actor_mass(aname, mass)
	vp.set_actor_friction(aname, friction)
	vp.set_actor_restitution(aname, restitution)
	vp.sync_actor_physics_shape(aname)
	active_actors.append(aname)

func _make_static_sphere(aname: String, pos: Vector3, radius: float, color: Color) -> void:
	var d := radius * 2.0
	vp.add_sphere(aname, pos, Vector3.ZERO, Vector3(d, d, d), color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_STATIC)
	vp.set_actor_mass(aname, 0.0)
	vp.sync_actor_physics_shape(aname)
	passive_actors.append(aname)

func _make_kinematic_box(aname: String, pos: Vector3, scl: Vector3, color: Color) -> void:
	vp.add_cube(aname, pos, Vector3.ZERO, scl, color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_KINEMATIC)
	vp.set_actor_mass(aname, 0.0)
	vp.sync_actor_physics_shape(aname)
	passive_actors.append(aname)

func _make_visual(aname: String, mesh_type: int, pos: Vector3,
		scl: Vector3, color: Color) -> void:
	match mesh_type:
		OhaoConst.MESH_CUBE:
			vp.add_cube(aname, pos, Vector3.ZERO, scl, color)
		OhaoConst.MESH_SPHERE:
			vp.add_sphere(aname, pos, Vector3.ZERO, scl, color)
		OhaoConst.MESH_CYLINDER:
			vp.add_cylinder(aname, pos, Vector3.ZERO, scl, color)
		OhaoConst.MESH_PLANE:
			vp.add_plane(aname, pos, Vector3.ZERO, scl, color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_STATIC)
	vp.set_actor_mass(aname, 0.0)
	vp.set_actor_gravity_enabled(aname, false)
	vp.sync_actor_physics_shape(aname)
	visual_actors.append(aname)

# =============================================================================
# SCENE MANAGEMENT
# =============================================================================

func _reset_scene() -> void:
	# Destroy any active constraints
	for ch in _constraint_handles:
		if ch >= 0:
			vp.destroy_constraint(ch)
	_constraint_handles.clear()

	if _hinge_handle >= 0:
		vp.destroy_constraint(_hinge_handle)
		_hinge_handle = -1

	if _slider_handle >= 0:
		vp.destroy_constraint(_slider_handle)
		_slider_handle = -1

	for ph in _pendulum_handles:
		if ph >= 0:
			vp.destroy_constraint(ph)
	_pendulum_handles.clear()

	# Destroy character
	if _char_handle >= 0:
		vp.destroy_character(_char_handle)
		_char_handle = -1

	active_actors.clear()
	passive_actors.clear()
	visual_actors.clear()
	_projectile_count = 0

	vp.clear_scene()
	# Re-add lighting
	vp.add_directional_light("Sun", Vector3(0, 20, 0),
		Vector3(-0.4, -1, -0.3), Color(1, 0.95, 0.9), 1.8)
	vp.add_point_light("Fill", Vector3(-8, 6, 8),
		Color(0.6, 0.7, 1.0), 1.5, 20.0)

func _restart_demo() -> void:
	physics_paused = true
	vp.pause_physics()
	match current_demo:
		1: _demo_hinge_door()
		2: _demo_pendulum_chain()
		3: _demo_slider_platform()
		4: _demo_constraint_showcase()
		5: _demo_ccd_projectiles()
		6: _demo_collision_layers()
		7: _demo_raycasting()
		8: _demo_character_platformer()
		_: _demo_hinge_door()
	_log("Reset. Press P to start physics.")

# =============================================================================
# DEMO 1: HINGE DOOR
# =============================================================================

func _demo_hinge_door() -> void:
	_reset_scene()
	current_demo = 1
	current_demo_name = "Hinge Door"
	_hinge_motor_on = true
	_hinge_timer = 0.0
	_log("Demo 1: Hinge Door — M toggles motor")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Door frame: two pillars + top beam
	_make_static_box("PillarL", Vector3(-1.5, 2.0, 0), Vector3(0.3, 4.0, 0.3),
		Color(0.5, 0.4, 0.3))
	_make_static_box("PillarR", Vector3(1.5, 2.0, 0), Vector3(0.3, 4.0, 0.3),
		Color(0.5, 0.4, 0.3))
	_make_static_box("TopBeam", Vector3(0, 4.15, 0), Vector3(3.3, 0.3, 0.3),
		Color(0.5, 0.4, 0.3))

	# Door panel (dynamic)
	_make_dynamic_box("Door", Vector3(0, 2.0, 0), Vector3(2.7, 3.8, 0.15),
		Color(0.6, 0.3, 0.15), 5.0, 0.1, 0.4)

	vp.finish_sync()

	# Create hinge constraint at left edge of door
	var pillar_h := vp.get_actor_body_handle("PillarL")
	var door_h := vp.get_actor_body_handle("Door")
	if pillar_h >= 0 and door_h >= 0:
		_hinge_handle = vp.create_constraint_hinge(
			pillar_h, door_h,
			Vector3(-1.35, 2.0, 0),  # anchor at left edge
			Vector3(0, 1, 0),         # vertical axis
			-90.0, 90.0              # limits: +/- 90 degrees
		)
		if _hinge_handle >= 0:
			vp.set_constraint_motor(_hinge_handle, true, 2.0, 50.0)
			_log("Hinge created (handle %d), motor ON" % _hinge_handle)
		else:
			_log("WARNING: Hinge constraint creation failed")
	else:
		_log("WARNING: Could not get body handles (pillar=%d, door=%d)" % [pillar_h, door_h])

	# Some boxes to push around
	for i in range(3):
		_make_dynamic_box("Box_%d" % i,
			Vector3(3.0 + i * 1.2, 0.5, -1.0 + i * 1.0),
			Vector3(0.8, 0.8, 0.8),
			Color(0.2, 0.5, 0.8), 1.0)

# =============================================================================
# DEMO 2: PENDULUM CHAIN
# =============================================================================

func _demo_pendulum_chain() -> void:
	_reset_scene()
	current_demo = 2
	current_demo_name = "Pendulum Chain"
	_log("Demo 2: Pendulum Chain — watch the chain swing")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Ceiling beam
	_make_static_box("Ceiling", Vector3(0, 8.0, 0), Vector3(4.0, 0.3, 0.3),
		Color(0.5, 0.5, 0.55))

	# Chain of 5 spheres connected by distance constraints
	var chain_colors: Array[Color] = [
		Color(0.9, 0.2, 0.2),
		Color(0.9, 0.5, 0.1),
		Color(0.9, 0.9, 0.2),
		Color(0.2, 0.8, 0.3),
		Color(0.2, 0.4, 0.9),
	]
	var link_spacing := 1.4
	var start_y := 7.5

	for i in range(5):
		var y := start_y - i * link_spacing
		_make_dynamic_sphere("Chain_%d" % i, Vector3(0, y, 0), 0.35,
			chain_colors[i], 2.0, 0.3, 0.2)

	vp.finish_sync()

	# Point constraint: ceiling -> first sphere
	var ceil_h := vp.get_actor_body_handle("Ceiling")
	var first_h := vp.get_actor_body_handle("Chain_0")
	if ceil_h >= 0 and first_h >= 0:
		var ch := vp.create_constraint_point(ceil_h, first_h,
			Vector3(0, 7.85, 0), Vector3(0, 0.35, 0))
		if ch >= 0:
			_pendulum_handles.append(ch)

	# Distance constraints between adjacent spheres
	for i in range(4):
		var h1 := vp.get_actor_body_handle("Chain_%d" % i)
		var h2 := vp.get_actor_body_handle("Chain_%d" % (i + 1))
		if h1 >= 0 and h2 >= 0:
			var ch := vp.create_constraint_distance(h1, h2,
				Vector3(0, -0.35, 0), Vector3(0, 0.35, 0),
				link_spacing - 0.7, link_spacing - 0.7)
			if ch >= 0:
				_pendulum_handles.append(ch)

	# Give bottom sphere an initial velocity kick
	vp.set_actor_linear_velocity("Chain_4", Vector3(8.0, 0, 0))
	_log("Chain: %d constraints created" % _pendulum_handles.size())

# =============================================================================
# DEMO 3: SLIDER PLATFORM
# =============================================================================

func _demo_slider_platform() -> void:
	_reset_scene()
	current_demo = 3
	current_demo_name = "Slider Platform"
	_slider_motor_on = true
	_slider_dir = 1.0
	_slider_timer = 0.0
	_log("Demo 3: Slider Platform — Left/Right override, M motor toggle")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Two guide pillars
	_make_static_box("GuideL", Vector3(-5.0, 1.5, 0), Vector3(0.4, 3.0, 2.0),
		Color(0.4, 0.4, 0.5))
	_make_static_box("GuideR", Vector3(5.0, 1.5, 0), Vector3(0.4, 3.0, 2.0),
		Color(0.4, 0.4, 0.5))

	# Rail visual
	_make_visual("Rail", OhaoConst.MESH_CUBE,
		Vector3(0, 0.5, 0), Vector3(10.0, 0.1, 0.5), Color(0.6, 0.6, 0.2))

	# Platform (dynamic, on slider)
	_make_dynamic_box("Platform", Vector3(0, 1.0, 0), Vector3(3.0, 0.3, 2.0),
		Color(0.3, 0.6, 0.9), 10.0, 0.1, 0.8)

	# Boxes sitting on platform
	for i in range(3):
		_make_dynamic_box("Rider_%d" % i,
			Vector3(-1.0 + i * 1.0, 1.7, 0),
			Vector3(0.6, 0.6, 0.6),
			Color(0.8, 0.3, 0.2), 1.0, 0.1, 0.6)

	vp.finish_sync()

	# Slider constraint on the platform
	var ground_h := vp.get_actor_body_handle("Ground")
	var plat_h := vp.get_actor_body_handle("Platform")
	if ground_h >= 0 and plat_h >= 0:
		_slider_handle = vp.create_constraint_slider(
			ground_h, plat_h,
			Vector3(1, 0, 0),  # X axis
			-4.0, 4.0          # travel limits
		)
		if _slider_handle >= 0:
			vp.set_constraint_motor(_slider_handle, true, 2.0, 100.0)
			_log("Slider created (handle %d), motor ON" % _slider_handle)
		else:
			_log("WARNING: Slider constraint creation failed")

# =============================================================================
# DEMO 4: CONSTRAINT SHOWCASE
# =============================================================================

func _demo_constraint_showcase() -> void:
	_reset_scene()
	current_demo = 4
	current_demo_name = "Constraint Showcase"
	_log("Demo 4: 6 Constraint Types — watch each behavior")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(24, 0.5, 10),
		Color(0.35, 0.35, 0.4))

	var stations: Array[Dictionary] = [
		{"x": -8.0,  "type": "Fixed",    "color": Color(0.8, 0.2, 0.2)},
		{"x": -4.8,  "type": "Point",    "color": Color(0.8, 0.5, 0.1)},
		{"x": -1.6,  "type": "Hinge",    "color": Color(0.8, 0.8, 0.2)},
		{"x":  1.6,  "type": "Slider",   "color": Color(0.2, 0.7, 0.3)},
		{"x":  4.8,  "type": "Distance", "color": Color(0.2, 0.4, 0.9)},
		{"x":  8.0,  "type": "Cone",     "color": Color(0.7, 0.2, 0.8)},
	]

	for s in stations:
		var x: float = s["x"]
		var stype: String = s["type"]
		var col: Color = s["color"]

		# Anchor post (static)
		_make_static_box("Anchor_%s" % stype,
			Vector3(x, 3.0, 0), Vector3(0.3, 0.3, 0.3),
			Color(0.6, 0.6, 0.6))

		# Platform label marker
		_make_visual("Label_%s" % stype, OhaoConst.MESH_CUBE,
			Vector3(x, 0.02, 2.0), Vector3(2.0, 0.04, 0.5), col)

		# Dynamic body
		_make_dynamic_box("Body_%s" % stype,
			Vector3(x, 2.0, 0), Vector3(0.8, 0.8, 0.8),
			col, 2.0, 0.3, 0.5)

	vp.finish_sync()

	# Create constraints for each station
	for s in stations:
		var stype: String = s["type"]
		var x: float = s["x"]
		var anchor_h := vp.get_actor_body_handle("Anchor_%s" % stype)
		var body_h := vp.get_actor_body_handle("Body_%s" % stype)
		if anchor_h < 0 or body_h < 0:
			_log("Skip %s: handles %d/%d" % [stype, anchor_h, body_h])
			continue

		var ch := -1
		match stype:
			"Fixed":
				ch = vp.create_constraint_fixed(anchor_h, body_h,
					Vector3(x, 2.5, 0))
			"Point":
				ch = vp.create_constraint_point(anchor_h, body_h,
					Vector3(x, 3.0, 0), Vector3(0, 0.8, 0))
			"Hinge":
				ch = vp.create_constraint_hinge(anchor_h, body_h,
					Vector3(x, 3.0, 0), Vector3(0, 0, 1),
					-90.0, 90.0)
			"Slider":
				ch = vp.create_constraint_slider(anchor_h, body_h,
					Vector3(0, 1, 0),  # vertical axis
					-2.0, 0.5)
			"Distance":
				ch = vp.create_constraint_distance(anchor_h, body_h,
					Vector3(0, 0, 0), Vector3(0, 0, 0),
					0.5, 2.0)
			"Cone":
				ch = vp.create_constraint_cone(anchor_h, body_h,
					Vector3(x, 3.0, 0), Vector3(0, -1, 0),
					0.6)

		if ch >= 0:
			_constraint_handles.append(ch)
			_log("%s constraint: handle %d" % [stype, ch])

	# Give each body a small kick
	vp.set_actor_linear_velocity("Body_Point", Vector3(3, 0, 0))
	vp.set_actor_linear_velocity("Body_Hinge", Vector3(2, 0, 0))
	vp.set_actor_linear_velocity("Body_Cone", Vector3(2, 1, 0))
	vp.set_actor_linear_velocity("Body_Distance", Vector3(3, 0, 0))

# =============================================================================
# DEMO 5: CCD & PROJECTILES
# =============================================================================

func _demo_ccd_projectiles() -> void:
	_reset_scene()
	current_demo = 5
	current_demo_name = "CCD & Projectiles"
	_projectile_count = 0
	_log("Demo 5: F=fire sphere, H=hitscan ray, C=clear projectiles")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Thin wall
	_make_static_box("ThinWall", Vector3(0, 2.0, -5), Vector3(6.0, 4.0, 0.05),
		Color(0.7, 0.7, 0.75), 0.0, 0.5)

	# Target block behind wall
	_make_dynamic_box("Target", Vector3(0, 1.0, -7), Vector3(2.0, 2.0, 2.0),
		Color(0.9, 0.2, 0.2), 5.0, 0.3)

	# Hit marker (visual-only sphere that moves to hit point)
	_make_visual("HitMarker", OhaoConst.MESH_SPHERE,
		Vector3(0, -10, 0), Vector3(0.3, 0.3, 0.3), Color(1, 1, 0))

	# Firing position indicator
	_make_visual("FirePos", OhaoConst.MESH_SPHERE,
		Vector3(0, 2.0, 5), Vector3(0.4, 0.4, 0.4), Color(0, 1, 0))

	vp.finish_sync()

# =============================================================================
# DEMO 6: COLLISION LAYERS
# =============================================================================

func _demo_collision_layers() -> void:
	_reset_scene()
	current_demo = 6
	current_demo_name = "Collision Layers"
	_layer_collision_on = true
	_log("Demo 6: Collision Layers — L toggles dynamic-static collision")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Shelf at y=3
	_make_static_box("Shelf", Vector3(0, 3.0, 0), Vector3(8.0, 0.3, 4.0),
		Color(0.4, 0.5, 0.6))

	# 6 spheres sitting on the shelf
	var ball_colors: Array[Color] = [
		Color(0.9, 0.2, 0.2), Color(0.9, 0.5, 0.1), Color(0.9, 0.9, 0.2),
		Color(0.2, 0.8, 0.3), Color(0.2, 0.4, 0.9), Color(0.7, 0.2, 0.8),
	]
	for i in range(6):
		var x := -2.5 + i * 1.0
		_make_dynamic_sphere("LayerBall_%d" % i,
			Vector3(x, 3.8, 0), 0.35,
			ball_colors[i], 1.5, 0.5, 0.3)

	# Catch platform at bottom
	_make_static_box("Catch", Vector3(0, 0.5, 0), Vector3(12.0, 0.2, 6.0),
		Color(0.3, 0.6, 0.3))

	vp.finish_sync()

# =============================================================================
# DEMO 7: RAYCASTING & OVERLAP
# =============================================================================

func _demo_raycasting() -> void:
	_reset_scene()
	current_demo = 7
	current_demo_name = "Raycasting & Overlap"
	_ray_hit_pos = Vector3.ZERO
	_ray_hit_handle = -1
	_overlap_count = 0
	_ray_angle = 0.0
	_ray_all_hits.clear()
	_log("Demo 7: Rotating ray, A=cast_ray_all, G=overlap_sphere")

	# Ground
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Scattered obstacles
	var obstacle_data: Array[Dictionary] = [
		{"pos": Vector3(3, 1, 0), "scl": Vector3(1.5, 2, 1.5), "col": Color(0.8, 0.3, 0.2)},
		{"pos": Vector3(-3, 0.75, 2), "scl": Vector3(1.0, 1.5, 1.0), "col": Color(0.2, 0.6, 0.8)},
		{"pos": Vector3(0, 1, -4), "scl": Vector3(2, 2, 1), "col": Color(0.6, 0.8, 0.2)},
		{"pos": Vector3(-4, 0.5, -2), "scl": Vector3(1, 1, 1), "col": Color(0.8, 0.2, 0.7)},
		{"pos": Vector3(4, 1.2, 3), "scl": Vector3(1.5, 2.4, 1.5), "col": Color(0.3, 0.7, 0.4)},
		{"pos": Vector3(-2, 0.8, 4), "scl": Vector3(1.2, 1.6, 1.2), "col": Color(0.9, 0.6, 0.1)},
	]
	for i in range(obstacle_data.size()):
		var d: Dictionary = obstacle_data[i]
		_make_static_box("Obs_%d" % i, d["pos"], d["scl"], d["col"])

	# Ray origin marker
	_make_visual("RayOrigin", OhaoConst.MESH_SPHERE,
		Vector3(0, 2, 0), Vector3(0.3, 0.3, 0.3), Color(0, 1, 0))

	# Hit marker
	_make_visual("RayHit", OhaoConst.MESH_SPHERE,
		Vector3(0, -10, 0), Vector3(0.25, 0.25, 0.25), Color(1, 0, 0))

	vp.finish_sync()

# =============================================================================
# DEMO 8: CHARACTER PLATFORMER
# =============================================================================

func _demo_character_platformer() -> void:
	_reset_scene()
	current_demo = 8
	current_demo_name = "Character Platformer"
	_char_yaw = 0.0
	_char_pitch = 0.0
	_char_velocity = Vector3.ZERO
	_char_ground_state = 3
	_char_jump_requested = false
	_log("Demo 8: WASD+Mouse, Space=jump, Tab=game mode")

	# Large floor
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(40, 0.5, 40),
		Color(0.35, 0.35, 0.4))

	# Gentle ramp (30 degrees)
	vp.add_cube("Ramp30", Vector3(8, 1.5, 0), Vector3(0, 0, -30), Vector3(8, 0.3, 4),
		Color(0.5, 0.6, 0.3))
	vp.set_actor_body_type("Ramp30", OhaoConst.BODY_STATIC)
	vp.set_actor_mass("Ramp30", 0.0)
	vp.set_actor_friction("Ramp30", 0.8)
	vp.sync_actor_physics_shape("Ramp30")
	passive_actors.append("Ramp30")

	# Staircase (5 steps)
	for i in range(5):
		var step_name := "Step_%d" % i
		_make_static_box(step_name,
			Vector3(-6 + i * 1.2, 0.25 + i * 0.5, 0),
			Vector3(1.2, 0.5 + i * 0.5, 3.0),
			Color(0.4, 0.4, 0.5))

	# Steep slope (60 degrees)
	vp.add_cube("SteepSlope", Vector3(0, 2.5, -8), Vector3(0, 0, -60), Vector3(6, 0.3, 4),
		Color(0.7, 0.3, 0.3))
	vp.set_actor_body_type("SteepSlope", OhaoConst.BODY_STATIC)
	vp.set_actor_mass("SteepSlope", 0.0)
	vp.set_actor_friction("SteepSlope", 0.5)
	vp.sync_actor_physics_shape("SteepSlope")
	passive_actors.append("SteepSlope")

	# Platform over gap
	_make_static_box("GapPlatL", Vector3(-12, 2.0, -4), Vector3(4, 0.3, 3),
		Color(0.3, 0.5, 0.7))
	_make_static_box("GapPlatR", Vector3(-5, 2.0, -4), Vector3(4, 0.3, 3),
		Color(0.3, 0.5, 0.7))

	# Some obstacles
	_make_static_box("Wall1", Vector3(4, 1.5, 6), Vector3(0.3, 3, 4),
		Color(0.6, 0.4, 0.3))
	_make_static_box("Wall2", Vector3(-4, 1.0, 8), Vector3(4, 2, 0.3),
		Color(0.6, 0.4, 0.3))

	vp.finish_sync()

	# Create character controller
	_char_handle = vp.create_character(
		Vector3(0, 1.0, 5),  # start position
		0.3,                  # capsule radius
		1.5,                  # capsule height
		50.0,                 # max slope degrees
		80.0                  # mass
	)
	if _char_handle >= 0:
		_log("Character created (handle %d)" % _char_handle)
	else:
		_log("WARNING: Character creation failed")

# =============================================================================
# INPUT
# =============================================================================

func _unhandled_input(event: InputEvent) -> void:
	if not vp:
		return

	# Character mouse look (demo 8 in game mode)
	if current_demo == 8 and vp.get_input_mode() == OhaoConst.INPUT_GAME:
		if event is InputEventMouseMotion:
			var mm := event as InputEventMouseMotion
			_char_yaw -= mm.relative.x * 0.003
			_char_pitch -= mm.relative.y * 0.003
			_char_pitch = clampf(_char_pitch, -1.4, 1.4)
			return

	if event is InputEventKey and event.pressed:
		match event.keycode:
			KEY_1: _demo_hinge_door()
			KEY_2: _demo_pendulum_chain()
			KEY_3: _demo_slider_platform()
			KEY_4: _demo_constraint_showcase()
			KEY_5: _demo_ccd_projectiles()
			KEY_6: _demo_collision_layers()
			KEY_7: _demo_raycasting()
			KEY_8: _demo_character_platformer()
			KEY_R: _restart_demo()
			KEY_P:
				physics_paused = !physics_paused
				if physics_paused:
					vp.pause_physics()
					_log("Physics PAUSED")
				else:
					vp.play_physics()
					_log("Physics RESUMED")
			KEY_TAB:
				if vp.get_input_mode() == OhaoConst.INPUT_GAME:
					Ohao.exit_game_mode()
					_log("Editor mode")
				else:
					Ohao.enter_game_mode()
					_log("Game mode")
			KEY_M:
				_handle_motor_toggle()
			KEY_F:
				if current_demo == 5:
					_fire_projectile()
			KEY_H:
				if current_demo == 5:
					_fire_hitscan()
			KEY_C:
				if current_demo == 5:
					_clear_projectiles()
			KEY_L:
				if current_demo == 6:
					_toggle_layer_collision()
			KEY_A:
				if current_demo == 7:
					_cast_ray_all()
			KEY_G:
				if current_demo == 7:
					_do_overlap_sphere()
			KEY_SPACE:
				if current_demo == 8:
					_char_jump_requested = true

# =============================================================================
# DEMO-SPECIFIC ACTIONS
# =============================================================================

func _handle_motor_toggle() -> void:
	if current_demo == 1 and _hinge_handle >= 0:
		_hinge_motor_on = !_hinge_motor_on
		vp.set_constraint_motor(_hinge_handle, _hinge_motor_on, 2.0, 50.0)
		_log("Hinge motor: %s" % ("ON" if _hinge_motor_on else "OFF"))
	elif current_demo == 3 and _slider_handle >= 0:
		_slider_motor_on = !_slider_motor_on
		vp.set_constraint_motor(_slider_handle, _slider_motor_on, 2.0 * _slider_dir, 100.0)
		_log("Slider motor: %s" % ("ON" if _slider_motor_on else "OFF"))

func _fire_projectile() -> void:
	var pname := "Proj_%d" % _projectile_count
	_projectile_count += 1
	_make_dynamic_sphere(pname, Vector3(0, 2.0, 5), 0.2,
		Color(1, 0.5, 0), 0.5, 0.8, 0.1)
	vp.finish_sync()
	vp.set_actor_linear_velocity(pname, Vector3(0, 0, -50))
	_log("Fired %s (v=50, may tunnel through thin wall)" % pname)

func _fire_hitscan() -> void:
	var origin := Vector3(0, 2.0, 5)
	var direction := Vector3(0, 0, -1)
	var result := vp.cast_ray(origin, direction, 50.0)
	if result.has("hit") and result["hit"]:
		var hit_pos: Vector3 = result.get("position", Vector3.ZERO)
		var hit_normal: Vector3 = result.get("normal", Vector3.UP)
		vp.set_actor_position("HitMarker", hit_pos)
		_log("Hitscan HIT at (%.1f, %.1f, %.1f)" % [hit_pos.x, hit_pos.y, hit_pos.z])
	else:
		_log("Hitscan: no hit")

func _clear_projectiles() -> void:
	for i in range(_projectile_count):
		var pname := "Proj_%d" % i
		if vp.has_actor(pname):
			vp.remove_actor(pname)
	_projectile_count = 0
	_log("Projectiles cleared")

func _toggle_layer_collision() -> void:
	_layer_collision_on = !_layer_collision_on
	vp.set_layer_collision(OhaoConst.LAYER_DYNAMIC, OhaoConst.LAYER_STATIC, _layer_collision_on)
	if _layer_collision_on:
		_log("Dynamic-Static collision: ON (spheres collide with shelf)")
	else:
		_log("Dynamic-Static collision: OFF (spheres fall through!)")

func _cast_ray_all() -> void:
	var origin := Vector3(0, 2, 0)
	var dir := Vector3(cos(_ray_angle), 0, sin(_ray_angle))
	_ray_all_hits = vp.cast_ray_all(origin, dir, 20.0)
	_log("cast_ray_all: %d hits along direction (%.2f, 0, %.2f)" % [
		_ray_all_hits.size(), dir.x, dir.z])

func _do_overlap_sphere() -> void:
	var result := vp.overlap_sphere(Vector3(0, 2, 0), 5.0)
	_overlap_count = result.size()
	_log("overlap_sphere(r=5): %d bodies found" % _overlap_count)

# =============================================================================
# PROCESS
# =============================================================================

func _process(delta: float) -> void:
	if not vp:
		return

	# Demo 1: Oscillate hinge motor direction
	if current_demo == 1 and _hinge_handle >= 0 and _hinge_motor_on and not physics_paused:
		_hinge_timer += delta
		if _hinge_timer > 3.0:
			_hinge_timer = 0.0
			var speed := 2.0 if fmod(int(_hinge_timer) / 3, 2) == 0 else -2.0
			# Alternate direction by using sin
			var dir := sin(Time.get_ticks_msec() * 0.001) * 2.0
			vp.set_constraint_motor(_hinge_handle, true, dir, 50.0)

	# Demo 3: Oscillate slider motor
	if current_demo == 3 and _slider_handle >= 0 and _slider_motor_on and not physics_paused:
		_slider_timer += delta
		if _slider_timer > 2.5:
			_slider_timer = 0.0
			_slider_dir *= -1.0
			vp.set_constraint_motor(_slider_handle, true, 2.0 * _slider_dir, 100.0)

	# Demo 3: Left/Right override
	if current_demo == 3 and _slider_handle >= 0 and not physics_paused:
		if Input.is_key_pressed(KEY_LEFT):
			vp.set_constraint_motor(_slider_handle, true, -3.0, 100.0)
			_slider_timer = 0.0
		elif Input.is_key_pressed(KEY_RIGHT):
			vp.set_constraint_motor(_slider_handle, true, 3.0, 100.0)
			_slider_timer = 0.0

	# Demo 7: Rotating ray
	if current_demo == 7 and not physics_paused:
		_ray_angle += delta * 0.8
		var origin := Vector3(0, 2, 0)
		var dir := Vector3(cos(_ray_angle), 0, sin(_ray_angle))
		var result := vp.cast_ray(origin, dir, 20.0)
		if result.has("hit") and result["hit"]:
			_ray_hit_pos = result.get("position", Vector3.ZERO)
			_ray_hit_normal = result.get("normal", Vector3.UP)
			_ray_hit_handle = result.get("body_handle", -1)
			vp.set_actor_position("RayHit", _ray_hit_pos)
		else:
			_ray_hit_pos = Vector3.ZERO
			_ray_hit_handle = -1
			vp.set_actor_position("RayHit", Vector3(0, -10, 0))

	# Demo 8: Character update
	if current_demo == 8 and _char_handle >= 0 and not physics_paused:
		_update_character(delta)

	queue_redraw()

func _update_character(delta: float) -> void:
	# Get current state
	var state := vp.get_character_state(_char_handle)
	_char_ground_state = state.get("ground_state", 3)
	var is_on_ground := (_char_ground_state == OhaoConst.GROUND_ON_GROUND)

	# Build movement input from WASD
	var move_input := Vector3.ZERO
	var forward := Vector3(-sin(_char_yaw), 0, -cos(_char_yaw))
	var right := Vector3(cos(_char_yaw), 0, -sin(_char_yaw))

	if Input.is_key_pressed(KEY_W): move_input += forward
	if Input.is_key_pressed(KEY_S): move_input -= forward
	if Input.is_key_pressed(KEY_A): move_input -= right
	if Input.is_key_pressed(KEY_D): move_input += right

	if move_input.length_squared() > 0.01:
		move_input = move_input.normalized() * 6.0  # movement speed

	# Jump
	if _char_jump_requested and is_on_ground:
		_char_velocity.y = 8.0
		_char_jump_requested = false
	elif _char_jump_requested:
		_char_jump_requested = false

	# Gravity
	var gravity := Vector3(0, -20.0, 0)

	# Update character
	vp.update_character(_char_handle, delta, gravity, move_input)

	# Sync camera from character position
	var char_pos: Vector3 = state.get("position", Vector3.ZERO)
	var eye_pos := char_pos + Vector3(0, 0.7, 0)  # eye offset
	vp.set_camera_position(eye_pos)
	var cpp_yaw_deg := -rad_to_deg(_char_yaw) - 90.0
	var cpp_pitch_deg := rad_to_deg(_char_pitch)
	vp.set_camera_rotation_deg(cpp_pitch_deg, cpp_yaw_deg)

# =============================================================================
# HUD
# =============================================================================

func _draw() -> void:
	if not vp:
		return

	var font := ThemeDB.fallback_font
	var y := 10.0

	# Title
	draw_string(font, Vector2(10, y + 16), "OHAO Advanced Physics Test",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 16, Color.WHITE)
	y += 24.0

	# Current demo
	if current_demo_name != "":
		draw_string(font, Vector2(10, y + 14),
			"Demo %d: %s" % [current_demo, current_demo_name],
			HORIZONTAL_ALIGNMENT_LEFT, -1, 14, Color(0.5, 0.9, 1.0))
		y += 20.0

	# Physics state
	var state_str := "PAUSED" if physics_paused else "RUNNING"
	var state_color := Color.YELLOW if physics_paused else Color.GREEN
	draw_string(font, Vector2(10, y + 12), "Physics: %s" % state_str,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, state_color)
	y += 16.0

	# Actor counts
	var total := active_actors.size() + passive_actors.size() + visual_actors.size()
	draw_string(font, Vector2(10, y + 12),
		"Active: %d  Passive: %d  Visual: %d  (Total: %d)" % [
			active_actors.size(), passive_actors.size(), visual_actors.size(), total],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.7, 0.7, 0.7))
	y += 20.0

	# Demo-specific HUD
	match current_demo:
		1: _draw_hinge_hud(font, y)
		3: _draw_slider_hud(font, y)
		5: _draw_projectile_hud(font, y)
		6: _draw_layer_hud(font, y)
		7: _draw_raycast_hud(font, y)
		8: _draw_character_hud(font, y)

	# Log
	y = size.y - 90.0
	var log_start := maxi(0, _log_lines.size() - 5)
	for i in range(log_start, _log_lines.size()):
		draw_string(font, Vector2(10, y + 11), _log_lines[i],
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color(0.5, 0.5, 0.5))
		y += 14.0

	# Controls (right side)
	_draw_controls(font)

func _draw_hinge_hud(font: Font, y: float) -> void:
	var motor_str := "ON" if _hinge_motor_on else "OFF"
	var motor_col := Color.GREEN if _hinge_motor_on else Color.RED
	draw_string(font, Vector2(10, y + 12),
		"Hinge motor: %s  (M to toggle)" % motor_str,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, motor_col)

func _draw_slider_hud(font: Font, y: float) -> void:
	var motor_str := "ON" if _slider_motor_on else "OFF"
	draw_string(font, Vector2(10, y + 12),
		"Slider motor: %s  Dir: %.1f  (M toggle, Left/Right override)" % [motor_str, _slider_dir],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.7, 0.9, 0.7))

func _draw_projectile_hud(font: Font, y: float) -> void:
	draw_string(font, Vector2(10, y + 12),
		"Projectiles fired: %d  (F=fire, H=hitscan, C=clear)" % _projectile_count,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.9, 0.7, 0.3))

func _draw_layer_hud(font: Font, y: float) -> void:
	var col_str := "ON" if _layer_collision_on else "OFF"
	var col_color := Color.GREEN if _layer_collision_on else Color.RED
	draw_string(font, Vector2(10, y + 12),
		"Dynamic-Static collision: %s  (L to toggle)" % col_str,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, col_color)

func _draw_raycast_hud(font: Font, y: float) -> void:
	if _ray_hit_handle >= 0:
		draw_string(font, Vector2(10, y + 12),
			"Ray hit: pos=(%.1f, %.1f, %.1f) normal=(%.1f, %.1f, %.1f) handle=%d" % [
				_ray_hit_pos.x, _ray_hit_pos.y, _ray_hit_pos.z,
				_ray_hit_normal.x, _ray_hit_normal.y, _ray_hit_normal.z,
				_ray_hit_handle],
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.9, 0.9, 0.3))
	else:
		draw_string(font, Vector2(10, y + 12), "Ray: no hit",
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.5, 0.5, 0.5))
	y += 16.0
	draw_string(font, Vector2(10, y + 12),
		"Overlap count: %d  |  cast_ray_all hits: %d" % [_overlap_count, _ray_all_hits.size()],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.7, 0.7, 0.9))

func _draw_character_hud(font: Font, y: float) -> void:
	var ground_names := ["ON_GROUND", "ON_STEEP", "NOT_SUPPORTED", "IN_AIR"]
	var ground_colors := [Color.GREEN, Color.YELLOW, Color.ORANGE, Color.RED]
	var gs := clampi(_char_ground_state, 0, 3)
	draw_string(font, Vector2(10, y + 12),
		"Ground: %s" % ground_names[gs],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, ground_colors[gs])
	y += 16.0
	if _char_handle >= 0:
		var state := vp.get_character_state(_char_handle)
		var pos: Vector3 = state.get("position", Vector3.ZERO)
		draw_string(font, Vector2(10, y + 12),
			"Pos: (%.1f, %.1f, %.1f)  Yaw: %.1f" % [pos.x, pos.y, pos.z, rad_to_deg(_char_yaw)],
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.7, 0.7, 0.7))

func _draw_controls(font: Font) -> void:
	var rx := size.x - 250.0
	var ry := 10.0

	# Common controls
	var common := [
		"P - Play/Pause Physics",
		"1-8 - Switch Demo",
		"R - Reset",
		"TAB - Toggle Camera",
	]
	for line in common:
		draw_string(font, Vector2(rx, ry + 12), line,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.4, 0.4, 0.4))
		ry += 15.0

	ry += 5.0

	# Demo-specific controls
	var specific: Array[String] = []
	match current_demo:
		1: specific = ["M - Toggle motor"]
		3: specific = ["M - Toggle motor", "Left/Right - Override"]
		5: specific = ["F - Fire projectile", "H - Hitscan ray", "C - Clear"]
		6: specific = ["L - Toggle layers"]
		7: specific = ["A - Cast ray all", "G - Overlap sphere"]
		8: specific = ["WASD - Move", "Mouse - Look", "Space - Jump"]

	for line in specific:
		draw_string(font, Vector2(rx, ry + 12), line,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.5, 0.7, 0.9))
		ry += 15.0

# =============================================================================
# HELPERS
# =============================================================================

func _log(msg: String) -> void:
	print("[AdvPhys] %s" % msg)
	_log_lines.append(msg)
	if _log_lines.size() > 50:
		_log_lines = _log_lines.slice(_log_lines.size() - 50)
