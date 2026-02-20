@tool
extends EditorPlugin

var ohao_main_screen: Control
var ohao_viewport: OhaoViewport
var _signals_connected := false

func _enter_tree() -> void:
	ohao_main_screen = preload("res://addons/ohao/ohao_editor_main.tscn").instantiate()

	var main_screen = get_editor_interface().get_editor_main_screen()
	main_screen.add_child(ohao_main_screen)

	ohao_main_screen.set_anchors_preset(Control.PRESET_FULL_RECT)
	ohao_main_screen.set_offsets_preset(Control.PRESET_FULL_RECT)

	_make_visible(false)

	ohao_viewport = ohao_main_screen.get_node_or_null("VBoxContainer/ViewportArea/OhaoViewport")

	print("[OHAO] Editor plugin loaded")

func _exit_tree() -> void:
	if _signals_connected:
		scene_changed.disconnect(_on_scene_changed)
		scene_closed.disconnect(_on_scene_closed)
		_signals_connected = false
	if is_instance_valid(ohao_main_screen):
		ohao_main_screen.queue_free()
		ohao_main_screen = null
	print("[OHAO] Editor plugin unloaded")

func _process(_delta: float) -> void:
	# Wait for main screen script to be fully ready before connecting signals
	if not _signals_connected and _is_main_ready():
		scene_changed.connect(_on_scene_changed)
		scene_closed.connect(_on_scene_closed)
		_signals_connected = true
		# Do initial sync if tab is visible and empty
		if ohao_main_screen.visible and ohao_viewport:
			if ohao_viewport.get_synced_object_count() == 0:
				ohao_main_screen.trigger_full_sync()

func _has_main_screen() -> bool:
	return true

func _make_visible(visible: bool) -> void:
	if is_instance_valid(ohao_main_screen):
		ohao_main_screen.visible = visible
		if visible and _is_main_ready() and ohao_viewport:
			if ohao_viewport.get_synced_object_count() == 0:
				ohao_main_screen.trigger_full_sync()

func _get_plugin_name() -> String:
	return "OHAO"

func _get_plugin_icon() -> Texture2D:
	var gui = get_editor_interface().get_base_control()
	if gui.has_theme_icon("Node3D", "EditorIcons"):
		return gui.get_theme_icon("Node3D", "EditorIcons")
	return null

func get_ohao_viewport() -> OhaoViewport:
	return ohao_viewport

func _is_main_ready() -> bool:
	if not is_instance_valid(ohao_main_screen):
		return false
	# Check that the script is loaded (not a placeholder) and _is_ready is set
	return ohao_main_screen.get("_is_ready") == true

func _on_scene_changed(_scene_root: Node) -> void:
	if _is_main_ready():
		ohao_main_screen.on_scene_changed()

func _on_scene_closed(_filepath: String) -> void:
	if _is_main_ready():
		ohao_main_screen.on_scene_closed()
