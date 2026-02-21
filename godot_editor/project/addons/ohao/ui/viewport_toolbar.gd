@tool
extends HBoxContainer
## Viewport overlay toolbar: render mode, simulation controls, camera mode, stats

signal render_mode_changed(index: int)
signal camera_mode_changed(index: int)
signal play_pressed
signal pause_pressed
signal step_pressed
signal speed_changed(value: float)
signal post_process_toggled
signal wireframe_toggled(enabled: bool)
signal grid_toggled(enabled: bool)
signal import_model_requested

var _viewport: OhaoViewport

func setup(viewport: OhaoViewport) -> void:
	_viewport = viewport

func _ready() -> void:
	if not Engine.is_editor_hint():
		return

	# Semi-transparent dark background
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.1, 0.1, 0.1, 0.85)
	style.corner_radius_bottom_left = 4
	style.corner_radius_bottom_right = 4
	style.content_margin_left = 8
	style.content_margin_right = 8
	style.content_margin_top = 4
	style.content_margin_bottom = 4
	add_theme_stylebox_override("panel", style)

	# Build toolbar contents
	_build_toolbar()

func _build_toolbar() -> void:
	# Render mode dropdown
	var render_mode := OptionButton.new()
	render_mode.name = "RenderMode"
	render_mode.add_item("Forward", 0)
	render_mode.add_item("Deferred", 1)
	render_mode.custom_minimum_size.x = 100
	render_mode.item_selected.connect(func(idx): render_mode_changed.emit(idx))
	add_child(render_mode)

	_add_separator()

	# Simulation controls
	var play_btn := Button.new()
	play_btn.name = "PlayBtn"
	play_btn.text = "Play"
	play_btn.custom_minimum_size.x = 50
	play_btn.pressed.connect(func(): play_pressed.emit())
	add_child(play_btn)

	var pause_btn := Button.new()
	pause_btn.name = "PauseBtn"
	pause_btn.text = "Pause"
	pause_btn.custom_minimum_size.x = 50
	pause_btn.pressed.connect(func(): pause_pressed.emit())
	add_child(pause_btn)

	var step_btn := Button.new()
	step_btn.name = "StepBtn"
	step_btn.text = "Step"
	step_btn.custom_minimum_size.x = 50
	step_btn.pressed.connect(func(): step_pressed.emit())
	add_child(step_btn)

	# Speed control
	var speed_label := Label.new()
	speed_label.text = "1.0x"
	speed_label.name = "SpeedLabel"
	speed_label.custom_minimum_size.x = 35
	add_child(speed_label)

	var speed_slider := HSlider.new()
	speed_slider.name = "SpeedSlider"
	speed_slider.min_value = 0.1
	speed_slider.max_value = 3.0
	speed_slider.step = 0.1
	speed_slider.value = 1.0
	speed_slider.custom_minimum_size.x = 80
	speed_slider.value_changed.connect(func(val):
		speed_label.text = "%.1fx" % val
		speed_changed.emit(val)
	)
	add_child(speed_slider)

	_add_separator()

	# Camera mode
	var camera_mode := OptionButton.new()
	camera_mode.name = "CameraMode"
	camera_mode.add_item("FPS", 0)
	camera_mode.add_item("Orbit", 1)
	camera_mode.custom_minimum_size.x = 80
	camera_mode.item_selected.connect(func(idx): camera_mode_changed.emit(idx))
	add_child(camera_mode)

	_add_separator()

	# Wireframe toggle
	var wire_btn := Button.new()
	wire_btn.name = "WireframeBtn"
	wire_btn.text = "Wire"
	wire_btn.toggle_mode = true
	wire_btn.pressed.connect(func(): wireframe_toggled.emit(wire_btn.button_pressed))
	add_child(wire_btn)

	# Grid toggle
	var grid_btn := Button.new()
	grid_btn.name = "GridBtn"
	grid_btn.text = "Grid"
	grid_btn.toggle_mode = true
	grid_btn.button_pressed = true
	grid_btn.pressed.connect(func(): grid_toggled.emit(grid_btn.button_pressed))
	add_child(grid_btn)

	_add_separator()

	# Import model button
	var import_btn := Button.new()
	import_btn.name = "ImportBtn"
	import_btn.text = "Import"
	import_btn.pressed.connect(func(): import_model_requested.emit())
	add_child(import_btn)

	# Spacer
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(spacer)

	# Post-process panel toggle
	var pp_btn := Button.new()
	pp_btn.name = "PostProcessBtn"
	pp_btn.text = "Post FX"
	pp_btn.toggle_mode = true
	pp_btn.pressed.connect(func(): post_process_toggled.emit())
	add_child(pp_btn)

	_add_separator()

	# Stats label (updated every second)
	var stats_label := Label.new()
	stats_label.name = "StatsLabel"
	stats_label.text = "-- FPS | --x-- | 0 objs"
	stats_label.add_theme_font_size_override("font_size", 11)
	stats_label.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7, 0.9))
	add_child(stats_label)

func _add_separator() -> void:
	var sep := VSeparator.new()
	sep.custom_minimum_size.x = 8
	add_child(sep)

# Called from parent to update stats display
func update_stats(fps: int, width: int, height: int, obj_count: int) -> void:
	var label := get_node_or_null("StatsLabel") as Label
	if label:
		label.text = "%d FPS | %dx%d | %d objs" % [fps, width, height, obj_count]

func set_play_state(playing: bool) -> void:
	var play_btn := get_node_or_null("PlayBtn") as Button
	var pause_btn := get_node_or_null("PauseBtn") as Button
	if play_btn:
		play_btn.disabled = playing
	if pause_btn:
		pause_btn.disabled = not playing
