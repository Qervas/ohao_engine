extends Control

# Full post-processing pipeline stress test
# Enables ALL effects progressively and auto-quits after 100 frames

@onready var ohao_viewport: OhaoViewport = $OhaoViewport
var frame_count: int = 0
var scene_built: bool = false

func _ready() -> void:
	if not ohao_viewport:
		push_error("[PipelineTest] OhaoViewport not found")
		get_tree().quit(1)
		return
	call_deferred("_build_scene")

func _build_scene() -> void:
	var vp = ohao_viewport
	vp.set_render_mode(1)  # Deferred
	vp.clear_scene()

	# Floor (metallic for SSR reflections)
	vp.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO, Vector3(30, 1, 30), Color(0.1, 0.1, 0.15))
	vp.set_actor_pbr("Floor", 0.9, 0.2)  # Metallic + smooth

	# Emissive pillars (for bloom)
	vp.add_cylinder("NeonPillar1", Vector3(5, 2, 0), Vector3.ZERO, Vector3(0.3, 4, 0.3), Color(0, 1, 0.9))
	vp.set_actor_pbr("NeonPillar1", 0.9, 0.2)
	vp.add_cylinder("NeonPillar2", Vector3(-5, 2, 0), Vector3.ZERO, Vector3(0.3, 4, 0.3), Color(1, 0, 0.8))
	vp.set_actor_pbr("NeonPillar2", 0.9, 0.2)

	# Regular objects (for SSAO testing)
	vp.add_cube("Box1", Vector3(0, 1, 0), Vector3.ZERO, Vector3(2, 2, 2), Color.RED)
	vp.add_cube("Box2", Vector3(3, 0.5, -2), Vector3(0, 30, 0), Vector3(1, 1, 1), Color.BLUE)
	vp.add_sphere("Sphere1", Vector3(-2, 1, 3), Vector3.ZERO, Vector3(1, 1, 1), Color.GREEN)

	# Lights
	vp.add_directional_light("Sun", Vector3(0, 10, 0), Vector3(-0.3, -1, -0.5), Color.WHITE, 0.5)
	vp.add_point_light("NeonCyan", Vector3(5, 3, 0), Color(0, 1, 0.9), 3.0, 15.0)
	vp.add_point_light("NeonMagenta", Vector3(-5, 3, 0), Color(1, 0, 0.8), 3.0, 15.0)
	vp.add_point_light("CenterLight", Vector3(0, 4, 0), Color(0.8, 0.9, 1), 2.0, 20.0)

	vp.finish_sync()
	vp.set_camera_position(Vector3(0, 3, 10))
	vp.set_camera_rotation_deg(-15, -90)

	# Start with only tonemapping
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(0)  # ACES
	vp.set_exposure(1.2)
	print("[PipelineTest] Scene built - starting with tonemapping only")
	scene_built = true

func _process(_delta: float) -> void:
	if not scene_built:
		return
	frame_count += 1

	match frame_count:
		5:
			print("[PipelineTest] Frame 5 - enabling SSAO")
			ohao_viewport.set_ssao_enabled(true)
			ohao_viewport.set_ssao_radius(0.5)
			ohao_viewport.set_ssao_intensity(1.0)
		15:
			print("[PipelineTest] Frame 15 - enabling Bloom")
			ohao_viewport.set_bloom_enabled(true)
			ohao_viewport.set_bloom_intensity(1.2)
			ohao_viewport.set_bloom_threshold(0.4)
		25:
			print("[PipelineTest] Frame 25 - enabling SSR")
			ohao_viewport.set_ssr_enabled(true)
			ohao_viewport.set_ssr_max_distance(20.0)
			ohao_viewport.set_ssr_thickness(0.5)
		35:
			print("[PipelineTest] Frame 35 - enabling Volumetric Fog")
			ohao_viewport.set_volumetrics_enabled(true)
			ohao_viewport.set_volumetric_density(0.03)
			ohao_viewport.set_volumetric_scattering(0.7)
			ohao_viewport.set_fog_color(Color(0.1, 0.3, 0.5))
		45:
			print("[PipelineTest] Frame 45 - enabling TAA")
			ohao_viewport.set_taa_enabled(true)
			ohao_viewport.set_taa_blend_factor(0.9)
		55:
			print("[PipelineTest] Frame 55 - enabling Motion Blur")
			ohao_viewport.set_motion_blur_enabled(true)
			ohao_viewport.set_motion_blur_intensity(0.6)
			ohao_viewport.set_motion_blur_samples(12)
		65:
			print("[PipelineTest] Frame 65 - enabling DoF")
			ohao_viewport.set_dof_enabled(true)
			ohao_viewport.set_dof_focus_distance(8.0)
			ohao_viewport.set_dof_aperture(2.8)
			ohao_viewport.set_dof_max_blur(6.0)
		75:
			print("[PipelineTest] Frame 75 - ALL 8 effects active, changing camera...")
			ohao_viewport.set_camera_position(Vector3(6, 2, 6))
			ohao_viewport.set_camera_rotation_deg(-10, -135)
		90:
			print("[PipelineTest] Frame 90 - changing exposure...")
			ohao_viewport.set_exposure(2.0)
		100:
			var stats = ohao_viewport.get_render_stats()
			print("[PipelineTest] Frame 100 - SUCCESS! All 8 effects rendered without crash!")
			print("[PipelineTest] Stats: %s" % str(stats))
			get_tree().quit(0)

	if frame_count > 110:
		push_error("[PipelineTest] TIMEOUT")
		get_tree().quit(1)
