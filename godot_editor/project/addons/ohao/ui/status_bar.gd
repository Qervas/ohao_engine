@tool
extends HBoxContainer
## Bottom status bar: sync status, object count, resolution

## Sync states - use these constants from other scripts
const SYNC_NONE := 0
const SYNC_SYNCING := 1
const SYNC_SYNCED := 2

var _sync_state := SYNC_NONE
var _sync_dot: ColorRect
var _sync_label: Label
var _obj_label: Label
var _res_label: Label

func _ready() -> void:
	if not Engine.is_editor_hint():
		return

	custom_minimum_size.y = 24

	# Dark background
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.12, 0.12, 0.14, 1.0)
	style.content_margin_left = 8
	style.content_margin_right = 8
	style.content_margin_top = 2
	style.content_margin_bottom = 2
	add_theme_stylebox_override("panel", style)

	_build_status_bar()

func _build_status_bar() -> void:
	# Sync indicator dot
	_sync_dot = ColorRect.new()
	_sync_dot.custom_minimum_size = Vector2(8, 8)
	_sync_dot.color = Color(0.5, 0.5, 0.5)  # Gray = no scene
	add_child(_sync_dot)

	# Center the dot vertically
	var dot_margin := Control.new()
	dot_margin.custom_minimum_size.x = 4
	add_child(dot_margin)

	# Sync label
	_sync_label = Label.new()
	_sync_label.text = "No scene"
	_sync_label.add_theme_font_size_override("font_size", 11)
	_sync_label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	add_child(_sync_label)

	# Spacer
	var spacer1 := Control.new()
	spacer1.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(spacer1)

	# Object count
	_obj_label = Label.new()
	_obj_label.text = "0 objects"
	_obj_label.add_theme_font_size_override("font_size", 11)
	_obj_label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	add_child(_obj_label)

	# Spacer
	var spacer2 := Control.new()
	spacer2.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(spacer2)

	# Resolution
	_res_label = Label.new()
	_res_label.text = "--x--"
	_res_label.add_theme_font_size_override("font_size", 11)
	_res_label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	add_child(_res_label)

func set_sync_state(state: int, object_count: int = 0) -> void:
	_sync_state = state
	if not _sync_dot or not _sync_label:
		return
	match state:
		SYNC_NONE:
			_sync_dot.color = Color(0.5, 0.5, 0.5)  # Gray
			_sync_label.text = "No scene"
		SYNC_SYNCING:
			_sync_dot.color = Color(0.9, 0.8, 0.1)  # Yellow
			_sync_label.text = "Syncing..."
		SYNC_SYNCED:
			_sync_dot.color = Color(0.2, 0.8, 0.2)  # Green
			_sync_label.text = "Synced: %d objects" % object_count

func set_object_count(count: int) -> void:
	if _obj_label:
		_obj_label.text = "%d objects" % count

func set_resolution(width: int, height: int) -> void:
	if _res_label:
		_res_label.text = "%dx%d" % [width, height]
