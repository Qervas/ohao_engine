class_name OhaoSettings
## Single source of truth for post-processing effects: metadata, parameters,
## stability status, and recommended usage. AI agents read this to discover
## what's available and safely toggle "LEGO block" effects.
##
## Usage:
##   OhaoSettings.apply_effect(vp, "bloom", {"bloom_threshold": 0.4})
##   OhaoSettings.recommended_for(["cyberpunk"])  # → ["bloom", "ssao", "tonemapping"]
##   OhaoSettings.list_stable()                   # → ["bloom", "ssao", "tonemapping"]

const EFFECTS := {
	"bloom": {
		"description": "Glow around bright surfaces. Essential for neon/cyberpunk.",
		"stable": true,
		"toggle": "bloom_enabled",
		"params": {
			"bloom_threshold": {
				"type": "float", "min": 0.1, "max": 3.0, "default": 1.0,
				"description": "Brightness threshold for bloom. Lower = more glow.",
			},
			"bloom_intensity": {
				"type": "float", "min": 0.0, "max": 3.0, "default": 0.5,
				"description": "Bloom glow strength.",
			},
		},
		"recommended_for": ["cyberpunk", "horror", "neon", "sci-fi", "magical"],
	},
	"ssao": {
		"description": "Darkens corners and crevices. Adds depth to indoor scenes.",
		"stable": true,
		"toggle": "ssao_enabled",
		"params": {
			"ssao_radius": {
				"type": "float", "min": 0.1, "max": 2.0, "default": 0.5,
				"description": "Ambient occlusion sample radius.",
			},
			"ssao_intensity": {
				"type": "float", "min": 0.0, "max": 4.0, "default": 1.0,
				"description": "Ambient occlusion darkness strength.",
			},
		},
		"recommended_for": ["indoor", "horror", "realistic", "architectural"],
	},
	"tonemapping": {
		"description": "HDR to display color mapping. Always recommended.",
		"stable": true,
		"toggle": "tonemapping_enabled",
		"params": {
			"tonemap_operator": {
				"type": "int", "min": 0, "max": 3, "default": 0,
				"options": {"ACES": 0, "Reinhard": 1, "Uncharted2": 2, "Neutral": 3},
				"description": "Tonemap curve. ACES=cinematic, Neutral=clean.",
			},
			"exposure": {
				"type": "float", "min": 0.1, "max": 5.0, "default": 1.0,
				"description": "Scene brightness. Lower = darker mood.",
			},
			"gamma": {
				"type": "float", "min": 1.0, "max": 3.0, "default": 2.2,
				"description": "Gamma correction curve.",
			},
		},
		"recommended_for": ["all"],
	},
	"taa": {
		"description": "Temporal anti-aliasing. Smooths jagged edges across frames.",
		"stable": false,
		"toggle": "taa_enabled",
		"params": {
			"taa_blend_factor": {
				"type": "float", "min": 0.0, "max": 1.0, "default": 0.1,
				"description": "Temporal blend. Higher = smoother but more ghosting.",
			},
		},
		"recommended_for": ["realistic", "cinematic"],
	},
	"ssr": {
		"description": "Screen-space reflections on shiny surfaces.",
		"stable": false,
		"toggle": "ssr_enabled",
		"params": {
			"ssr_max_distance": {
				"type": "float", "min": 1.0, "max": 200.0, "default": 50.0,
				"description": "Maximum ray march distance for reflections.",
			},
			"ssr_thickness": {
				"type": "float", "min": 0.01, "max": 5.0, "default": 0.5,
				"description": "Ray thickness for hit detection.",
			},
		},
		"recommended_for": ["realistic", "cinematic", "cyberpunk"],
	},
	"volumetrics": {
		"description": "Volumetric fog and light shafts. Atmospheric depth.",
		"stable": false,
		"toggle": "volumetrics_enabled",
		"params": {
			"volumetric_density": {
				"type": "float", "min": 0.0, "max": 0.2, "default": 0.03,
				"description": "Fog density. Higher = thicker fog.",
			},
			"volumetric_scattering": {
				"type": "float", "min": 0.0, "max": 1.0, "default": 0.5,
				"description": "Forward scattering amount.",
			},
			"fog_color": {
				"type": "color", "default": Color(0.5, 0.5, 0.6),
				"description": "Volumetric fog tint color.",
			},
		},
		"recommended_for": ["horror", "cinematic", "underwater", "foggy"],
	},
	"motion_blur": {
		"description": "Blur during camera/object motion. Cinematic feel.",
		"stable": false,
		"toggle": "motion_blur_enabled",
		"params": {
			"motion_blur_intensity": {
				"type": "float", "min": 0.0, "max": 1.0, "default": 0.5,
				"description": "Blur strength during motion.",
			},
			"motion_blur_samples": {
				"type": "int", "min": 4, "max": 32, "default": 8,
				"description": "Sample count. Higher = smoother blur.",
			},
		},
		"recommended_for": ["cinematic", "racing"],
	},
	"dof": {
		"description": "Depth of field. Blurs objects away from focus point.",
		"stable": false,
		"toggle": "dof_enabled",
		"params": {
			"dof_focus_distance": {
				"type": "float", "min": 0.1, "max": 100.0, "default": 8.0,
				"description": "Distance of the focus plane from camera.",
			},
			"dof_aperture": {
				"type": "float", "min": 0.5, "max": 16.0, "default": 2.8,
				"description": "Aperture size. Lower = more blur.",
			},
			"dof_max_blur": {
				"type": "float", "min": 0.0, "max": 20.0, "default": 6.0,
				"description": "Maximum blur radius in pixels.",
			},
		},
		"recommended_for": ["cinematic", "photo", "cutscene"],
	},
}


## Enable an effect and optionally override its parameters.
## Example: OhaoSettings.apply_effect(vp, "bloom", {"bloom_threshold": 0.4})
static func apply_effect(vp, effect_name: String, overrides: Dictionary = {}) -> void:
	if not EFFECTS.has(effect_name):
		push_warning("OhaoSettings: unknown effect '%s'" % effect_name)
		return
	var effect: Dictionary = EFFECTS[effect_name]

	# Enable the toggle
	vp.call("set_" + effect["toggle"], true)

	# Apply defaults, then overrides
	var params: Dictionary = effect["params"]
	for key in params:
		var val = overrides.get(key, params[key]["default"])
		vp.call("set_" + key, val)


## Disable an effect by its toggle.
static func disable_effect(vp, effect_name: String) -> void:
	if not EFFECTS.has(effect_name):
		push_warning("OhaoSettings: unknown effect '%s'" % effect_name)
		return
	vp.call("set_" + EFFECTS[effect_name]["toggle"], false)


## Get current status of an effect: enabled state + all param values.
## Returns {"enabled": bool, "param_name": value, ...}
static func get_effect_status(vp, effect_name: String) -> Dictionary:
	if not EFFECTS.has(effect_name):
		return {}
	var effect: Dictionary = EFFECTS[effect_name]
	var result := {}

	# Read toggle state
	var toggle_getter := "get_" + effect["toggle"]
	if vp.has_method(toggle_getter):
		result["enabled"] = vp.call(toggle_getter)
	else:
		# Fallback: try is_ prefix
		var is_getter := "is_" + effect["toggle"]
		if vp.has_method(is_getter):
			result["enabled"] = vp.call(is_getter)

	# Read param values
	for key in effect["params"]:
		var getter := "get_" + key
		if vp.has_method(getter):
			result[key] = vp.call(getter)
		else:
			result[key] = effect["params"][key]["default"]

	return result


## Return effect names recommended for any of the given tags.
## Example: recommended_for(["cyberpunk", "neon"]) → ["bloom", "ssao", "tonemapping"]
static func recommended_for(tags: Array) -> Array:
	var result := []
	for effect_name in EFFECTS:
		var rec: Array = EFFECTS[effect_name]["recommended_for"]
		if rec.has("all"):
			result.append(effect_name)
			continue
		for tag in tags:
			if rec.has(tag):
				result.append(effect_name)
				break
	return result


## Return names of stable (safe to enable) effects.
static func list_stable() -> Array:
	var result := []
	for effect_name in EFFECTS:
		if EFFECTS[effect_name]["stable"]:
			result.append(effect_name)
	return result


## Return all effect names.
static func list_all() -> Array:
	return EFFECTS.keys()


## Return full metadata dictionary for a single effect.
static func get_effect_info(effect_name: String) -> Dictionary:
	if EFFECTS.has(effect_name):
		return EFFECTS[effect_name]
	return {}
