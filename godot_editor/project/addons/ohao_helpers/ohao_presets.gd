class_name OhaoPresets
## Rendering atmosphere presets and scene templates.
## Apply with: OhaoPresets.apply_rendering(viewport, "horror")

# === Rendering Presets ===
# Each key maps to an OhaoViewport property name (minus "set_" prefix).

const RENDERING := {
	"horror": {
		"render_mode": 1,
		"bloom_enabled": true, "bloom_threshold": 0.6, "bloom_intensity": 0.8,
		"ssao_enabled": true, "ssao_radius": 0.8, "ssao_intensity": 2.0,
		"volumetrics_enabled": true, "volumetric_density": 0.06, "volumetric_scattering": 0.5,
		"fog_color": Color(0.2, 0.1, 0.3),
		"tonemapping_enabled": true, "tonemap_operator": 0, "exposure": 0.6, "gamma": 2.4,
		"dof_enabled": false, "motion_blur_enabled": false,
		"camera_mode": 0,
	},
	"cyberpunk": {
		"render_mode": 1,
		"bloom_enabled": true, "bloom_threshold": 0.4, "bloom_intensity": 1.2,
		"ssao_enabled": true, "ssao_radius": 0.5, "ssao_intensity": 1.0,
		"volumetrics_enabled": true, "volumetric_density": 0.03, "volumetric_scattering": 0.7,
		"fog_color": Color(0.1, 0.3, 0.5),
		"tonemapping_enabled": true, "tonemap_operator": 0, "exposure": 1.4, "gamma": 2.2,
		"dof_enabled": false, "motion_blur_enabled": false,
		"camera_mode": 0,
	},
	"bright": {
		"render_mode": 1,
		"bloom_enabled": true, "bloom_threshold": 1.5, "bloom_intensity": 0.3,
		"ssao_enabled": true, "ssao_radius": 0.3, "ssao_intensity": 0.5,
		"volumetrics_enabled": false,
		"tonemapping_enabled": true, "tonemap_operator": 3, "exposure": 1.2, "gamma": 2.2,
		"dof_enabled": false, "motion_blur_enabled": false,
		"camera_mode": 1,
	},
	"cinematic": {
		"render_mode": 1,
		"bloom_enabled": true, "bloom_threshold": 0.8, "bloom_intensity": 0.6,
		"ssao_enabled": true, "ssao_radius": 0.5, "ssao_intensity": 1.2,
		"volumetrics_enabled": true, "volumetric_density": 0.015, "volumetric_scattering": 0.6,
		"fog_color": Color(0.5, 0.5, 0.6),
		"tonemapping_enabled": true, "tonemap_operator": 0, "exposure": 1.0, "gamma": 2.2,
		"dof_enabled": true, "dof_focus_distance": 8.0, "dof_aperture": 2.8, "dof_max_blur": 6.0,
		"motion_blur_enabled": true, "motion_blur_intensity": 0.6, "motion_blur_samples": 12,
		"camera_mode": 0,
	},
	"fps_action": {
		"render_mode": 1,
		"bloom_enabled": true, "bloom_threshold": 1.0, "bloom_intensity": 0.5,
		"ssao_enabled": true, "ssao_radius": 0.4, "ssao_intensity": 1.0,
		"volumetrics_enabled": false,
		"tonemapping_enabled": true, "tonemap_operator": 0, "exposure": 1.0, "gamma": 2.2,
		"dof_enabled": false, "motion_blur_enabled": false,
		"camera_mode": 0,
	},
	"minimal": {
		"bloom_enabled": false, "ssao_enabled": false, "ssr_enabled": false,
		"volumetrics_enabled": false, "dof_enabled": false, "motion_blur_enabled": false,
		"taa_enabled": false,
		"tonemapping_enabled": true, "tonemap_operator": 0, "exposure": 1.0, "gamma": 2.2,
	},
}

# === Scene Templates ===
# Each template has "objects" and "lights" arrays.

const TEMPLATES := {
	"arena": {
		"objects": [
			{"type": "plane", "name": "Ground", "pos": Vector3(0, 0, 0), "rot": Vector3.ZERO,
			 "scale": Vector3(20, 1, 20), "color": Color(0.3, 0.3, 0.35)},
		],
		"lights": [
			{"type": "directional", "name": "Sun", "pos": Vector3(0, 10, 0),
			 "dir": Vector3(-0.5, -1, -0.3), "color": Color(1, 0.95, 0.9), "intensity": 1.5},
			{"type": "point", "name": "FillLight", "pos": Vector3(-5, 3, 5),
			 "color": Color(0.4, 0.5, 0.7), "intensity": 0.6, "range": 20.0},
		],
	},
	"corridor": {
		"objects": [
			{"type": "plane", "name": "Floor", "pos": Vector3(0, 0, 0), "rot": Vector3.ZERO,
			 "scale": Vector3(4, 1, 30), "color": Color(0.25, 0.25, 0.3)},
			{"type": "cube", "name": "WallLeft", "pos": Vector3(-2, 1.5, 0), "rot": Vector3.ZERO,
			 "scale": Vector3(0.2, 3, 30), "color": Color(0.35, 0.35, 0.4)},
			{"type": "cube", "name": "WallRight", "pos": Vector3(2, 1.5, 0), "rot": Vector3.ZERO,
			 "scale": Vector3(0.2, 3, 30), "color": Color(0.35, 0.35, 0.4)},
			{"type": "plane", "name": "Ceiling", "pos": Vector3(0, 3, 0), "rot": Vector3(180, 0, 0),
			 "scale": Vector3(4, 1, 30), "color": Color(0.2, 0.2, 0.25)},
		],
		"lights": [
			{"type": "point", "name": "Light1", "pos": Vector3(0, 2.5, -10),
			 "color": Color(1, 0.9, 0.7), "intensity": 1.0, "range": 8.0},
			{"type": "point", "name": "Light2", "pos": Vector3(0, 2.5, 0),
			 "color": Color(1, 0.9, 0.7), "intensity": 1.0, "range": 8.0},
			{"type": "point", "name": "Light3", "pos": Vector3(0, 2.5, 10),
			 "color": Color(1, 0.9, 0.7), "intensity": 1.0, "range": 8.0},
		],
	},
	"outdoor": {
		"objects": [
			{"type": "plane", "name": "Ground", "pos": Vector3(0, 0, 0), "rot": Vector3.ZERO,
			 "scale": Vector3(50, 1, 50), "color": Color(0.3, 0.5, 0.2)},
		],
		"lights": [
			{"type": "directional", "name": "Sun", "pos": Vector3(0, 20, 0),
			 "dir": Vector3(-0.3, -1, -0.5), "color": Color(1, 0.98, 0.95), "intensity": 2.0},
		],
	},
}


# === Material Presets ===
# Each entry: albedo texture, normal map, metallic, roughness.
# Place PBR textures in res://textures/<name>_albedo.png + <name>_normal.png

const MATERIALS := {
	"stone": {"texture": "res://textures/stone_albedo.png", "normal": "res://textures/stone_normal.png", "metallic": 0.0, "roughness": 0.8},
	"metal": {"texture": "res://textures/metal_albedo.png", "normal": "res://textures/metal_normal.png", "metallic": 1.0, "roughness": 0.3},
	"wood": {"texture": "res://textures/wood_albedo.png", "normal": "res://textures/wood_normal.png", "metallic": 0.0, "roughness": 0.6},
	"concrete": {"texture": "res://textures/concrete_albedo.png", "normal": "res://textures/concrete_normal.png", "metallic": 0.0, "roughness": 0.9},
	"brick": {"texture": "res://textures/brick_albedo.png", "normal": "res://textures/brick_normal.png", "metallic": 0.0, "roughness": 0.85},
	"grass": {"texture": "res://textures/grass_albedo.png", "normal": "res://textures/grass_normal.png", "metallic": 0.0, "roughness": 0.95},
	"marble": {"texture": "res://textures/marble_albedo.png", "normal": "res://textures/marble_normal.png", "metallic": 0.0, "roughness": 0.2},
	"rust": {"texture": "res://textures/rust_albedo.png", "normal": "res://textures/rust_normal.png", "metallic": 0.7, "roughness": 0.8},
}


## Apply a PBR material preset to an actor in an OhaoViewport.
static func apply_material(vp, actor_name: String, preset_name: String) -> void:
	if MATERIALS.has(preset_name):
		var mat: Dictionary = MATERIALS[preset_name]
		if mat.has("texture"):
			vp.set_actor_texture(actor_name, mat["texture"])
		if mat.has("normal"):
			vp.set_actor_normal_map(actor_name, mat["normal"])
		vp.set_actor_pbr(actor_name, mat.get("metallic", 0.0), mat.get("roughness", 0.5))
		return
	if preset_name.begins_with("ai:"):
		var desc := preset_name.substr(3).strip_edges()
		var ai_node = Engine.get_main_loop().root.get_node_or_null("OhaoAI")
		if ai_node:
			ai_node.apply_ai_material(vp, actor_name, desc)
		return
	push_warning("OhaoPresets: unknown material preset '%s'" % preset_name)


## Apply a rendering preset to an OhaoViewport.
static func apply_rendering(vp, preset_name: String) -> void:
	if not RENDERING.has(preset_name):
		push_warning("OhaoPresets: unknown rendering preset '%s'" % preset_name)
		return
	var preset: Dictionary = RENDERING[preset_name]
	for key in preset:
		vp.call("set_" + key, preset[key])


## Build a scene template into an OhaoViewport.
## Objects and lights from the template are added, then finish_sync() is called.
static func apply_template(vp, template_name: String) -> void:
	if not TEMPLATES.has(template_name):
		push_warning("OhaoPresets: unknown template '%s'" % template_name)
		return
	var tmpl: Dictionary = TEMPLATES[template_name]
	_add_objects(vp, tmpl.get("objects", []))
	_add_lights(vp, tmpl.get("lights", []))


static func _add_objects(vp, objects: Array) -> void:
	for obj in objects:
		var t: String = obj.get("type", "cube")
		var n: String = obj.get("name", "Obj")
		var p: Vector3 = obj.get("pos", Vector3.ZERO)
		var r: Vector3 = obj.get("rot", Vector3.ZERO)
		var s: Vector3 = obj.get("scale", Vector3.ONE)
		var c: Color = obj.get("color", Color.WHITE)
		match t:
			"cube": vp.add_cube(n, p, r, s, c)
			"sphere": vp.add_sphere(n, p, r, s, c)
			"plane": vp.add_plane(n, p, r, s, c)
			"cylinder": vp.add_cylinder(n, p, r, s, c)
		# Apply material preset if specified
		if obj.has("material"):
			apply_material(vp, n, obj["material"])
		else:
			# Apply individual texture/normal overrides
			if obj.has("texture"):
				vp.set_actor_texture(n, obj["texture"])
			if obj.has("normal"):
				vp.set_actor_normal_map(n, obj["normal"])


static func _add_lights(vp, lights: Array) -> void:
	for l in lights:
		var t: String = l.get("type", "point")
		var n: String = l.get("name", "Light")
		var p: Vector3 = l.get("pos", Vector3.ZERO)
		var c: Color = l.get("color", Color.WHITE)
		var i: float = l.get("intensity", 1.0)
		match t:
			"directional":
				var d: Vector3 = l.get("dir", Vector3(0, -1, 0))
				vp.add_directional_light(n, p, d, c, i)
			"point":
				var r: float = l.get("range", 10.0)
				vp.add_point_light(n, p, c, i, r)
