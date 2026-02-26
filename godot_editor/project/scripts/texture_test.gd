extends Control

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

func _ready() -> void:
	if not ohao_viewport:
		push_error("OhaoViewport not found")
		return
	call_deferred("_build_scene")

func _build_scene() -> void:
	var vp = ohao_viewport

	vp.clear_scene()

	# Ground plane
	vp.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO, Vector3(20, 1, 20), Color(0.5, 0.5, 0.5))

	# Test cubes with different materials
	var materials = ["stone", "metal", "wood", "concrete", "brick", "grass", "marble", "rust"]
	var x_start = -10.5
	for i in range(materials.size()):
		var mat_name = materials[i]
		var cube_name = "Cube_" + mat_name
		var pos = Vector3(x_start + i * 3.0, 1.0, 0.0)
		vp.add_cube(cube_name, pos, Vector3.ZERO, Vector3(2, 2, 2), Color.WHITE)

	# Lighting
	vp.add_directional_light("Sun", Vector3(0, 10, 0), Vector3(-0.5, -1, -0.3), Color(1, 0.95, 0.9), 1.5)
	vp.add_point_light("FillLight", Vector3(-5, 3, 5), Color(0.4, 0.5, 0.7), 0.6, 20.0)

	vp.finish_sync()

	# Apply material presets
	for i in range(materials.size()):
		var mat_name = materials[i]
		var cube_name = "Cube_" + mat_name
		OhaoPresets.apply_material(vp, cube_name, mat_name)

	# Also apply stone to the floor
	OhaoPresets.apply_material(vp, "Floor", "stone")

	# Apply nice rendering
	OhaoPresets.apply_rendering(vp, "bright")

	# FPS camera for inspection
	vp.set_camera_mode(OhaoConst.CAMERA_FPS)
	vp.set_move_speed(6.0)

	print("[TextureTest] Scene built with 8 material cubes + textured floor")
	print("[TextureTest] Materials: stone, metal, wood, concrete, brick, grass, marble, rust")
