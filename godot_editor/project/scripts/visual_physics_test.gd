extends Control
## Visual Physics Demo
##
## Uses the built-in physics on visual actors (no OhaoPhysicsBody nodes).
## Each actor created via vp.add_cube/sphere already has a PhysicsComponent.
## We configure it as static or dynamic via the per-actor physics API.
##
## Controls:
##   P     - Play/Pause physics
##   1-4   - Switch demo
##   R     - Reset current demo
##   TAB   - Toggle editor/game camera
##   Mouse - Orbit camera (editor mode)

@onready var vp: OhaoViewport = $OhaoViewport

var current_demo: String = ""
var physics_paused: bool = true
# Track actor names per category for HUD
var active_actors: Array[String] = []
var passive_actors: Array[String] = []
var visual_actors: Array[String] = []

func _ready() -> void:
	if not vp:
		push_error("OhaoViewport not found!")
		return
	call_deferred("_init_scene")

func _init_scene() -> void:
	vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)
	vp.set_camera_position(Vector3(0, 6, 12))
	vp.set_camera_rotation_deg(-20, 0)

	vp.clear_scene()
	vp.add_directional_light("Sun", Vector3(0, 20, 0),
		Vector3(-0.4, -1, -0.3), Color(1, 0.95, 0.9), 1.8)
	vp.add_point_light("Fill", Vector3(-8, 6, 8),
		Color(0.6, 0.7, 1.0), 1.5, 20.0)

	vp.set_ssao_enabled(true)
	vp.set_bloom_enabled(true)
	vp.set_bloom_intensity(0.3)
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)

	_demo_bouncing_balls()
	_log("Physics PAUSED. Press P to start, 1-4 for demos.")

# =============================================================================
# FACTORIES — configure the built-in physics on visual actors
# =============================================================================

## Static box: visual + physics collider that never moves
func _make_static_box(aname: String, pos: Vector3, scl: Vector3, color: Color,
		restitution: float = 0.0, friction: float = 0.8) -> void:
	vp.add_cube(aname, pos, Vector3.ZERO, scl, color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_STATIC)
	vp.set_actor_mass(aname, 0.0)
	vp.set_actor_friction(aname, friction)
	vp.set_actor_restitution(aname, restitution)
	vp.sync_actor_physics_shape(aname)
	passive_actors.append(aname)

## Dynamic box: visual + physics, affected by gravity
func _make_dynamic_box(aname: String, pos: Vector3, scl: Vector3, color: Color,
		mass: float = 2.0, restitution: float = 0.2, friction: float = 0.5) -> void:
	vp.add_cube(aname, pos, Vector3.ZERO, scl, color)
	vp.set_actor_body_type(aname, OhaoConst.BODY_DYNAMIC)
	vp.set_actor_mass(aname, mass)
	vp.set_actor_friction(aname, friction)
	vp.set_actor_restitution(aname, restitution)
	vp.sync_actor_physics_shape(aname)
	active_actors.append(aname)

## Dynamic sphere: visual + physics, affected by gravity
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

## Visual-only: just a rendered mesh, physics disabled
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
	# Make the built-in physics static + zero mass so it does nothing
	vp.set_actor_body_type(aname, OhaoConst.BODY_STATIC)
	vp.set_actor_mass(aname, 0.0)
	vp.set_actor_gravity_enabled(aname, false)
	vp.sync_actor_physics_shape(aname)
	visual_actors.append(aname)

# =============================================================================
# SCENE MANAGEMENT
# =============================================================================

func _reset_scene() -> void:
	active_actors.clear()
	passive_actors.clear()
	visual_actors.clear()
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
		"Bouncing Balls": _demo_bouncing_balls()
		"Box Stack": _demo_box_stack()
		"Domino Chain": _demo_dominos()
		"Ramp + Collision": _demo_ramp_collision()
		_: _demo_bouncing_balls()
	_log("Reset. Press P to start physics.")

# =============================================================================
# DEMOS
# =============================================================================

func _demo_bouncing_balls() -> void:
	_reset_scene()
	current_demo = "Bouncing Balls"
	_log("Demo: Bouncing Balls — different bounciness")

	# --- Passive: ground + walls ---
	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4), 0.5)
	_make_static_box("WallL", Vector3(-10, 0.5, 0), Vector3(0.3, 1.5, 20), Color(0.3, 0.3, 0.35))
	_make_static_box("WallR", Vector3(10, 0.5, 0), Vector3(0.3, 1.5, 20), Color(0.3, 0.3, 0.35))
	_make_static_box("WallF", Vector3(0, 0.5, -10), Vector3(20, 1.5, 0.3), Color(0.3, 0.3, 0.35))
	_make_static_box("WallB", Vector3(0, 0.5, 10), Vector3(20, 1.5, 0.3), Color(0.3, 0.3, 0.35))

	# --- Visual: ground markers ---
	for i in range(7):
		var x := -6.0 + i * 2.0
		_make_visual("Mark_%d" % i, OhaoConst.MESH_CYLINDER,
			Vector3(x, 0.02, -3.0), Vector3(0.6, 0.04, 0.6), Color(0.15, 0.15, 0.2))

	# --- Active: bouncing spheres (different bounciness) ---
	var bounce_colors := [Color(0.2, 0.2, 0.9), Color(0.3, 0.5, 0.9), Color(0.2, 0.7, 0.7),
		Color(0.3, 0.8, 0.3), Color(0.9, 0.8, 0.2), Color(0.9, 0.4, 0.1), Color(0.9, 0.1, 0.1)]
	for i in range(7):
		var x := -6.0 + i * 2.0
		var bounce := 0.1 + i * 0.14
		_make_dynamic_sphere("Bounce_%d" % i, Vector3(x, 5.0, -3.0), 0.4,
			bounce_colors[i], 1.0, bounce)

	# Heavy ball
	_make_dynamic_sphere("Heavy", Vector3(0, 8.0, 0), 0.8,
		Color(0.9, 0.85, 0.1), 20.0, 0.5)

	vp.finish_sync()

func _demo_box_stack() -> void:
	_reset_scene()
	current_demo = "Box Stack"
	_log("Demo: Box Stack — pyramid + wrecking ball")

	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(20, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Decorative pillars
	_make_visual("PillarL", OhaoConst.MESH_CYLINDER,
		Vector3(-4, 1.5, -3), Vector3(0.3, 3, 0.3), Color(0.25, 0.25, 0.3))
	_make_visual("PillarR", OhaoConst.MESH_CYLINDER,
		Vector3(4, 1.5, -3), Vector3(0.3, 3, 0.3), Color(0.25, 0.25, 0.3))

	# Tower of boxes
	var layer_colors := [Color(0.8, 0.2, 0.2), Color(0.8, 0.5, 0.1), Color(0.8, 0.8, 0.2),
		Color(0.3, 0.7, 0.2), Color(0.2, 0.4, 0.8), Color(0.6, 0.2, 0.8)]
	for row in range(6):
		var count := 6 - row
		var y := 0.5 + row * 1.05
		var x_start := -(count - 1) * 0.55
		for col in range(count):
			var x := x_start + col * 1.1
			_make_dynamic_box("Stack_%d_%d" % [row, col],
				Vector3(x, y, 0), Vector3(1, 1, 1),
				layer_colors[row], 2.0, 0.05)

	# Wrecking ball
	_make_dynamic_sphere("Wrecker", Vector3(6.0, 8.0, 0), 0.7,
		Color(0.9, 0.1, 0.1), 15.0, 0.2)

	vp.finish_sync()

func _demo_dominos() -> void:
	_reset_scene()
	current_demo = "Domino Chain"
	_log("Demo: Domino Chain — topple the first")

	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(30, 0.5, 10),
		Color(0.35, 0.35, 0.4))

	# Lane markers
	_make_visual("LaneL", OhaoConst.MESH_CUBE,
		Vector3(0, 0.02, -1.2), Vector3(22, 0.04, 0.05), Color(0.5, 0.5, 0.2))
	_make_visual("LaneR", OhaoConst.MESH_CUBE,
		Vector3(0, 0.02, 1.2), Vector3(22, 0.04, 0.05), Color(0.5, 0.5, 0.2))

	# Dominos
	var domino_count := 20
	var spacing := 0.9
	var start_x := -(domino_count - 1) * spacing * 0.5
	for i in range(domino_count):
		var x := start_x + i * spacing
		var hue := float(i) / float(domino_count)
		var color := Color.from_hsv(hue, 0.7, 0.9)
		_make_dynamic_box("Domino_%d" % i,
			Vector3(x, 1.0, 0), Vector3(0.2, 2.0, 0.8),
			color, 1.0, 0.05)

	# Trigger ball with initial velocity
	_make_dynamic_sphere("Trigger", Vector3(start_x - 2.5, 1.0, 0), 0.4,
		Color.WHITE, 3.0, 0.1)
	vp.set_actor_linear_velocity("Trigger", Vector3(4.0, 0, 0))

	vp.finish_sync()

func _demo_ramp_collision() -> void:
	_reset_scene()
	current_demo = "Ramp + Collision"
	_log("Demo: Ramp — balls roll down and hit targets")

	_make_static_box("Ground", Vector3(0, -0.25, 0), Vector3(30, 0.5, 20),
		Color(0.35, 0.35, 0.4))

	# Ramp (tilted static box)
	vp.add_cube("Ramp", Vector3(-6, 2.5, 0), Vector3(0, 0, -20), Vector3(10, 0.3, 4),
		Color(0.4, 0.5, 0.3))
	vp.set_actor_body_type("Ramp", OhaoConst.BODY_STATIC)
	vp.set_actor_mass("Ramp", 0.0)
	vp.set_actor_friction("Ramp", 0.3)
	vp.sync_actor_physics_shape("Ramp")
	passive_actors.append("Ramp")

	_make_static_box("RampSupport", Vector3(-9, 1.5, 0), Vector3(1, 3, 4),
		Color(0.5, 0.4, 0.3))

	# Cones
	_make_visual("Cone1", OhaoConst.MESH_CYLINDER,
		Vector3(0, 0.3, -3), Vector3(0.2, 0.6, 0.2), Color(1.0, 0.5, 0.0))
	_make_visual("Cone2", OhaoConst.MESH_CYLINDER,
		Vector3(0, 0.3, 3), Vector3(0.2, 0.6, 0.2), Color(1.0, 0.5, 0.0))

	# Target boxes
	for i in range(5):
		var z := -2.0 + i * 1.0
		_make_dynamic_box("Target_%d" % i,
			Vector3(2.0, 0.5, z), Vector3(0.8, 0.8, 0.8),
			Color(0.9, 0.2, 0.2), 1.5, 0.1)

	# Balls at top of ramp
	var ball_colors := [Color(0.2, 0.6, 1.0), Color(0.1, 0.9, 0.3), Color(1.0, 0.8, 0.1)]
	for i in range(3):
		var z := -1.0 + i * 1.0
		_make_dynamic_sphere("RampBall_%d" % i,
			Vector3(-9, 5.5 + i * 0.8, z), 0.4,
			ball_colors[i], 3.0, 0.3)

	vp.finish_sync()

# =============================================================================
# INPUT
# =============================================================================

func _unhandled_input(event: InputEvent) -> void:
	if not vp:
		return
	if event is InputEventKey and event.pressed:
		match event.keycode:
			KEY_1: _demo_bouncing_balls()
			KEY_2: _demo_box_stack()
			KEY_3: _demo_dominos()
			KEY_4: _demo_ramp_collision()
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
				else:
					Ohao.enter_game_mode()

# =============================================================================
# HUD (no sync loop needed — renderer auto-syncs from PhysicsComponent)
# =============================================================================

func _process(_delta: float) -> void:
	queue_redraw()

func _draw() -> void:
	if not vp:
		return

	var font := ThemeDB.fallback_font
	var y := 10.0

	draw_string(font, Vector2(10, y + 16), "OHAO Visual Physics Demo",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 16, Color.WHITE)
	y += 24.0

	if current_demo != "":
		draw_string(font, Vector2(10, y + 14), current_demo,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 14, Color(0.5, 0.9, 1.0))
		y += 20.0

	var total := active_actors.size() + passive_actors.size() + visual_actors.size()
	draw_string(font, Vector2(10, y + 12),
		"Active: %d  Passive: %d  Visual: %d  (Total: %d)" % [
			active_actors.size(), passive_actors.size(), visual_actors.size(), total],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.7, 0.7, 0.7))
	y += 16.0

	var state_str := "PAUSED" if physics_paused else "RUNNING"
	var state_color := Color.YELLOW if physics_paused else Color.GREEN
	draw_string(font, Vector2(10, y + 12), "Physics: %s" % state_str,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, state_color)
	y += 20.0

	var log_start := maxi(0, _log_lines.size() - 5)
	for i in range(log_start, _log_lines.size()):
		draw_string(font, Vector2(10, y + 11), _log_lines[i],
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color(0.5, 0.5, 0.5))
		y += 14.0

	var rx := size.x - 220.0
	var ry := 10.0
	var controls := [
		"P - Play/Pause Physics",
		"1 - Bouncing Balls",
		"2 - Box Stack",
		"3 - Dominos",
		"4 - Ramp Collision",
		"R - Reset",
		"TAB - Toggle Camera",
	]
	for line in controls:
		draw_string(font, Vector2(rx, ry + 12), line,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.4, 0.4, 0.4))
		ry += 15.0

# =============================================================================
# HELPERS
# =============================================================================

var _log_lines: Array[String] = []

func _log(msg: String) -> void:
	print("[VisPhys] %s" % msg)
	_log_lines.append(msg)
	if _log_lines.size() > 30:
		_log_lines = _log_lines.slice(_log_lines.size() - 30)
