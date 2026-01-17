@tool
extends EditorPlugin

var ohao_main_screen: Control
var ohao_viewport: OhaoViewport

func _enter_tree() -> void:
	# Create the main screen
	ohao_main_screen = preload("res://addons/ohao/ohao_editor_main.tscn").instantiate()

	# Add to editor main screen area
	var main_screen = get_editor_interface().get_editor_main_screen()
	main_screen.add_child(ohao_main_screen)

	# Set anchors AFTER adding to tree so they're relative to the editor main screen
	ohao_main_screen.set_anchors_preset(Control.PRESET_FULL_RECT)
	ohao_main_screen.set_offsets_preset(Control.PRESET_FULL_RECT)

	# Start hidden - Godot will call _make_visible(true) when tab is selected
	_make_visible(false)

	# Get viewport reference after adding to tree
	ohao_viewport = ohao_main_screen.get_node_or_null("HSplit/ViewportContainer/OhaoViewport")

	print("[OHAO] Editor plugin loaded")

func _exit_tree() -> void:
	if is_instance_valid(ohao_main_screen):
		ohao_main_screen.queue_free()
		ohao_main_screen = null
	print("[OHAO] Editor plugin unloaded")

func _has_main_screen() -> bool:
	return true

func _make_visible(visible: bool) -> void:
	if is_instance_valid(ohao_main_screen):
		ohao_main_screen.visible = visible
		print("[OHAO] Tab visibility: ", visible)

func _get_plugin_name() -> String:
	return "OHAO"

func _get_plugin_icon() -> Texture2D:
	var gui = get_editor_interface().get_base_control()
	if gui.has_theme_icon("Node3D", "EditorIcons"):
		return gui.get_theme_icon("Node3D", "EditorIcons")
	return null

func get_ohao_viewport() -> OhaoViewport:
	return ohao_viewport
