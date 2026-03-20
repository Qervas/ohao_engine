class_name OhaoCharacter
extends Node
## OhaoCharacter — drop-in character controller wrapping Jolt physics.
##
## Attach to any Node3D. Handles movement, mouse look, jump, sprint, gravity.
## Uses the Jolt character controller for real collision/sliding/slopes.
##
## Usage:
##   var char = Ohao.make_character("fps", {move_speed = 8.0})
##   char.position = Vector3(0, 0, 0)
##   ohao_viewport.add_child(char)

# ── Presets ──

const PRESETS := {
	"fps": {
		"description": "First-person shooter. WASD + mouse look, jump, sprint.",
		"tags": ["fps", "shooter", "horror", "exploration"],
		"camera_mode": 0,  # CAMERA_FPS
		"eye_height": 1.6,
		"capsule_radius": 0.3,
		"capsule_height": 1.8,
		"max_slope_deg": 50.0,
		"mass": 80.0,
		"move_speed": 6.0,
		"sprint_speed": 10.0,
		"jump_height": 1.2,
		"mouse_sensitivity": 0.002,
		"gravity": 18.0,
		"air_control": 0.3,
		"ground_friction": 10.0,
	},
	"third_person": {
		"description": "Third-person. Over-shoulder orbit camera, WASD movement.",
		"tags": ["third_person", "rpg", "adventure"],
		"camera_mode": 1,  # CAMERA_ORBIT
		"eye_height": 1.6,
		"capsule_radius": 0.3,
		"capsule_height": 1.8,
		"max_slope_deg": 50.0,
		"mass": 80.0,
		"move_speed": 5.0,
		"sprint_speed": 9.0,
		"jump_height": 1.0,
		"mouse_sensitivity": 0.003,
		"gravity": 18.0,
		"air_control": 0.2,
		"ground_friction": 10.0,
	},
	"side_scroller": {
		"description": "2.5D side-scroller. Arrow keys / AD, fixed camera axis.",
		"tags": ["platformer", "side_scroller", "2d"],
		"camera_mode": 1,
		"eye_height": 1.0,
		"capsule_radius": 0.25,
		"capsule_height": 1.4,
		"max_slope_deg": 60.0,
		"mass": 60.0,
		"move_speed": 5.0,
		"sprint_speed": 8.0,
		"jump_height": 1.5,
		"mouse_sensitivity": 0.0,
		"gravity": 22.0,
		"air_control": 0.5,
		"ground_friction": 10.0,
	},
}

# ── Signals ──

signal jumped
signal landed
signal sprinting_changed(is_sprinting: bool)

# ── Exported params ──

@export var preset: String = "fps"
@export var move_speed: float = 6.0
@export var sprint_speed: float = 10.0
@export var jump_height: float = 1.2
@export var mouse_sensitivity: float = 0.002
@export var gravity: float = 18.0
@export var air_control: float = 0.3
@export var eye_height: float = 1.6
@export var capsule_radius: float = 0.3
@export var capsule_height: float = 1.8
@export var max_slope_deg: float = 50.0
@export var mass: float = 80.0
@export var pitch_limit: float = 89.0
@export var ground_friction: float = 10.0

# ── Internal state ──

var _char_handle: int = -1
var _vp: OhaoViewport
var _parent: Node3D
var _camera_pitch: float = 0.0
var _camera_yaw: float = 0.0
var _was_on_ground: bool = false
var _is_sprinting: bool = false
var _is_active: bool = true
var _overrides: Dictionary = {}


# ── Lifecycle ──

func _ready() -> void:
	_parent = get_parent() as Node3D
	if not _parent:
		push_warning("OhaoCharacter: parent must be Node3D, got %s" % (get_parent().get_class() if get_parent() else "null"))
		return

	_vp = Ohao.viewport(self)
	if not _vp:
		push_warning("OhaoCharacter: no OhaoViewport found in tree")
		return

	# Apply preset defaults, then user overrides
	_apply_preset()

	# Create Jolt character controller
	_char_handle = _vp.create_character(
		_parent.global_position,
		capsule_radius, capsule_height, max_slope_deg, mass
	)
	if _char_handle < 0:
		push_error("OhaoCharacter: failed to create Jolt character")
		return

	# Camera + input setup
	var cam_mode: int = PRESETS.get(preset, {}).get("camera_mode", OhaoConst.CAMERA_FPS)
	_vp.set_camera_mode(cam_mode)
	Ohao.enter_game_mode(_vp)
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

	# Start looking slightly down so player sees the ground
	_camera_pitch = deg_to_rad(-5.0)
	_sync_camera()

	# AI-playable convention
	_parent.add_to_group("player")

	print("[OhaoCharacter] Created '%s' character (handle=%d)" % [preset, _char_handle])


func _exit_tree() -> void:
	if _char_handle >= 0 and _vp and is_instance_valid(_vp):
		_vp.destroy_character(_char_handle)
		_char_handle = -1
	if _vp and is_instance_valid(_vp):
		Ohao.exit_game_mode(_vp)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _apply_preset() -> void:
	if not PRESETS.has(preset):
		return
	var p: Dictionary = PRESETS[preset]
	# Apply preset values only if user hasn't overridden via _overrides
	var fields := ["move_speed", "sprint_speed", "jump_height", "mouse_sensitivity",
		"gravity", "air_control", "eye_height", "capsule_radius", "capsule_height",
		"max_slope_deg", "mass", "ground_friction"]
	for field in fields:
		if _overrides.has(field):
			set(field, _overrides[field])
		elif p.has(field):
			set(field, p[field])


# ── Input ──

func _unhandled_input(event: InputEvent) -> void:
	if not _is_active or _char_handle < 0:
		return

	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_camera_yaw -= event.relative.x * mouse_sensitivity
		_camera_pitch -= event.relative.y * mouse_sensitivity
		_camera_pitch = clampf(_camera_pitch, -deg_to_rad(pitch_limit), deg_to_rad(pitch_limit))
		_sync_camera()

	# Escape toggles mouse capture
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		else:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


# ── Physics update ──

func _physics_process(delta: float) -> void:
	if not _is_active or _char_handle < 0 or not _vp:
		return

	# 1. Gather input direction
	var input_dir := Vector3.ZERO
	if Input.is_action_pressed("move_forward"):
		input_dir.z -= 1.0
	if Input.is_action_pressed("move_backward"):
		input_dir.z += 1.0
	if Input.is_action_pressed("move_left"):
		input_dir.x -= 1.0
	if Input.is_action_pressed("move_right"):
		input_dir.x += 1.0
	input_dir = input_dir.normalized()

	# 2. Rotate input by camera yaw
	var forward := Vector3(-sin(_camera_yaw), 0.0, -cos(_camera_yaw))
	var right := Vector3(cos(_camera_yaw), 0.0, -sin(_camera_yaw))
	var move_dir := (forward * input_dir.z + right * input_dir.x).normalized()

	# 3. Sprint
	var want_sprint := Input.is_action_pressed("sprint") and input_dir.length() > 0.1
	if want_sprint != _is_sprinting:
		_is_sprinting = want_sprint
		sprinting_changed.emit(_is_sprinting)
	var speed := sprint_speed if _is_sprinting else move_speed

	# 4. Get current state from Jolt
	var state: Dictionary = _vp.get_character_state(_char_handle)
	var on_ground: bool = state.get("is_grounded", false)
	var current_vel: Vector3 = state.get("velocity", Vector3.ZERO)

	# 5. Jump
	if Input.is_action_just_pressed("jump") and on_ground:
		var jump_impulse := sqrt(2.0 * gravity * jump_height)
		_vp.set_character_velocity(_char_handle,
			Vector3(current_vel.x, jump_impulse, current_vel.z))
		jumped.emit()

	# 6. Compute horizontal movement input
	var control := 1.0 if on_ground else air_control
	var target_vel := move_dir * speed
	var move_input := Vector3(
		lerpf(current_vel.x, target_vel.x, control * ground_friction * delta),
		0.0,
		lerpf(current_vel.z, target_vel.z, control * ground_friction * delta)
	)

	# 7. Update Jolt character (handles collision, sliding, slopes, gravity)
	_vp.update_character(_char_handle, delta, Vector3(0, -gravity, 0), move_input)

	# 8. Read back authoritative position
	var new_state: Dictionary = _vp.get_character_state(_char_handle)
	var new_pos: Vector3 = new_state.get("position", _parent.global_position)
	var new_on_ground: bool = new_state.get("is_grounded", false)

	# 9. Sync parent Node3D
	_parent.global_position = new_pos

	# 10. Landing detection
	if new_on_ground and not _was_on_ground:
		landed.emit()
	_was_on_ground = new_on_ground

	# 11. Sync camera
	_sync_camera()


func _sync_camera() -> void:
	if not _vp or not _parent:
		return
	# FPS camera: eye position above character
	_vp.set_camera_position(_parent.global_position + Vector3(0, eye_height, 0))
	# Yaw conversion: cpp_yaw_deg = -rad_to_deg(godot_yaw) - 90.0
	var cpp_pitch: float = rad_to_deg(_camera_pitch)
	var cpp_yaw: float = -rad_to_deg(_camera_yaw) - 90.0
	_vp.set_camera_rotation_deg(cpp_pitch, cpp_yaw)


# ── Public API ──

func set_active(active: bool) -> void:
	_is_active = active


func teleport(pos: Vector3) -> void:
	if _char_handle >= 0 and _vp:
		_vp.set_character_position(_char_handle, pos)
	if _parent:
		_parent.global_position = pos
	_sync_camera()


func get_velocity() -> Vector3:
	if _char_handle >= 0 and _vp:
		var state: Dictionary = _vp.get_character_state(_char_handle)
		return state.get("velocity", Vector3.ZERO)
	return Vector3.ZERO


func get_look_direction() -> Vector3:
	if _vp:
		return _vp.get_camera_forward()
	return Vector3.FORWARD


func get_eye_position() -> Vector3:
	if _parent:
		return _parent.global_position + Vector3(0, eye_height, 0)
	return Vector3.ZERO


func is_on_ground() -> bool:
	return _was_on_ground


func get_camera_yaw() -> float:
	return _camera_yaw


func get_camera_pitch() -> float:
	return _camera_pitch


## AI-playable convention — called by OhaoServer /god/game_state
func get_game_state() -> Dictionary:
	var pos := _parent.global_position if _parent else Vector3.ZERO
	var state: Dictionary = {}
	if _char_handle >= 0 and _vp:
		state = _vp.get_character_state(_char_handle)
	return {
		"position": [pos.x, pos.y, pos.z],
		"velocity": [state.get("velocity", Vector3.ZERO).x,
			state.get("velocity", Vector3.ZERO).y,
			state.get("velocity", Vector3.ZERO).z],
		"on_ground": state.get("is_grounded", false),
		"ground_state": state.get("ground_state", OhaoConst.GROUND_IN_AIR),
		"is_sprinting": _is_sprinting,
		"is_active": _is_active,
		"preset": preset,
	}


# ── Static discovery API ──

static func list_presets() -> Array:
	return PRESETS.keys()


static func get_preset_info(preset_name: String) -> Dictionary:
	return PRESETS.get(preset_name, {})


static func recommended_for(tags: Array) -> Array:
	var results: Array = []
	for name in PRESETS:
		var p: Dictionary = PRESETS[name]
		var p_tags: Array = p.get("tags", [])
		for tag in tags:
			if tag in p_tags:
				results.append(name)
				break
	return results
