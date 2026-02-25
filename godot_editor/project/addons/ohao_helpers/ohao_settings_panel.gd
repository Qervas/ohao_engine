class_name OhaoSettingsPanel
extends CanvasLayer
## In-game settings panel auto-generated from OhaoSettings.EFFECTS.
## Toggle with F2 (or configured key). Every slider/toggle applies live.
##
## Usage:
##   var panel = OhaoSettingsPanel.new()
##   add_child(panel)
##   # Or: Ohao.create_settings_panel(self)
##   # Or: {"settings_panel": true} in scene builder config

@export var toggle_key: Key = KEY_F2

var _panel: PanelContainer
var _visible := false
var _vp: OhaoViewport
var _controls := {}  # param_key -> Control (for refreshing on preset apply)
var _toggles := {}   # effect_name -> CheckButton


func _ready() -> void:
	layer = 100
	_vp = Ohao.viewport(self)
	_build_ui()
	_panel.visible = false


func _unhandled_key_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == toggle_key:
			_toggle_panel()
			get_viewport().set_input_as_handled()


func _toggle_panel() -> void:
	_visible = not _visible
	_panel.visible = _visible
	if _visible:
		_refresh_all()


func _build_ui() -> void:
	# Background overlay
	var bg := ColorRect.new()
	bg.color = Color(0, 0, 0, 0.4)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(bg)

	# Main panel container (right side)
	_panel = PanelContainer.new()
	_panel.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
	_panel.offset_left = -360
	_panel.offset_top = 20
	_panel.offset_bottom = -20
	_panel.offset_right = -20
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.08, 0.08, 0.12, 0.92)
	style.corner_radius_top_left = 8
	style.corner_radius_top_right = 8
	style.corner_radius_bottom_left = 8
	style.corner_radius_bottom_right = 8
	style.border_width_left = 1
	style.border_width_right = 1
	style.border_width_top = 1
	style.border_width_bottom = 1
	style.border_color = Color(0.3, 0.3, 0.4)
	style.content_margin_left = 12
	style.content_margin_right = 12
	style.content_margin_top = 12
	style.content_margin_bottom = 12
	_panel.add_theme_stylebox_override("panel", style)
	add_child(_panel)

	# Scrollable content
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_panel.add_child(scroll)

	var root_vbox := VBoxContainer.new()
	root_vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root_vbox.add_theme_constant_override("separation", 6)
	scroll.add_child(root_vbox)

	# Title
	var title := Label.new()
	title.text = "OHAO Render Settings"
	title.add_theme_font_size_override("font_size", 18)
	title.add_theme_color_override("font_color", Color(0.9, 0.85, 1.0))
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	root_vbox.add_child(title)

	# Preset buttons
	_build_preset_row(root_vbox)

	# Separator
	root_vbox.add_child(HSeparator.new())

	# Stable effects first, experimental second
	var stable_names := []
	var experimental_names := []
	for effect_name in OhaoSettings.EFFECTS:
		if OhaoSettings.EFFECTS[effect_name]["stable"]:
			stable_names.append(effect_name)
		else:
			experimental_names.append(effect_name)

	for effect_name in stable_names:
		_build_effect_group(root_vbox, effect_name, OhaoSettings.EFFECTS[effect_name])

	if experimental_names.size() > 0:
		var exp_label := Label.new()
		exp_label.text = "-- Experimental --"
		exp_label.add_theme_font_size_override("font_size", 13)
		exp_label.add_theme_color_override("font_color", Color(1.0, 0.6, 0.2))
		exp_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		root_vbox.add_child(exp_label)

		for effect_name in experimental_names:
			_build_effect_group(root_vbox, effect_name, OhaoSettings.EFFECTS[effect_name])

	# Close hint
	var hint := Label.new()
	hint.text = "Press F2 to close"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.5, 0.5, 0.6))
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	root_vbox.add_child(hint)


func _build_preset_row(parent: VBoxContainer) -> void:
	var hbox := HBoxContainer.new()
	hbox.alignment = BoxContainer.ALIGNMENT_CENTER
	hbox.add_theme_constant_override("separation", 6)
	parent.add_child(hbox)

	var label := Label.new()
	label.text = "Presets:"
	label.add_theme_font_size_override("font_size", 13)
	label.add_theme_color_override("font_color", Color(0.7, 0.7, 0.8))
	hbox.add_child(label)

	for preset_name in OhaoPresets.RENDERING:
		var btn := Button.new()
		btn.text = preset_name.capitalize()
		btn.add_theme_font_size_override("font_size", 12)
		btn.custom_minimum_size = Vector2(70, 26)
		btn.pressed.connect(_on_preset_pressed.bind(preset_name))
		hbox.add_child(btn)


func _build_effect_group(parent: VBoxContainer, effect_name: String, effect: Dictionary) -> void:
	var group := VBoxContainer.new()
	group.add_theme_constant_override("separation", 2)
	parent.add_child(group)

	# Header with toggle
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 6)
	group.add_child(header)

	var check := CheckButton.new()
	var stability := "stable" if effect["stable"] else "experimental"
	check.text = "%s (%s)" % [effect_name.capitalize(), stability]
	check.add_theme_font_size_override("font_size", 14)
	if effect["stable"]:
		check.add_theme_color_override("font_color", Color(0.8, 0.9, 0.8))
	else:
		check.add_theme_color_override("font_color", Color(1.0, 0.7, 0.3))
	check.toggled.connect(_on_toggle.bind(effect["toggle"]))
	header.add_child(check)
	_toggles[effect_name] = check

	# Warning for experimental
	if not effect["stable"]:
		var warn := Label.new()
		warn.text = "May cause artifacts"
		warn.add_theme_font_size_override("font_size", 10)
		warn.add_theme_color_override("font_color", Color(1.0, 0.5, 0.2, 0.7))
		group.add_child(warn)

	# Parameters
	var params: Dictionary = effect["params"]
	for param_key in params:
		var param: Dictionary = params[param_key]
		var param_type: String = param.get("type", "float")

		if param_type == "color":
			_build_color_row(group, param_key, param)
		elif param_type == "int" and param.has("options"):
			_build_option_row(group, param_key, param)
		elif param_type == "int":
			_build_slider_row(group, param_key, param, true)
		else:
			_build_slider_row(group, param_key, param, false)

	# Small separator
	var sep := HSeparator.new()
	sep.add_theme_constant_override("separation", 4)
	group.add_child(sep)


func _build_slider_row(parent: VBoxContainer, param_key: String, param: Dictionary, is_int: bool) -> void:
	var hbox := HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 8)
	parent.add_child(hbox)

	# Label
	var label := Label.new()
	var display_name := param_key.replace("_", " ").capitalize()
	# Strip effect prefix for cleaner display
	for prefix in ["Bloom ", "Ssao ", "Ssr ", "Volumetric ", "Motion Blur ", "Dof ", "Taa ", "Tonemap "]:
		if display_name.begins_with(prefix):
			display_name = display_name.substr(prefix.length())
			break
	label.text = display_name
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.7))
	label.custom_minimum_size = Vector2(80, 0)
	hbox.add_child(label)

	# Slider
	var slider := HSlider.new()
	slider.min_value = param.get("min", 0.0)
	slider.max_value = param.get("max", 1.0)
	slider.step = 1.0 if is_int else 0.01
	slider.value = param.get("default", 0.0)
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size = Vector2(100, 0)
	hbox.add_child(slider)

	# Value label
	var val_label := Label.new()
	val_label.add_theme_font_size_override("font_size", 12)
	val_label.add_theme_color_override("font_color", Color(0.8, 0.8, 0.9))
	val_label.custom_minimum_size = Vector2(40, 0)
	val_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	if is_int:
		val_label.text = str(int(slider.value))
	else:
		val_label.text = "%.2f" % slider.value
	hbox.add_child(val_label)

	slider.value_changed.connect(func(val):
		if _vp:
			if is_int:
				_vp.call("set_" + param_key, int(val))
				val_label.text = str(int(val))
			else:
				_vp.call("set_" + param_key, val)
				val_label.text = "%.2f" % val
	)

	_controls[param_key] = slider


func _build_option_row(parent: VBoxContainer, param_key: String, param: Dictionary) -> void:
	var hbox := HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 8)
	parent.add_child(hbox)

	var label := Label.new()
	var display_name := param_key.replace("_", " ").capitalize()
	for prefix in ["Tonemap "]:
		if display_name.begins_with(prefix):
			display_name = display_name.substr(prefix.length())
			break
	label.text = display_name
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.7))
	label.custom_minimum_size = Vector2(80, 0)
	hbox.add_child(label)

	var option := OptionButton.new()
	option.add_theme_font_size_override("font_size", 12)
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var options_dict: Dictionary = param["options"]
	# Sort by value to maintain order
	var sorted_keys := options_dict.keys()
	sorted_keys.sort_custom(func(a, b): return options_dict[a] < options_dict[b])
	for opt_name in sorted_keys:
		option.add_item(opt_name, options_dict[opt_name])
	option.selected = param.get("default", 0)

	option.item_selected.connect(func(idx):
		if _vp:
			_vp.call("set_" + param_key, idx)
	)
	hbox.add_child(option)

	_controls[param_key] = option


func _build_color_row(parent: VBoxContainer, param_key: String, param: Dictionary) -> void:
	var hbox := HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 8)
	parent.add_child(hbox)

	var label := Label.new()
	var display_name := param_key.replace("_", " ").capitalize()
	label.text = display_name
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.7))
	label.custom_minimum_size = Vector2(80, 0)
	hbox.add_child(label)

	var picker := ColorPickerButton.new()
	picker.color = param.get("default", Color.WHITE)
	picker.custom_minimum_size = Vector2(60, 24)
	picker.color_changed.connect(func(col):
		if _vp:
			_vp.call("set_" + param_key, col)
	)
	hbox.add_child(picker)

	_controls[param_key] = picker


func _on_toggle(enabled: bool, toggle_key: String) -> void:
	if _vp:
		_vp.call("set_" + toggle_key, enabled)


func _on_preset_pressed(preset_name: String) -> void:
	if _vp:
		OhaoPresets.apply_rendering(_vp, preset_name)
		_refresh_all()


func _refresh_all() -> void:
	if not _vp:
		return

	for effect_name in OhaoSettings.EFFECTS:
		var effect: Dictionary = OhaoSettings.EFFECTS[effect_name]

		# Refresh toggle
		if _toggles.has(effect_name):
			var toggle_getter := "get_" + effect["toggle"]
			if _vp.has_method(toggle_getter):
				_toggles[effect_name].set_pressed_no_signal(_vp.call(toggle_getter))

		# Refresh params
		var params: Dictionary = effect["params"]
		for param_key in params:
			if not _controls.has(param_key):
				continue
			var ctrl = _controls[param_key]
			var getter := "get_" + param_key
			if _vp.has_method(getter):
				var val = _vp.call(getter)
				if ctrl is HSlider:
					ctrl.set_value_no_signal(val)
					# Update value label (next sibling)
					var parent_hbox = ctrl.get_parent()
					if parent_hbox and parent_hbox.get_child_count() > 2:
						var val_label = parent_hbox.get_child(2)
						if val_label is Label:
							if ctrl.step >= 1.0:
								val_label.text = str(int(val))
							else:
								val_label.text = "%.2f" % val
				elif ctrl is OptionButton:
					ctrl.selected = int(val)
				elif ctrl is ColorPickerButton:
					ctrl.color = val
