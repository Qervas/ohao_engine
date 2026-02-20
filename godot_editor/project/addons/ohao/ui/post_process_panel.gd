@tool
extends PanelContainer
## Slide-in post-processing panel with presets and collapsible advanced groups

signal settings_changed(setting: String, value: Variant)

var _viewport: OhaoViewport
var _is_custom := false
var _advanced_visible := false
var _group_containers := {}  # group_name -> VBoxContainer

func setup(viewport: OhaoViewport) -> void:
	_viewport = viewport

func _ready() -> void:
	if not Engine.is_editor_hint():
		return

	custom_minimum_size.x = 280

	# Dark semi-transparent background
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.12, 0.12, 0.14, 0.92)
	style.corner_radius_top_left = 4
	style.corner_radius_bottom_left = 4
	style.content_margin_left = 10
	style.content_margin_right = 10
	style.content_margin_top = 10
	style.content_margin_bottom = 10
	add_theme_stylebox_override("panel", style)

	_build_panel()

func _build_panel() -> void:
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(scroll)

	var vbox := VBoxContainer.new()
	vbox.name = "MainVBox"
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(vbox)

	# Title
	var title := Label.new()
	title.text = "Post Processing"
	title.add_theme_font_size_override("font_size", 16)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(title)

	vbox.add_child(HSeparator.new())

	# === Preset Buttons ===
	var preset_label := Label.new()
	preset_label.text = "Presets"
	preset_label.add_theme_font_size_override("font_size", 12)
	vbox.add_child(preset_label)

	var preset_grid := GridContainer.new()
	preset_grid.columns = 2
	preset_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.add_child(preset_grid)

	var presets := ["Cinematic", "Clean", "Stylized", "Custom"]
	for preset_name in presets:
		var btn := Button.new()
		btn.name = preset_name + "Btn"
		btn.text = preset_name
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.toggle_mode = true
		btn.button_group = _get_preset_group()
		btn.pressed.connect(_on_preset_selected.bind(preset_name))
		preset_grid.add_child(btn)

	vbox.add_child(HSeparator.new())

	# === Advanced Toggle ===
	var adv_btn := Button.new()
	adv_btn.name = "AdvancedToggle"
	adv_btn.text = "Advanced Settings >"
	adv_btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	adv_btn.pressed.connect(_toggle_advanced)
	vbox.add_child(adv_btn)

	# === Advanced Section (collapsed by default) ===
	var adv_container := VBoxContainer.new()
	adv_container.name = "AdvancedContainer"
	adv_container.visible = false
	vbox.add_child(adv_container)

	# Tonemapping group
	_add_group(adv_container, "Tonemapping", [
		_make_option("tonemap_operator", "Operator", ["ACES", "Reinhard", "Uncharted 2", "Neutral"], 0),
		_make_slider("exposure", "Exposure", 0.1, 5.0, 0.1, 1.0),
		_make_slider("gamma", "Gamma", 1.0, 3.0, 0.1, 2.2),
	])

	# Bloom group
	_add_group(adv_container, "Bloom", [
		_make_toggle("bloom_enabled", "Enable Bloom", true),
		_make_slider("bloom_intensity", "Intensity", 0.0, 2.0, 0.05, 0.5),
		_make_slider("bloom_threshold", "Threshold", 0.0, 3.0, 0.1, 1.0),
	])

	# SSAO group
	_add_group(adv_container, "Ambient Occlusion", [
		_make_toggle("ssao_enabled", "Enable SSAO", true),
		_make_slider("ssao_intensity", "Intensity", 0.0, 3.0, 0.1, 1.0),
		_make_slider("ssao_radius", "Radius", 0.05, 2.0, 0.05, 0.5),
	])

	# TAA group
	_add_group(adv_container, "Anti-Aliasing", [
		_make_toggle("taa_enabled", "Enable TAA", true),
	])

	# SSR group
	_add_group(adv_container, "Reflections", [
		_make_toggle("ssr_enabled", "Enable SSR", false),
	])

	# Volumetrics group
	_add_group(adv_container, "Volumetrics", [
		_make_toggle("volumetrics_enabled", "Enable Volumetric Fog", false),
		_make_slider("volumetric_density", "Density", 0.0, 0.2, 0.005, 0.02),
	])

	# Motion Blur group
	_add_group(adv_container, "Motion Blur", [
		_make_toggle("motion_blur_enabled", "Enable Motion Blur", false),
		_make_slider("motion_blur_intensity", "Intensity", 0.0, 1.0, 0.05, 0.5),
	])

	# Depth of Field group
	_add_group(adv_container, "Depth of Field", [
		_make_toggle("dof_enabled", "Enable DoF", false),
		_make_slider("dof_focus_distance", "Focus Distance", 1.0, 100.0, 1.0, 10.0),
		_make_slider("dof_aperture", "Aperture", 1.0, 22.0, 0.1, 2.8),
	])

var _preset_group: ButtonGroup
func _get_preset_group() -> ButtonGroup:
	if not _preset_group:
		_preset_group = ButtonGroup.new()
	return _preset_group

func _on_preset_selected(preset_name: String) -> void:
	if not _viewport:
		return

	_is_custom = (preset_name == "Custom")
	if _is_custom:
		return  # Custom = keep current settings

	match preset_name:
		"Cinematic":
			_apply_preset({
				"tonemap_operator": 0,  # ACES
				"bloom_enabled": true,
				"ssao_enabled": true,
				"dof_enabled": true,
				"exposure": 1.2,
				"taa_enabled": true,
				"ssr_enabled": false,
				"volumetrics_enabled": false,
				"motion_blur_enabled": false,
				"bloom_intensity": 0.5,
				"bloom_threshold": 1.0,
				"ssao_intensity": 1.0,
				"ssao_radius": 0.5,
				"gamma": 2.2,
			})
		"Clean":
			_apply_preset({
				"tonemap_operator": 3,  # Neutral
				"bloom_enabled": false,
				"ssao_enabled": false,
				"dof_enabled": false,
				"exposure": 1.0,
				"taa_enabled": true,
				"ssr_enabled": false,
				"volumetrics_enabled": false,
				"motion_blur_enabled": false,
				"gamma": 2.2,
			})
		"Stylized":
			_apply_preset({
				"tonemap_operator": 1,  # Reinhard
				"bloom_enabled": true,
				"bloom_intensity": 1.2,
				"bloom_threshold": 0.6,
				"ssao_enabled": true,
				"ssao_intensity": 2.0,
				"ssao_radius": 0.8,
				"dof_enabled": false,
				"exposure": 1.0,
				"taa_enabled": true,
				"ssr_enabled": false,
				"volumetrics_enabled": true,
				"volumetric_density": 0.05,
				"motion_blur_enabled": false,
				"gamma": 2.2,
			})

func _apply_preset(settings: Dictionary) -> void:
	if not _viewport:
		return
	for key in settings:
		var value = settings[key]
		_apply_setting(key, value)
		settings_changed.emit(key, value)

func _apply_setting(key: String, value: Variant) -> void:
	if not _viewport:
		return
	match key:
		"tonemap_operator": _viewport.set_tonemap_operator(value)
		"exposure": _viewport.set_exposure(value)
		"gamma": _viewport.set_gamma(value)
		"bloom_enabled": _viewport.set_bloom_enabled(value)
		"bloom_intensity": _viewport.set_bloom_intensity(value)
		"bloom_threshold": _viewport.set_bloom_threshold(value)
		"ssao_enabled": _viewport.set_ssao_enabled(value)
		"ssao_intensity": _viewport.set_ssao_intensity(value)
		"ssao_radius": _viewport.set_ssao_radius(value)
		"taa_enabled": _viewport.set_taa_enabled(value)
		"ssr_enabled": _viewport.set_ssr_enabled(value)
		"volumetrics_enabled": _viewport.set_volumetrics_enabled(value)
		"volumetric_density": _viewport.set_volumetric_density(value)
		"motion_blur_enabled": _viewport.set_motion_blur_enabled(value)
		"motion_blur_intensity": _viewport.set_motion_blur_intensity(value)
		"dof_enabled": _viewport.set_dof_enabled(value)
		"dof_focus_distance": _viewport.set_dof_focus_distance(value)
		"dof_aperture": _viewport.set_dof_aperture(value)

func _toggle_advanced() -> void:
	_advanced_visible = not _advanced_visible
	var adv := get_node_or_null("MainVBox/AdvancedContainer") as VBoxContainer
	if not adv:
		# ScrollContainer wraps, so find it via the scroll
		for child in get_children():
			if child is ScrollContainer:
				var main_vbox = child.get_node_or_null("MainVBox")
				if main_vbox:
					adv = main_vbox.get_node_or_null("AdvancedContainer")
				break
	if adv:
		adv.visible = _advanced_visible
	var toggle_btn = _find_node_recursive("AdvancedToggle")
	if toggle_btn:
		toggle_btn.text = "Advanced Settings v" if _advanced_visible else "Advanced Settings >"

func _find_node_recursive(node_name: String, parent: Node = null) -> Node:
	if parent == null:
		parent = self
	for child in parent.get_children():
		if child.name == node_name:
			return child
		var found := _find_node_recursive(node_name, child)
		if found:
			return found
	return null

# === UI Builder Helpers ===

func _add_group(parent: VBoxContainer, group_name: String, controls: Array) -> void:
	var header := Button.new()
	header.text = group_name + " >"
	header.alignment = HORIZONTAL_ALIGNMENT_LEFT
	header.add_theme_font_size_override("font_size", 12)
	parent.add_child(header)

	var content := VBoxContainer.new()
	content.name = group_name.replace(" ", "") + "Content"
	content.visible = false
	parent.add_child(content)

	_group_containers[group_name] = content

	header.pressed.connect(func():
		content.visible = not content.visible
		header.text = group_name + (" v" if content.visible else " >")
	)

	for ctrl in controls:
		if ctrl is Node:
			content.add_child(ctrl)

func _make_toggle(setting_key: String, label_text: String, default_val: bool) -> CheckBox:
	var check := CheckBox.new()
	check.name = setting_key
	check.text = label_text
	check.button_pressed = default_val
	check.toggled.connect(func(val):
		_apply_setting(setting_key, val)
		_mark_custom()
	)
	return check

func _make_slider(setting_key: String, label_text: String, min_val: float, max_val: float, step_val: float, default_val: float) -> VBoxContainer:
	var container := VBoxContainer.new()

	var label := Label.new()
	label.name = setting_key + "_label"
	label.text = "%s: %.2f" % [label_text, default_val]
	label.add_theme_font_size_override("font_size", 11)
	container.add_child(label)

	var slider := HSlider.new()
	slider.name = setting_key
	slider.min_value = min_val
	slider.max_value = max_val
	slider.step = step_val
	slider.value = default_val
	slider.value_changed.connect(func(val):
		label.text = "%s: %.2f" % [label_text, val]
		_apply_setting(setting_key, val)
		_mark_custom()
	)
	container.add_child(slider)

	return container

func _make_option(setting_key: String, label_text: String, options: Array, default_idx: int) -> VBoxContainer:
	var container := VBoxContainer.new()

	var label := Label.new()
	label.text = label_text
	label.add_theme_font_size_override("font_size", 11)
	container.add_child(label)

	var option := OptionButton.new()
	option.name = setting_key
	for i in options.size():
		option.add_item(options[i], i)
	option.selected = default_idx
	option.item_selected.connect(func(idx):
		_apply_setting(setting_key, idx)
		_mark_custom()
	)
	container.add_child(option)

	return container

func _mark_custom() -> void:
	_is_custom = true
	var custom_btn = _find_node_recursive("CustomBtn")
	if custom_btn and custom_btn is Button:
		custom_btn.button_pressed = true
