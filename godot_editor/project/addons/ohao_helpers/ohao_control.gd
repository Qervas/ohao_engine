class_name OhaoControl
## Agent-friendly control template system. Predefines interaction modes for
## different game types and simulations. AI agents select, query, and tweak
## templates declaratively — analogous to UE5 GameMode blueprints but for agents.
##
## Templates cover camera mode, input handling, and per-template setup.
## Each template has discoverable params with ranges so agents know what to tweak.
##
## Usage:
##   OhaoControl.apply(vp, "fps")
##   OhaoControl.apply(vp, "fps", {"move_speed": 8.0, "mouse_sensitivity": 0.5})
##   OhaoControl.list()                          # → ["fps", "orbit", "rts", ...]
##   OhaoControl.get_info("fps")                 # → full dict: desc, params, tags
##   OhaoControl.get_params("fps")               # → tweakable params with ranges
##   OhaoControl.recommended_for(["shooter"])    # → ["fps"]
##   OhaoControl.get_active(vp)                  # → "fps" (or "" if none applied)
##   OhaoControl.reset(vp)                       # → editor defaults

const TEMPLATES := {
	"fps": {
		"description": "First-person shooter. WASD movement, mouse look. Best for action/horror/exploration.",
		"tags": ["action", "shooter", "horror", "exploration", "fps", "stealth"],
		"camera_mode": OhaoConst.CAMERA_FPS,
		"input_mode": OhaoConst.INPUT_GAME,
		"params": {
			"move_speed": {
				"type": "float", "min": 1.0, "max": 20.0, "default": 5.0,
				"method": "set_move_speed",
				"description": "Player movement speed in m/s. 5=walk, 8=jog, 12=run.",
			},
			"mouse_sensitivity": {
				"type": "float", "min": 0.05, "max": 2.0, "default": 0.3,
				"method": "set_mouse_sensitivity",
				"description": "Mouse look sensitivity. 0.3=default, 0.1=slow, 0.8=fast.",
			},
		},
		"recommended_for": ["action", "shooter", "horror", "exploration", "fps", "stealth", "survival"],
	},
	"orbit": {
		"description": "3D model viewer. Mouse drag to orbit, scroll to zoom. Best for demos/inspection.",
		"tags": ["viewer", "inspector", "demo", "editor", "showcase"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_EDITOR,
		"params": {
			"move_speed": {
				"type": "float", "min": 0.5, "max": 20.0, "default": 5.0,
				"method": "set_move_speed",
				"description": "Orbit/pan speed multiplier.",
			},
			"mouse_sensitivity": {
				"type": "float", "min": 0.05, "max": 1.0, "default": 0.3,
				"method": "set_mouse_sensitivity",
				"description": "Orbit drag sensitivity.",
			},
		},
		"recommended_for": ["viewer", "demo", "inspection", "showcase", "model", "editor"],
	},
	"rts": {
		"description": "Real-time strategy. Angled top-down camera, WASD to pan. Click to select objects.",
		"tags": ["strategy", "rts", "tower_defense", "god_game", "simulation"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_EDITOR,
		"params": {
			"move_speed": {
				"type": "float", "min": 1.0, "max": 50.0, "default": 15.0,
				"method": "set_move_speed",
				"description": "Camera pan speed for WASD movement.",
			},
			"camera_height": {
				"type": "float", "min": 5.0, "max": 80.0, "default": 20.0,
				"description": "Camera height above scene. 20=close, 50=zoomed out.",
			},
			"camera_angle": {
				"type": "float", "min": -89.0, "max": -30.0, "default": -55.0,
				"description": "Pitch angle in degrees. -55=isometric, -89=top-down.",
			},
		},
		"recommended_for": ["strategy", "rts", "tower_defense", "god_game", "simulation", "4x"],
	},
	"physics_sandbox": {
		"description": "Physics playground. Orbit camera, click to select objects. Physics simulation running.",
		"tags": ["sandbox", "physics", "playground", "testing", "destruction"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_EDITOR,
		"params": {
			"gravity_scale": {
				"type": "float", "min": 0.0, "max": 5.0, "default": 1.0,
				"description": "Global gravity multiplier. 0=zero-G, 0.5=moon, 1=Earth, 2=heavy planet.",
			},
			"move_speed": {
				"type": "float", "min": 1.0, "max": 20.0, "default": 5.0,
				"method": "set_move_speed",
				"description": "Camera orbit/pan speed.",
			},
		},
		"recommended_for": ["sandbox", "physics", "destruction", "testing", "demo", "simulation"],
	},
	"cinematic": {
		"description": "Cutscene/trailer mode. Camera auto-frames the scene. No player movement. Best for showcases.",
		"tags": ["cinematic", "demo", "showcase", "cutscene", "trailer"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_EDITOR,
		"params": {
			"focus_on_scene": {
				"type": "bool", "default": true,
				"description": "Auto-frame camera to fit entire scene on apply.",
			},
			"mouse_sensitivity": {
				"type": "float", "min": 0.02, "max": 0.5, "default": 0.08,
				"method": "set_mouse_sensitivity",
				"description": "Minimal drag sensitivity for manual repositioning.",
			},
		},
		"recommended_for": ["cinematic", "demo", "showcase", "cutscene", "trailer", "portfolio"],
	},
	"vehicle": {
		"description": "Vehicle/driving control. Orbit chase camera. WASD for throttle/steering.",
		"tags": ["vehicle", "racing", "driving", "car", "mech"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_GAME,
		"params": {
			"move_speed": {
				"type": "float", "min": 5.0, "max": 100.0, "default": 20.0,
				"method": "set_move_speed",
				"description": "Camera speed scale for following vehicle.",
			},
			"mouse_sensitivity": {
				"type": "float", "min": 0.1, "max": 1.0, "default": 0.2,
				"method": "set_mouse_sensitivity",
				"description": "Camera look sensitivity.",
			},
		},
		"recommended_for": ["racing", "driving", "vehicle", "car", "mech", "spaceship"],
	},
	"top_down": {
		"description": "Overhead/isometric view. Fixed top-down camera. Best for puzzle/RPG/tactical.",
		"tags": ["top_down", "isometric", "rpg", "puzzle", "tactical", "overhead"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_EDITOR,
		"params": {
			"camera_height": {
				"type": "float", "min": 5.0, "max": 100.0, "default": 20.0,
				"description": "Camera height above scene center.",
			},
			"move_speed": {
				"type": "float", "min": 1.0, "max": 30.0, "default": 8.0,
				"method": "set_move_speed",
				"description": "Camera pan speed.",
			},
		},
		"recommended_for": ["rpg", "puzzle", "isometric", "top_down", "adventure", "tactical", "dungeon"],
	},
	"puzzle": {
		"description": "Point-and-click / puzzle. Orbit view. Click to select and interact with objects.",
		"tags": ["puzzle", "adventure", "point_click", "escape_room"],
		"camera_mode": OhaoConst.CAMERA_ORBIT,
		"input_mode": OhaoConst.INPUT_EDITOR,
		"params": {
			"mouse_sensitivity": {
				"type": "float", "min": 0.02, "max": 0.5, "default": 0.1,
				"method": "set_mouse_sensitivity",
				"description": "Orbit drag sensitivity. Low = deliberate, precise rotation.",
			},
			"move_speed": {
				"type": "float", "min": 0.5, "max": 5.0, "default": 1.0,
				"method": "set_move_speed",
				"description": "Slow camera movement for careful inspection.",
			},
		},
		"recommended_for": ["puzzle", "adventure", "point_click", "escape_room", "mystery"],
	},
}

const _META_KEY := "_ohao_control_template"


## Apply a named control template to the viewport.
## overrides: partial dict of param_name → value. Unspecified params use defaults.
##
## Example:
##   OhaoControl.apply(vp, "fps", {"move_speed": 8.0})
##   OhaoControl.apply(vp, "rts", {"camera_height": 35.0, "camera_angle": -70.0})
static func apply(vp, template_name: String, overrides: Dictionary = {}) -> void:
	if not TEMPLATES.has(template_name):
		push_warning("OhaoControl: unknown template '%s'. Call list() to see available templates." % template_name)
		return

	var tmpl: Dictionary = TEMPLATES[template_name]

	# Camera mode
	vp.set_camera_mode(tmpl["camera_mode"])

	# Input mode (with mouse filter fixup for game mode)
	_set_input_mode(vp, tmpl["input_mode"])

	# Apply params that have a direct viewport method call
	var params: Dictionary = tmpl["params"]
	for key in params:
		var p: Dictionary = params[key]
		if not p.has("method") or p["method"] == null:
			continue  # handled below in _apply_extra
		var val = overrides.get(key, p["default"])
		vp.call(p["method"], val)

	# Template-specific extra setup (camera position, gravity, etc.)
	_apply_extra(vp, template_name, overrides)

	# Track active template as viewport metadata
	vp.set_meta(_META_KEY, template_name)


## Reset viewport to editor defaults.
## Equivalent to: orbit camera, editor input, default move speed and sensitivity.
static func reset(vp) -> void:
	_set_input_mode(vp, OhaoConst.INPUT_EDITOR)
	vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)
	vp.set_move_speed(5.0)
	vp.set_mouse_sensitivity(0.3)
	if vp.has_meta(_META_KEY):
		vp.remove_meta(_META_KEY)


## Return the name of the currently applied template, or "" if none.
static func get_active(vp) -> String:
	if vp.has_meta(_META_KEY):
		return vp.get_meta(_META_KEY)
	return ""


## Return all template names.
static func list() -> Array:
	return TEMPLATES.keys()


## Return template names that match any of the given scenario keywords.
## Example: OhaoControl.recommended_for(["shooter", "action"]) → ["fps"]
## Example: OhaoControl.recommended_for(["puzzle"]) → ["puzzle", "top_down"]
static func recommended_for(scenario_tags: Array) -> Array:
	var result := []
	for tname in TEMPLATES:
		var rec: Array = TEMPLATES[tname]["recommended_for"]
		for tag in scenario_tags:
			if rec.has(tag):
				result.append(tname)
				break
	return result


## Return full template info: description, tags, params with ranges.
## Example: OhaoControl.get_info("fps")["params"]["move_speed"]["max"]  → 20.0
static func get_info(template_name: String) -> Dictionary:
	if TEMPLATES.has(template_name):
		return TEMPLATES[template_name].duplicate(true)
	return {}


## Return just the tweakable params dict for a template.
## Format: {"param_name": {"type": "float", "min": ..., "max": ..., "default": ..., "description": ...}}
## Agents use this to know what they can change and what valid ranges look like.
static func get_params(template_name: String) -> Dictionary:
	if TEMPLATES.has(template_name):
		return TEMPLATES[template_name]["params"].duplicate(true)
	return {}


## Return a summary dict of all templates: name → {description, tags, param_names}.
## Agents call this first to decide which template fits their scenario.
static func catalog() -> Dictionary:
	var result := {}
	for tname in TEMPLATES:
		var tmpl: Dictionary = TEMPLATES[tname]
		result[tname] = {
			"description": tmpl["description"],
			"tags": tmpl["tags"],
			"params": tmpl["params"].keys(),
			"recommended_for": tmpl["recommended_for"],
		}
	return result


# === Internal helpers ===

static func _set_input_mode(vp, mode: int) -> void:
	if mode == OhaoConst.INPUT_GAME:
		vp.set_input_mode(OhaoConst.INPUT_GAME)
		# Make parent Controls transparent so mouse events reach _unhandled_input
		var node: Node = vp.get_parent()
		while node:
			if node is Control:
				node.set_meta("_ohao_prev_mouse_filter", node.mouse_filter)
				node.mouse_filter = Control.MOUSE_FILTER_IGNORE
			node = node.get_parent()
	else:
		vp.set_input_mode(OhaoConst.INPUT_EDITOR)
		# Restore parent Controls' mouse filters
		var node: Node = vp.get_parent()
		while node:
			if node is Control and node.has_meta("_ohao_prev_mouse_filter"):
				node.mouse_filter = node.get_meta("_ohao_prev_mouse_filter")
				node.remove_meta("_ohao_prev_mouse_filter")
			node = node.get_parent()


static func _apply_extra(vp, template_name: String, overrides: Dictionary) -> void:
	match template_name:
		"physics_sandbox":
			# Set global gravity if set_gravity is available (requires OhaoViewport binding)
			var scale: float = overrides.get("gravity_scale", 1.0)
			if vp.has_method("set_gravity"):
				vp.set_gravity(Vector3(0.0, -9.81 * scale, 0.0))
		"cinematic":
			# Auto-frame camera to fit scene
			var do_focus: bool = overrides.get("focus_on_scene", true)
			if do_focus:
				vp.focus_on_scene()
		"rts":
			# Angle camera for isometric RTS view
			var height: float = overrides.get("camera_height", 20.0)
			var angle: float = overrides.get("camera_angle", -55.0)
			vp.set_camera_position(Vector3(0.0, height, height * 0.45))
			vp.set_camera_rotation_deg(angle, 0.0)
		"top_down":
			# Strict overhead camera
			var height: float = overrides.get("camera_height", 20.0)
			vp.set_camera_position(Vector3(0.0, height, 0.1))
			vp.set_camera_rotation_deg(-89.0, 0.0)
