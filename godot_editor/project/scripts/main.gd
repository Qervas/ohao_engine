extends Node3D
## Main scene controller for OHAO Engine
## Handles physics simulation controls and scene management

var physics_running := false
var physics_world = null  # Will be OHAO physics world

func _ready() -> void:
	print("[OHAO] Main scene ready")
	# TODO: Initialize OHAO physics world
	# physics_world = OhaoPhysicsWorld.new()

func _process(delta: float) -> void:
	# Handle input for physics controls
	if Input.is_action_just_pressed("ui_accept"):  # Space
		toggle_physics()

	if Input.is_action_just_pressed("ui_cancel"):  # Escape
		reset_physics()

func _physics_process(delta: float) -> void:
	if physics_running and physics_world:
		# Step OHAO physics
		# physics_world.step(delta)
		pass

func toggle_physics() -> void:
	physics_running = !physics_running
	print("[OHAO] Physics ", "started" if physics_running else "paused")

func reset_physics() -> void:
	physics_running = false
	print("[OHAO] Physics reset")
	# TODO: Reset all physics bodies to initial state
	# physics_world.reset()

# Called from UI buttons
func play_physics() -> void:
	physics_running = true
	print("[OHAO] Physics playing")

func pause_physics() -> void:
	physics_running = false
	print("[OHAO] Physics paused")

func step_physics() -> void:
	if physics_world:
		# physics_world.step_once()
		print("[OHAO] Physics stepped")
