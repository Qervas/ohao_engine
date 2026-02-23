class_name PlayerController
extends Node3D
## First-person player controller for OHAO Engine FPS
## Handles movement, mouse look, jumping, crouching, and sprinting.
## Requires an OhaoViewport ancestor and an OhaoPhysicsBody child.

signal health_changed(current: float, maximum: float)
signal player_died

# Movement
@export var move_speed: float = 6.0
@export var sprint_speed: float = 10.0
@export var crouch_speed: float = 3.0
@export var jump_impulse: float = 5.0
@export var gravity: float = 18.0
@export var air_control: float = 0.3
@export var ground_friction: float = 10.0

# Mouse look
@export var mouse_sensitivity: float = 0.002
@export var pitch_limit: float = 89.0

# Player stats
@export var max_health: float = 100.0
@export var headbob_enabled: bool = true
@export var headbob_frequency: float = 12.0
@export var headbob_amplitude: float = 0.03

# Internal state
var velocity: Vector3 = Vector3.ZERO
var health: float = 100.0
var is_on_ground: bool = false
var is_crouching: bool = false
var is_sprinting: bool = false
var is_alive: bool = true
var camera_pitch: float = 0.0
var camera_yaw: float = 0.0
var headbob_time: float = 0.0

# References
var ohao_viewport: OhaoViewport
var physics_body: OhaoPhysicsBody
var weapon_system: WeaponSystem

func _ready() -> void:
	health = max_health

	# Find OhaoViewport in parent tree
	var node = get_parent()
	while node:
		if node is OhaoViewport:
			ohao_viewport = node
			break
		node = node.get_parent()

	if not ohao_viewport:
		push_warning("PlayerController: No OhaoViewport found in parent tree")
		return

	# Find physics body child
	for child in get_children():
		if child is OhaoPhysicsBody:
			physics_body = child
			break

	# Find weapon system
	for child in get_children():
		if child is WeaponSystem:
			weapon_system = child
			break

	# Capture mouse
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func _unhandled_input(event: InputEvent) -> void:
	if not is_alive:
		return

	# Mouse look
	if event is InputEventMouseMotion and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		camera_yaw -= event.relative.x * mouse_sensitivity
		camera_pitch -= event.relative.y * mouse_sensitivity
		camera_pitch = clampf(camera_pitch, deg_to_rad(-pitch_limit), deg_to_rad(pitch_limit))
		rotation.y = camera_yaw
		# Pitch is applied through viewport camera
		if ohao_viewport:
			ohao_viewport.set_mouse_sensitivity(mouse_sensitivity)

	# Toggle mouse capture
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		else:
			Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func _physics_process(delta: float) -> void:
	if not is_alive:
		return

	# Sprint / crouch state
	is_sprinting = Input.is_action_pressed("sprint") and not is_crouching
	if Input.is_action_just_pressed("crouch"):
		is_crouching = !is_crouching

	# Movement direction (relative to camera yaw)
	var input_dir := Vector2.ZERO
	if Input.is_action_pressed("move_forward"):
		input_dir.y -= 1.0
	if Input.is_action_pressed("move_backward"):
		input_dir.y += 1.0
	if Input.is_action_pressed("move_left"):
		input_dir.x -= 1.0
	if Input.is_action_pressed("move_right"):
		input_dir.x += 1.0
	input_dir = input_dir.normalized()

	var forward := -Vector3(sin(camera_yaw), 0, cos(camera_yaw))
	var right := Vector3(cos(camera_yaw), 0, -sin(camera_yaw))
	var move_dir := (forward * -input_dir.y + right * input_dir.x).normalized()

	# Speed selection
	var current_speed := move_speed
	if is_sprinting:
		current_speed = sprint_speed
	elif is_crouching:
		current_speed = crouch_speed

	# Apply movement
	var control_factor := 1.0 if is_on_ground else air_control
	var target_velocity := move_dir * current_speed

	velocity.x = lerpf(velocity.x, target_velocity.x, ground_friction * control_factor * delta)
	velocity.z = lerpf(velocity.z, target_velocity.z, ground_friction * control_factor * delta)

	# Gravity
	if not is_on_ground:
		velocity.y -= gravity * delta
	else:
		velocity.y = 0.0

	# Jump
	if Input.is_action_just_pressed("jump") and is_on_ground:
		velocity.y = jump_impulse
		is_on_ground = false

	# Direct position integration (OHAO kinematic bodies don't auto-sync back)
	global_position += velocity * delta
	# Inform physics body of velocity for collision queries
	if physics_body:
		physics_body.set_linear_velocity(velocity)

	# Ground check (simple - assume on ground if Y velocity is near zero at low height)
	# A proper implementation would use a raycast or collision callback
	if global_position.y <= 0.1 and velocity.y <= 0.0:
		is_on_ground = true
		global_position.y = 0.0
		velocity.y = 0.0

	# Headbob
	if headbob_enabled and is_on_ground and input_dir.length() > 0.1:
		headbob_time += delta * headbob_frequency
		var bob_offset := sin(headbob_time) * headbob_amplitude
		# Would apply to camera offset if we had direct camera node access
	else:
		headbob_time = 0.0

func take_damage(amount: float, from_direction: Vector3 = Vector3.ZERO) -> void:
	if not is_alive:
		return

	health -= amount
	health_changed.emit(health, max_health)

	if health <= 0.0:
		health = 0.0
		die()

func heal(amount: float) -> void:
	health = minf(health + amount, max_health)
	health_changed.emit(health, max_health)

func die() -> void:
	is_alive = false
	player_died.emit()

	# Release mouse
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)

func respawn(spawn_position: Vector3) -> void:
	global_position = spawn_position
	velocity = Vector3.ZERO
	health = max_health
	is_alive = true
	is_on_ground = false
	health_changed.emit(health, max_health)
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func get_look_direction() -> Vector3:
	return -Vector3(
		sin(camera_yaw) * cos(camera_pitch),
		sin(camera_pitch),
		cos(camera_yaw) * cos(camera_pitch)
	)

func get_muzzle_position() -> Vector3:
	# Approximate muzzle position in front of camera
	return global_position + Vector3(0, 1.6, 0) + get_look_direction() * 0.5
