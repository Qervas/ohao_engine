extends Control

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

var ssgi_on := false
var frame_count := 0

func _ready() -> void:
	if not ohao_viewport:
		push_error("OhaoViewport not found")
		return
	call_deferred("_build_scene")

func _build_scene() -> void:
	var vp = ohao_viewport

	vp.set_render_mode(1)  # Deferred (required for SSGI)
	vp.clear_scene()

	# White floor - should pick up color bleeding from walls
	vp.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO, Vector3(20, 1, 20), Color(0.9, 0.9, 0.9))

	# Red wall - left side
	vp.add_cube("RedWall", Vector3(-4, 2, 0), Vector3.ZERO, Vector3(0.5, 4, 8), Color(0.9, 0.1, 0.1))

	# Green wall - right side
	vp.add_cube("GreenWall", Vector3(4, 2, 0), Vector3.ZERO, Vector3(0.5, 4, 8), Color(0.1, 0.9, 0.1))

	# Blue wall - back
	vp.add_cube("BlueWall", Vector3(0, 2, -4), Vector3.ZERO, Vector3(8, 4, 0.5), Color(0.1, 0.1, 0.9))

	# White ceiling
	vp.add_plane("Ceiling", Vector3(0, 4, 0), Vector3(180, 0, 0), Vector3(20, 1, 20), Color(0.9, 0.9, 0.9))

	# White box in center - should show color bleeding from all walls
	vp.add_cube("CenterBox", Vector3(0, 1, 0), Vector3.ZERO, Vector3(2, 2, 2), Color(0.95, 0.95, 0.95))

	# Lights
	vp.add_point_light("Light1", Vector3(0, 3.5, 0), Color.WHITE, 3.0, 20.0)
	vp.add_directional_light("Sun", Vector3(0, 10, 5), Vector3(-0.3, -1, -0.5), Color.WHITE, 0.5)

	vp.finish_sync()

	# Camera setup - look into the cornell box
	vp.set_camera_mode(OhaoConst.CAMERA_FPS)
	vp.set_camera_position(Vector3(0, 2, 8))

	# Enable baseline rendering
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)
	vp.set_exposure(1.0)
	vp.set_ssao_enabled(true)

	# Start with SSGI OFF so user can see the difference
	vp.set_ssgi_enabled(false)

	print("")
	print("=== SSGI Test Scene (Cornell Box) ===")
	print("Press SPACE to toggle SSGI on/off")
	print("Press 1-4 to change sample count (1/4/8/16)")
	print("Press +/- to adjust intensity")
	print("Press R to adjust radius")
	print("SSGI is currently OFF")
	print("=====================================")
	print("")

func _unhandled_input(event: InputEvent) -> void:
	if not ohao_viewport:
		return

	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_SPACE:
				ssgi_on = !ssgi_on
				ohao_viewport.set_ssgi_enabled(ssgi_on)
				print("[SSGI] %s | radius=%.1f intensity=%.1f samples=%d" % [
					"ON" if ssgi_on else "OFF",
					ohao_viewport.get_ssgi_radius(),
					ohao_viewport.get_ssgi_intensity(),
					ohao_viewport.get_ssgi_sample_count(),
				])

			KEY_1:
				ohao_viewport.set_ssgi_sample_count(1)
				print("[SSGI] samples = 1 (fast, noisy)")
			KEY_2:
				ohao_viewport.set_ssgi_sample_count(4)
				print("[SSGI] samples = 4 (default)")
			KEY_3:
				ohao_viewport.set_ssgi_sample_count(8)
				print("[SSGI] samples = 8 (quality)")
			KEY_4:
				ohao_viewport.set_ssgi_sample_count(16)
				print("[SSGI] samples = 16 (max quality)")

			KEY_EQUAL:  # + key
				var intensity = ohao_viewport.get_ssgi_intensity() + 0.2
				intensity = min(intensity, 3.0)
				ohao_viewport.set_ssgi_intensity(intensity)
				print("[SSGI] intensity = %.1f" % intensity)
			KEY_MINUS:
				var intensity = ohao_viewport.get_ssgi_intensity() - 0.2
				intensity = max(intensity, 0.0)
				ohao_viewport.set_ssgi_intensity(intensity)
				print("[SSGI] intensity = %.1f" % intensity)

			KEY_R:
				var radius = ohao_viewport.get_ssgi_radius()
				radius = radius + 1.0 if radius < 10.0 else 1.0
				ohao_viewport.set_ssgi_radius(radius)
				print("[SSGI] radius = %.1f" % radius)
