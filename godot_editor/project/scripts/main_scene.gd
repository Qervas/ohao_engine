extends Control
## Main scene controller for OHAO Engine
## Uses OHAO's custom Vulkan renderer embedded in the viewport

@onready var ohao_viewport: OhaoViewport = $OhaoViewport
@onready var play_pause_btn: Button = $SimulationPanel/VBox/PlaybackButtons/PlayPauseBtn
@onready var step_btn: Button = $SimulationPanel/VBox/PlaybackButtons/StepBtn
@onready var reset_btn: Button = $SimulationPanel/VBox/PlaybackButtons/ResetBtn
@onready var speed_slider: HSlider = $SimulationPanel/VBox/SpeedSlider
@onready var speed_label: Label = $SimulationPanel/VBox/SpeedLabel
@onready var status_label: Label = $SimulationPanel/VBox/StatusLabel
@onready var frame_label: Label = $SimulationPanel/VBox/FrameLabel

var is_playing := false
var simulation_speed := 1.0
var frame_count := 0

func _ready() -> void:
	print("[OHAO] Main scene ready - OHAO Vulkan renderer embedded")

	# Connect button signals
	play_pause_btn.pressed.connect(_on_play_pause_pressed)
	step_btn.pressed.connect(_on_step_pressed)
	reset_btn.pressed.connect(_on_reset_pressed)
	speed_slider.value_changed.connect(_on_speed_changed)

	update_ui()

	# Create test scene after a brief delay to let viewport initialize
	call_deferred("create_test_scene")

func _process(delta: float) -> void:
	if is_playing:
		frame_count += 1
		frame_label.text = "Frame: %d" % frame_count

func _on_play_pause_pressed() -> void:
	is_playing = !is_playing

	if is_playing:
		play_pause_btn.text = "|| Pause"
		status_label.text = "Status: Running"
		print("[OHAO] Physics simulation started")
	else:
		play_pause_btn.text = "> Play"
		status_label.text = "Status: Paused"
		print("[OHAO] Physics simulation paused")

	# TODO: Call OHAO physics world play/pause
	# if ohao_viewport:
	#     ohao_viewport.set_physics_running(is_playing)

func _on_step_pressed() -> void:
	if not is_playing:
		frame_count += 1
		frame_label.text = "Frame: %d" % frame_count
		print("[OHAO] Physics stepped one frame")
		# TODO: Call OHAO physics world step
		# if ohao_viewport:
		#     ohao_viewport.step_physics()

func _on_reset_pressed() -> void:
	is_playing = false
	frame_count = 0
	play_pause_btn.text = "> Play"
	status_label.text = "Status: Stopped"
	frame_label.text = "Frame: 0"
	print("[OHAO] Physics reset")
	# TODO: Call OHAO physics world reset
	# if ohao_viewport:
	#     ohao_viewport.reset_physics()

func _on_speed_changed(value: float) -> void:
	simulation_speed = value
	speed_label.text = "Simulation Speed: %.1fx" % value
	print("[OHAO] Simulation speed: %.1fx" % value)
	# TODO: Call OHAO physics world set speed
	# if ohao_viewport:
	#     ohao_viewport.set_simulation_speed(value)

func update_ui() -> void:
	play_pause_btn.text = "|| Pause" if is_playing else "> Play"
	status_label.text = "Status: Running" if is_playing else "Status: Stopped"
	frame_label.text = "Frame: %d" % frame_count
	speed_label.text = "Simulation Speed: %.1fx" % simulation_speed

func load_scene(path: String) -> void:
	if ohao_viewport:
		ohao_viewport.load_tscn(path)
		print("[OHAO] Loaded scene: ", path)

## Create a test scene with primitives to verify OHAO rendering
func create_test_scene() -> void:
	if not ohao_viewport:
		print("[OHAO] Error: No viewport available")
		return

	print("[OHAO] Creating test scene...")

	# Clear any existing scene
	ohao_viewport.clear_scene()

	# Add a ground plane
	ohao_viewport.add_plane("Ground", Vector3(0, -1, 0), Vector3.ZERO, Vector3(10, 1, 10), Color(0.3, 0.3, 0.3))

	# Add some cubes in a row
	ohao_viewport.add_cube("RedCube", Vector3(-2, 0, 0), Vector3.ZERO, Vector3.ONE, Color(0.9, 0.2, 0.2))
	ohao_viewport.add_cube("GreenCube", Vector3(0, 0, 0), Vector3.ZERO, Vector3.ONE, Color(0.2, 0.9, 0.2))
	ohao_viewport.add_cube("BlueCube", Vector3(2, 0, 0), Vector3.ZERO, Vector3.ONE, Color(0.2, 0.2, 0.9))

	# Add a sphere
	ohao_viewport.add_sphere("Sphere", Vector3(0, 2, 0), Vector3.ZERO, Vector3.ONE, Color(1.0, 0.8, 0.0))

	# Add a directional light
	ohao_viewport.add_directional_light("Sun", Vector3(5, 10, 5), Vector3(-0.5, -1, -0.3), Color.WHITE, 1.0)

	# Rebuild vertex buffers with new scene data
	ohao_viewport.finish_sync()

	print("[OHAO] Test scene created with 5 objects + 1 light")
