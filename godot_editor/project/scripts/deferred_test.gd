extends Control

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

func _ready() -> void:
	if not ohao_viewport:
		push_error("OhaoViewport not found")
		return
	call_deferred("_build_scene")

func _build_scene() -> void:
	var vp = ohao_viewport

	# Test deferred mode WITHOUT textures
	vp.set_render_mode(1)  # Deferred
	vp.clear_scene()

	vp.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO, Vector3(20, 1, 20), Color(0.5, 0.5, 0.5))
	vp.add_cube("Box", Vector3(0, 1, 0), Vector3.ZERO, Vector3(2, 2, 2), Color.RED)
	vp.add_directional_light("Sun", Vector3(0, 10, 0), Vector3(-0.5, -1, -0.3), Color.WHITE, 1.5)
	vp.finish_sync()

	vp.set_camera_mode(OhaoConst.CAMERA_FPS)
	print("[DeferredTest] Simple deferred scene built - no textures")
