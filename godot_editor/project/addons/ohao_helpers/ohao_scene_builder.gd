class_name OhaoSceneBuilder
## Declarative scene building from Dictionary descriptions.

const _Control = preload("res://addons/ohao_helpers/ohao_control.gd")
##
## Usage:
##   OhaoSceneBuilder.build(vp, {
##       "template": "arena",
##       "rendering": "horror",
##       "objects": [
##           {"type": "cube", "name": "Box", "pos": Vector3(0, 1, 0),
##            "scale": Vector3(1, 1, 1), "color": Color.RED},
##       ],
##       "lights": [
##           {"type": "point", "name": "L", "pos": Vector3(3, 4, 0),
##            "color": Color.WHITE, "intensity": 2.0, "range": 15.0},
##       ],
##   })


## Build a full scene from a dictionary description.
## Keys:
##   "template" (String) - Base template name from OhaoPresets.TEMPLATES
##   "rendering" (String|Dictionary) - Preset name or custom settings dict
##   "objects" (Array) - Objects to add
##   "lights" (Array) - Lights to add
static func build(vp, desc: Dictionary) -> void:
	vp.clear_scene()

	# Apply template first (base objects + lights)
	if desc.has("template"):
		OhaoPresets.apply_template(vp, desc["template"])

	# Add scene-specific objects and lights (merged with template)
	if desc.has("objects"):
		OhaoPresets._add_objects(vp, desc["objects"])
	if desc.has("lights"):
		OhaoPresets._add_lights(vp, desc["lights"])

	# Finish GPU buffer rebuild
	vp.finish_sync()

	# Apply rendering preset or custom settings
	if desc.has("rendering"):
		var r = desc["rendering"]
		if r is String:
			OhaoPresets.apply_rendering(vp, r)
		elif r is Dictionary:
			for key in r:
				vp.call("set_" + key, r[key])

	# Apply control template if specified
	# "control": "fps"  or  "control": {"template": "fps", "move_speed": 8.0}
	if desc.has("control"):
		var c = desc["control"]
		var overrides: Dictionary = desc.get("control_params", {})
		if c is String:
			_Control.apply(vp, c, overrides)
		elif c is Dictionary:
			var tname: String = c.get("template", "orbit")
			var c_copy: Dictionary = c.duplicate()
			c_copy.erase("template")
			c_copy.merge(overrides, true)
			_Control.apply(vp, tname, c_copy)

	# Attach in-game settings panel if requested
	if desc.get("settings_panel", false):
		var PanelClass = load("res://addons/ohao_helpers/ohao_settings_panel.gd")
		var panel = PanelClass.new()
		vp.get_parent().add_child.call_deferred(panel)


## Build walls around a rectangular area.
static func walls(vp, size: Vector3, height: float, color: Color = Color(0.4, 0.4, 0.45)) -> void:
	var hw: float = size.x * 0.5
	var hd: float = size.z * 0.5
	var wall_thickness := 0.2
	# Front wall
	vp.add_cube("WallFront", Vector3(0, height * 0.5, -hd), Vector3.ZERO,
		Vector3(size.x, height, wall_thickness), color)
	# Back wall
	vp.add_cube("WallBack", Vector3(0, height * 0.5, hd), Vector3.ZERO,
		Vector3(size.x, height, wall_thickness), color)
	# Left wall
	vp.add_cube("WallLeft", Vector3(-hw, height * 0.5, 0), Vector3.ZERO,
		Vector3(wall_thickness, height, size.z), color)
	# Right wall
	vp.add_cube("WallRight", Vector3(hw, height * 0.5, 0), Vector3.ZERO,
		Vector3(wall_thickness, height, size.z), color)


## Place a grid of objects.
static func grid(vp, type: String, count_x: int, count_z: int,
		spacing: float, color: Color = Color.WHITE) -> void:
	var offset_x := -(count_x - 1) * spacing * 0.5
	var offset_z := -(count_z - 1) * spacing * 0.5
	for ix in count_x:
		for iz in count_z:
			var pos := Vector3(offset_x + ix * spacing, 0.5, offset_z + iz * spacing)
			var n := "%s_%d_%d" % [type, ix, iz]
			match type:
				"cube": vp.add_cube(n, pos, Vector3.ZERO, Vector3.ONE, color)
				"sphere": vp.add_sphere(n, pos, Vector3.ZERO, Vector3.ONE, color)
				"cylinder": vp.add_cylinder(n, pos, Vector3.ZERO, Vector3.ONE, color)
