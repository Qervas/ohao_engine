extends Control
## Audio system test scene
## Tests 2D sounds, 3D positional audio, ambient loops, and category volume control.

var vp: OhaoViewport
var music_handle: int = 0
var ambient_handle: int = 0

func _ready() -> void:
	# Wait for viewport to initialize
	await get_tree().create_timer(0.5).timeout

	vp = Ohao.viewport(self)
	if not vp:
		push_error("OhaoViewport not found!")
		return

	# Build a simple scene
	vp.clear_scene()
	vp.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO, Vector3(20, 1, 20), Color(0.3, 0.3, 0.35))
	vp.add_cube("SoundSource", Vector3(5, 1, 0), Vector3.ZERO, Vector3(1, 1, 1), Color.RED)
	vp.add_cube("AmbientSource", Vector3(-5, 1, -3), Vector3.ZERO, Vector3(0.5, 2, 0.5), Color.BLUE)
	vp.add_point_light("Light1", Vector3(0, 5, 0), Color.WHITE, 3.0, 20.0)
	vp.finish_sync()

	# Set up camera
	vp.set_camera_mode(OhaoConst.CAMERA_FPS)

	print("[Audio Test] Scene ready. Auto-playing test sound...")

	# Auto-play a sound on startup to verify audio works
	var test_handle = vp.play_sound("res://sounds/sfx/gunshot.wav", OhaoConst.AUDIO_SFX, false, 1.0)
	print("[Audio Test] play_sound returned handle: ", test_handle)

	print("[Audio Test] Press keys to test audio:")
	print("  1 = Play 2D gunshot (non-positional)")
	print("  2 = Play 3D gunshot at red cube (5, 1, 0)")
	print("  3 = Toggle ambient loop at blue cube (-5, 1, -3)")
	print("  4 = Toggle background music")
	print("  5 = Mute SFX category")
	print("  6 = Unmute SFX category")
	print("  7 = Set master volume to 50%")
	print("  8 = Set master volume to 100%")
	print("  0 = Stop all sounds")


func _input(event: InputEvent) -> void:
	if not vp:
		return

	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_1:
				print("[Audio Test] Playing 2D gunshot")
				Ohao.play_sfx(OhaoConst.SFX_GUNSHOT)

			KEY_2:
				print("[Audio Test] Playing 3D gunshot at (5, 1, 0)")
				Ohao.play_sfx_at(OhaoConst.SFX_GUNSHOT, Vector3(5, 1, 0))

			KEY_3:
				if ambient_handle == 0:
					print("[Audio Test] Starting ambient loop at (-5, 1, -3)")
					ambient_handle = Ohao.play_ambient_at(OhaoConst.AMBIENT_DRIP, Vector3(-5, 1, -3))
				else:
					print("[Audio Test] Stopping ambient loop")
					Ohao.stop_sound(ambient_handle)
					ambient_handle = 0

			KEY_4:
				if music_handle == 0:
					print("[Audio Test] Starting background music")
					music_handle = Ohao.play_music(OhaoConst.MUSIC_AMBIENT_DARK, 0.5)
				else:
					print("[Audio Test] Stopping background music")
					Ohao.stop_sound(music_handle)
					music_handle = 0

			KEY_5:
				print("[Audio Test] Muting SFX category")
				vp.set_category_volume(OhaoConst.AUDIO_SFX, 0.0)

			KEY_6:
				print("[Audio Test] Unmuting SFX category")
				vp.set_category_volume(OhaoConst.AUDIO_SFX, 1.0)

			KEY_7:
				print("[Audio Test] Master volume 50%")
				vp.set_master_volume(0.5)

			KEY_8:
				print("[Audio Test] Master volume 100%")
				vp.set_master_volume(1.0)

			KEY_0:
				print("[Audio Test] Stopping all sounds")
				vp.stop_all_sounds()
				music_handle = 0
				ambient_handle = 0
