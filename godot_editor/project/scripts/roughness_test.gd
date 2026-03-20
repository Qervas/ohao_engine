extends Control
## Roughness precision test — 10 spheres from roughness 0.0 to 1.0
## If the fix works: smooth gradient from mirror-like to matte
## If broken: only 4 distinct looks (0.0, 0.33, 0.67, 1.0)

@onready var vp: OhaoViewport = $OhaoViewport

func _ready() -> void:
	if not vp:
		push_error("No OhaoViewport")
		return
	call_deferred("_build")

func _build() -> void:
	# Floor
	vp.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO, Vector3(30, 1, 20), Color(0.15, 0.15, 0.15))
	vp.set_actor_pbr("Floor", 0.0, 0.9)

	# 10 spheres — roughness 0.0 to 0.9
	for i in 10:
		var roughness: float = float(i) / 9.0
		var name: String = "Sphere_%d" % i
		var x: float = (i - 4.5) * 2.5
		vp.add_sphere(name, Vector3(x, 1.2, 0), Vector3.ZERO, Vector3(1.0, 1.0, 1.0), Color(0.9, 0.1, 0.1))
		vp.set_actor_pbr(name, 0.8, roughness)

	# Second row — same roughness range, different metallic
	for i in 10:
		var roughness: float = float(i) / 9.0
		var name: String = "Sphere_NM_%d" % i
		var x: float = (i - 4.5) * 2.5
		vp.add_sphere(name, Vector3(x, 1.2, -3.5), Vector3.ZERO, Vector3(1.0, 1.0, 1.0), Color(0.8, 0.8, 0.8))
		vp.set_actor_pbr(name, 0.0, roughness)

	# Labels — cubes as markers at each end
	vp.add_cube("Label_Smooth", Vector3(-12.5, 0.3, 2.0), Vector3.ZERO, Vector3(2.0, 0.5, 0.5), Color(0.0, 0.8, 0.0))
	vp.add_cube("Label_Rough", Vector3(12.5, 0.3, 2.0), Vector3.ZERO, Vector3(2.0, 0.5, 0.5), Color(0.8, 0.0, 0.0))

	# Lighting — strong directional + point for specular highlights
	vp.add_directional_light("Sun", Vector3(5, 10, 5), Vector3(-0.3, -0.8, -0.4).normalized(), Color(1.0, 0.95, 0.9), 3.0)
	vp.add_point_light("FillLight", Vector3(0, 5, 8), Color(0.8, 0.85, 1.0), 2.0, 25.0)
	vp.add_point_light("BackLight", Vector3(0, 4, -8), Color(0.6, 0.6, 0.7), 1.5, 20.0)

	vp.finish_sync()

	# Rendering — enable bloom + SSAO for visual quality
	vp.set_bloom_enabled(true)
	vp.set_bloom_threshold(0.6)
	vp.set_bloom_intensity(0.5)
	vp.set_ssao_enabled(true)
	vp.set_ssao_radius(0.5)
	vp.set_ssao_intensity(1.0)
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)

	# Camera — orbit viewing the row of spheres
	vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)
	vp.focus_on_scene()

	print("[Roughness Test] Built — 10 spheres per row, roughness 0.0 → 1.0")
	print("  Top row: metallic=0.8 (red), Bottom row: metallic=0.0 (grey)")
	print("  Left = smooth (mirror), Right = rough (matte)")
