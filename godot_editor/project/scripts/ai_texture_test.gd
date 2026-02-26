extends Control

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

func _ready() -> void:
	if not ohao_viewport:
		push_error("OhaoViewport not found")
		return
	call_deferred("_build_scene")


func _build_scene() -> void:
	var vp := ohao_viewport

	# Connect signals for debug output
	var ai = Engine.get_main_loop().root.get_node_or_null("OhaoAI")
	if ai:
		ai.material_generated.connect(_on_material_generated)
		ai.generation_failed.connect(_on_generation_failed)

	# Build scene: 3 cubes + ground + lighting
	OhaoSceneBuilder.build(vp, {
		"template": "arena",
		"rendering": "bright",
		"objects": [
			{"type": "cube", "name": "StoneCube", "pos": Vector3(-3, 1, 0),
			 "scale": Vector3(2, 2, 2), "color": Color(0.6, 0.6, 0.6),
			 "material": "ai:worn stone wall"},
			{"type": "cube", "name": "MetalCube", "pos": Vector3(0, 1, 0),
			 "scale": Vector3(2, 2, 2), "color": Color(0.5, 0.5, 0.55),
			 "material": "ai:rusty metal plate"},
			{"type": "cube", "name": "WoodCube", "pos": Vector3(3, 1, 0),
			 "scale": Vector3(2, 2, 2), "color": Color(0.5, 0.35, 0.2),
			 "material": "ai:dark wood plank"},
		],
	})

	vp.focus_on_scene()
	print("[AI Texture Test] Scene built — waiting for AI textures...")


func _on_material_generated(description: String, result: Dictionary) -> void:
	print("[AI Texture Test] Generated: '%s' -> %s" % [description, result["albedo_path"]])


func _on_generation_failed(description: String, error: String) -> void:
	push_warning("[AI Texture Test] Failed: '%s' -> %s" % [description, error])
