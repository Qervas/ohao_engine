@tool
extends Control
## OHAO Editor Main Screen
## Embeds OHAO's Vulkan renderer directly in the Godot Editor

@onready var ohao_viewport: OhaoViewport = $HSplit/ViewportContainer/OhaoViewport
@onready var play_pause_btn: Button = $HSplit/RightPanel/VBox/PlaybackButtons/PlayPauseBtn
@onready var step_btn: Button = $HSplit/RightPanel/VBox/PlaybackButtons/StepBtn
@onready var reset_btn: Button = $HSplit/RightPanel/VBox/PlaybackButtons/ResetBtn
@onready var speed_slider: HSlider = $HSplit/RightPanel/VBox/SpeedSlider
@onready var speed_label: Label = $HSplit/RightPanel/VBox/SpeedLabel
@onready var status_label: Label = $HSplit/RightPanel/VBox/StatusSection/StatusLabel
@onready var frame_label: Label = $HSplit/RightPanel/VBox/StatusSection/FrameLabel
@onready var fps_label: Label = $HSplit/RightPanel/VBox/StatusSection/FPSLabel
@onready var load_scene_btn: Button = $HSplit/RightPanel/VBox/SceneSection/LoadSceneBtn
@onready var sync_scene_btn: Button = $HSplit/RightPanel/VBox/SceneSection/SyncSceneBtn
@onready var examples_btn: OptionButton = $HSplit/RightPanel/VBox/SceneSection/ExamplesBtn
@onready var wireframe_check: CheckBox = $HSplit/RightPanel/VBox/RenderSection/WireframeCheck
@onready var grid_check: CheckBox = $HSplit/RightPanel/VBox/RenderSection/GridCheck
@onready var gizmos_check: CheckBox = $HSplit/RightPanel/VBox/RenderSection/GizmosCheck
@onready var render_mode_option: OptionButton = $HSplit/RightPanel/VBox/RenderSection/RenderModeOption

# Post-processing UI elements
@onready var tonemap_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/TonemapCheck"
@onready var tonemap_option: OptionButton = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/TonemapOption"
@onready var exposure_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/ExposureLabel"
@onready var exposure_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/ExposureSlider"
@onready var gamma_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/GammaLabel"
@onready var gamma_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/GammaSlider"
@onready var bloom_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/BloomCheck"
@onready var bloom_intensity_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/BloomIntensityLabel"
@onready var bloom_intensity_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/BloomIntensitySlider"
@onready var bloom_threshold_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/BloomThresholdLabel"
@onready var bloom_threshold_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/BloomThresholdSlider"
@onready var ssao_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/SSAOCheck"
@onready var ssao_intensity_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/SSAOIntensityLabel"
@onready var ssao_intensity_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/SSAOIntensitySlider"
@onready var ssao_radius_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/SSAORadiusLabel"
@onready var ssao_radius_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/SSAORadiusSlider"
@onready var taa_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/TAACheck"
@onready var ssr_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/SSRCheck"
@onready var volumetrics_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/VolumetricsCheck"
@onready var volumetric_density_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/VolumetricDensityLabel"
@onready var volumetric_density_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/VolumetricDensitySlider"
@onready var motion_blur_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/MotionBlurCheck"
@onready var motion_blur_intensity_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/MotionBlurIntensityLabel"
@onready var motion_blur_intensity_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/MotionBlurIntensitySlider"
@onready var dof_check: CheckBox = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/DOFCheck"
@onready var dof_focus_dist_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/DOFFocusDistLabel"
@onready var dof_focus_dist_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/DOFFocusDistSlider"
@onready var dof_aperture_label: Label = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/DOFApertureLabel"
@onready var dof_aperture_slider: HSlider = $"HSplit/RightPanel/VBox/PostProcessScroll/PostProcessVBox/DOFApertureSlider"

var is_playing := false
var simulation_speed := 1.0
var frame_count := 0
var fps_counter := 0.0
var fps_timer := 0.0

# Example scenes
const EXAMPLES_PATH := "res://examples/"
var example_scenes := [
	"basic_scene.tscn",
	"multi_light_scene.tscn"
]

func _ready() -> void:
	if not Engine.is_editor_hint():
		return

	# Connect signals
	play_pause_btn.pressed.connect(_on_play_pause)
	step_btn.pressed.connect(_on_step)
	reset_btn.pressed.connect(_on_reset)
	speed_slider.value_changed.connect(_on_speed_changed)
	load_scene_btn.pressed.connect(_on_load_scene)
	sync_scene_btn.pressed.connect(_on_sync_scene)
	examples_btn.item_selected.connect(_on_example_selected)
	wireframe_check.toggled.connect(_on_wireframe_toggled)
	grid_check.toggled.connect(_on_grid_toggled)
	gizmos_check.toggled.connect(_on_gizmos_toggled)
	render_mode_option.item_selected.connect(_on_render_mode_changed)

	# Connect post-processing signals
	tonemap_check.toggled.connect(_on_tonemap_toggled)
	tonemap_option.item_selected.connect(_on_tonemap_operator_changed)
	exposure_slider.value_changed.connect(_on_exposure_changed)
	gamma_slider.value_changed.connect(_on_gamma_changed)
	bloom_check.toggled.connect(_on_bloom_toggled)
	bloom_intensity_slider.value_changed.connect(_on_bloom_intensity_changed)
	bloom_threshold_slider.value_changed.connect(_on_bloom_threshold_changed)
	ssao_check.toggled.connect(_on_ssao_toggled)
	ssao_intensity_slider.value_changed.connect(_on_ssao_intensity_changed)
	ssao_radius_slider.value_changed.connect(_on_ssao_radius_changed)
	taa_check.toggled.connect(_on_taa_toggled)
	ssr_check.toggled.connect(_on_ssr_toggled)
	volumetrics_check.toggled.connect(_on_volumetrics_toggled)
	volumetric_density_slider.value_changed.connect(_on_volumetric_density_changed)
	motion_blur_check.toggled.connect(_on_motion_blur_toggled)
	motion_blur_intensity_slider.value_changed.connect(_on_motion_blur_intensity_changed)
	dof_check.toggled.connect(_on_dof_toggled)
	dof_focus_dist_slider.value_changed.connect(_on_dof_focus_distance_changed)
	dof_aperture_slider.value_changed.connect(_on_dof_aperture_changed)

	# Populate examples dropdown
	_populate_examples()

	print("[OHAO] Editor main screen ready (AAA Renderer)")

	# Don't auto-load - start empty, user can select from Examples dropdown
	# or open a scene in Godot 3D view and click "Sync from Editor"

func _populate_examples() -> void:
	examples_btn.clear()
	examples_btn.add_item("-- Load Example --", 0)
	var idx := 1
	for scene_name in example_scenes:
		# Remove .tscn extension for display
		var display_name = scene_name.replace(".tscn", "").replace("_", " ").capitalize()
		examples_btn.add_item(display_name, idx)
		idx += 1

func _load_default_example() -> void:
	if not ohao_viewport:
		return
	# Load the basic scene as default
	_load_example_scene("basic_scene.tscn")

func _load_example_scene(scene_name: String) -> void:
	var path = EXAMPLES_PATH + scene_name
	if not ResourceLoader.exists(path):
		print("[OHAO] Example scene not found: ", path)
		return

	var packed_scene := ResourceLoader.load(path) as PackedScene
	if not packed_scene:
		print("[OHAO] Failed to load: ", path)
		return

	var scene_root := packed_scene.instantiate()
	if not scene_root:
		print("[OHAO] Failed to instantiate: ", path)
		return

	# Add to tree temporarily so global transforms work
	add_child(scene_root)

	# Sync the instantiated scene to OHAO
	ohao_viewport.sync_from_godot(scene_root)
	var count = ohao_viewport.get_synced_object_count()
	print("[OHAO] Loaded example '%s' with %d objects" % [scene_name, count])

	# Clean up the temporary instance
	remove_child(scene_root)
	scene_root.queue_free()

func _on_example_selected(index: int) -> void:
	if index == 0:
		return  # "-- Load Example --" selected
	var scene_idx = index - 1
	if scene_idx >= 0 and scene_idx < example_scenes.size():
		_load_example_scene(example_scenes[scene_idx])
	# Reset dropdown to placeholder
	examples_btn.select(0)

func _process(delta: float) -> void:
	if not Engine.is_editor_hint():
		return

	# Update FPS counter
	fps_timer += delta
	fps_counter += 1
	if fps_timer >= 1.0:
		fps_label.text = "FPS: %d" % int(fps_counter)
		fps_counter = 0
		fps_timer = 0.0

	# Update frame count when playing
	if is_playing:
		frame_count += 1
		frame_label.text = "Frame: %d" % frame_count

func _on_play_pause() -> void:
	is_playing = !is_playing
	if is_playing:
		play_pause_btn.text = "Pause"
		status_label.text = "Status: Running"
	else:
		play_pause_btn.text = "Play"
		status_label.text = "Status: Paused"
	# TODO: ohao_viewport.set_physics_running(is_playing)

func _on_step() -> void:
	if not is_playing:
		frame_count += 1
		frame_label.text = "Frame: %d" % frame_count
		# TODO: ohao_viewport.step_physics()

func _on_reset() -> void:
	is_playing = false
	frame_count = 0
	play_pause_btn.text = "Play"
	status_label.text = "Status: Stopped"
	frame_label.text = "Frame: 0"
	# TODO: ohao_viewport.reset_physics()

func _on_speed_changed(value: float) -> void:
	simulation_speed = value
	speed_label.text = "Speed: %.1fx" % value
	# TODO: ohao_viewport.set_simulation_speed(value)

func _on_load_scene() -> void:
	var dialog := EditorFileDialog.new()
	dialog.file_mode = EditorFileDialog.FILE_MODE_OPEN_FILE
	dialog.access = EditorFileDialog.ACCESS_FILESYSTEM
	dialog.add_filter("*.tscn", "Godot Scene Files")
	dialog.file_selected.connect(_on_scene_file_selected)
	add_child(dialog)
	dialog.popup_centered(Vector2(800, 600))

func _on_scene_file_selected(path: String) -> void:
	if ohao_viewport:
		ohao_viewport.load_tscn(path)
		print("[OHAO] Loaded scene: ", path)

func _on_sync_scene() -> void:
	# Sync current editor scene to OHAO using optimized C++ implementation
	var edited_scene = EditorInterface.get_edited_scene_root()
	if not edited_scene:
		print("[OHAO] No scene open in editor")
		return
	if not ohao_viewport:
		print("[OHAO] Viewport not ready")
		return

	# Use C++ implementation for fast traversal and mapping
	ohao_viewport.sync_from_godot(edited_scene)
	var count = ohao_viewport.get_synced_object_count()
	print("[OHAO] Synced %d objects from editor scene" % count)

func _on_wireframe_toggled(enabled: bool) -> void:
	# TODO: ohao_viewport.set_wireframe_mode(enabled)
	pass

func _on_grid_toggled(enabled: bool) -> void:
	# TODO: ohao_viewport.set_grid_visible(enabled)
	pass

func _on_gizmos_toggled(enabled: bool) -> void:
	# TODO: ohao_viewport.set_gizmos_visible(enabled)
	pass

# === AAA Render Settings Callbacks ===

func _on_render_mode_changed(index: int) -> void:
	if ohao_viewport:
		ohao_viewport.set_render_mode(index)
		print("[OHAO] Render mode: %s" % ["Forward", "Deferred"][index])

func _on_tonemap_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_tonemapping_enabled(enabled)

func _on_tonemap_operator_changed(index: int) -> void:
	if ohao_viewport:
		ohao_viewport.set_tonemap_operator(index)
		print("[OHAO] Tonemap: %s" % ["ACES", "Reinhard", "Uncharted2", "Neutral"][index])

func _on_exposure_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_exposure(value)
	exposure_label.text = "Exposure: %.1f" % value

func _on_gamma_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_gamma(value)
	gamma_label.text = "Gamma: %.1f" % value

func _on_bloom_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_bloom_enabled(enabled)

func _on_bloom_intensity_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_bloom_intensity(value)
	bloom_intensity_label.text = "Intensity: %.2f" % value

func _on_bloom_threshold_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_bloom_threshold(value)
	bloom_threshold_label.text = "Threshold: %.1f" % value

func _on_ssao_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_ssao_enabled(enabled)

func _on_ssao_intensity_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_ssao_intensity(value)
	ssao_intensity_label.text = "Intensity: %.1f" % value

func _on_ssao_radius_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_ssao_radius(value)
	ssao_radius_label.text = "Radius: %.2f" % value

func _on_taa_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_taa_enabled(enabled)

func _on_ssr_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_ssr_enabled(enabled)

func _on_volumetrics_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_volumetrics_enabled(enabled)

func _on_volumetric_density_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_volumetric_density(value)
	volumetric_density_label.text = "Density: %.3f" % value

func _on_motion_blur_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_motion_blur_enabled(enabled)

func _on_motion_blur_intensity_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_motion_blur_intensity(value)
	motion_blur_intensity_label.text = "Intensity: %.2f" % value

func _on_dof_toggled(enabled: bool) -> void:
	if ohao_viewport:
		ohao_viewport.set_dof_enabled(enabled)

func _on_dof_focus_distance_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_dof_focus_distance(value)
	dof_focus_dist_label.text = "Focus Distance: %.1f" % value

func _on_dof_aperture_changed(value: float) -> void:
	if ohao_viewport:
		ohao_viewport.set_dof_aperture(value)
	dof_aperture_label.text = "Aperture: %.1f" % value
