extends Control
## Cyberpunk Wave Defense - Neon Arena
## A wave-based FPS shooter in a neon-lit cyberpunk arena.
## Enemies are glowing androids that get tougher each wave.
## WASD to move, mouse to look, LMB to shoot, R to reload, 1/2/3 for weapons.

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

var player: PlayerController
var game_manager: GameManager
var hud: FPSHud
var music_handle: int = 0
var ambient_handle: int = 0

func _ready() -> void:
	if not ohao_viewport:
		push_error("NeonDefense: OhaoViewport not found")
		return
	call_deferred("_setup_game")

func _setup_game() -> void:
	# Switch to game mode: disables C++ editor input + makes Control chain transparent to mouse
	Ohao.enter_game_mode(ohao_viewport)

	_build_level()
	_create_player()
	_customize_weapons()
	_create_game_manager()
	_create_hud()
	_customize_hud()
	_configure_rendering()
	_setup_audio()
	game_manager.start_game()


func _process(_delta: float) -> void:
	if not ohao_viewport:
		return

	# Sync C++ rendering camera from GDScript player state every frame
	if player and player.is_alive:
		var eye_pos := player.position + Vector3(0, 1.6, 0)
		ohao_viewport.set_camera_position(eye_pos)
		var pitch_deg := rad_to_deg(player.camera_pitch)
		var yaw_deg := -rad_to_deg(player.camera_yaw) - 90.0
		ohao_viewport.set_camera_rotation_deg(pitch_deg, yaw_deg)

	# Enemy visual sync handled automatically by OhaoMeshInstance


# ── Level ──────────────────────────────────────────────────────────────

func _build_level() -> void:
	ohao_viewport.clear_scene()

	# Dark metallic floor
	ohao_viewport.add_plane("Floor", Vector3(0, 0, 0), Vector3.ZERO,
		Vector3(30, 1, 30), Color(0.08, 0.08, 0.12))
	OhaoPresets.apply_material(ohao_viewport, "Floor", "metal")

	# Arena walls
	var wall_color := Color(0.12, 0.12, 0.18)
	var wall_h := 5.0
	var arena := 25.0

	ohao_viewport.add_cube("WallNorth", Vector3(0, wall_h / 2, -arena),
		Vector3.ZERO, Vector3(arena * 2, wall_h, 0.5), wall_color)
	ohao_viewport.add_cube("WallSouth", Vector3(0, wall_h / 2, arena),
		Vector3.ZERO, Vector3(arena * 2, wall_h, 0.5), wall_color)
	ohao_viewport.add_cube("WallEast", Vector3(arena, wall_h / 2, 0),
		Vector3.ZERO, Vector3(0.5, wall_h, arena * 2), wall_color)
	ohao_viewport.add_cube("WallWest", Vector3(-arena, wall_h / 2, 0),
		Vector3.ZERO, Vector3(0.5, wall_h, arena * 2), wall_color)

	for w in ["WallNorth", "WallSouth", "WallEast", "WallWest"]:
		OhaoPresets.apply_material(ohao_viewport, w, "concrete")

	# Neon pillars – alternating cyan and magenta
	var pillar_data := [
		[Vector3(8, 2.5, 8),   Color(0.0, 1.0, 0.9)],
		[Vector3(-8, 2.5, 8),  Color(1.0, 0.0, 0.8)],
		[Vector3(8, 2.5, -8),  Color(0.0, 0.8, 1.0)],
		[Vector3(-8, 2.5, -8), Color(0.8, 0.0, 1.0)],
		[Vector3(15, 2.5, 0),  Color(0.0, 1.0, 0.6)],
		[Vector3(-15, 2.5, 0), Color(1.0, 0.2, 0.6)],
		[Vector3(0, 2.5, 15),  Color(0.2, 0.6, 1.0)],
		[Vector3(0, 2.5, -15), Color(0.6, 0.0, 1.0)],
	]
	for i in range(pillar_data.size()):
		var pname := "Pillar_%d" % i
		ohao_viewport.add_cylinder(pname, pillar_data[i][0],
			Vector3.ZERO, Vector3(0.4, 5.0, 0.4), pillar_data[i][1])
		ohao_viewport.set_actor_pbr(pname, 0.9, 0.2)

	# Cover objects – metallic crates
	var covers := [
		[Vector3(4, 0.75, 3),    Vector3(2, 1.5, 2)],
		[Vector3(-6, 1.0, -4),   Vector3(3, 2, 1)],
		[Vector3(10, 0.5, -6),   Vector3(4, 1, 2)],
		[Vector3(-3, 0.75, 10),  Vector3(2, 1.5, 1.5)],
		[Vector3(12, 1.0, 8),    Vector3(1.5, 2, 3)],
		[Vector3(-10, 0.5, -12), Vector3(3, 1, 1.5)],
	]
	for i in range(covers.size()):
		var cname := "Cover_%d" % i
		ohao_viewport.add_cube(cname, covers[i][0], Vector3.ZERO,
			covers[i][1], Color(0.18, 0.2, 0.25))
		OhaoPresets.apply_material(ohao_viewport, cname, "metal")

	# Center platform
	ohao_viewport.add_cube("Platform", Vector3(0, 0.25, 0), Vector3.ZERO,
		Vector3(6, 0.5, 6), Color(0.1, 0.1, 0.15))
	OhaoPresets.apply_material(ohao_viewport, "Platform", "metal")

	# === Lighting ===
	# Cool ambient fill — just enough to see geometry
	ohao_viewport.add_directional_light("Moon", Vector3(0, 20, 0),
		Vector3(-0.3, -1, -0.5), Color(0.15, 0.18, 0.35), 0.8)

	# Neon point lights — cranked intensity + range for bloom to catch
	ohao_viewport.add_point_light("NeonCyan1",    Vector3(8, 3, 8),    Color(0.0, 1.0, 0.9), 8.0, 18.0)
	ohao_viewport.add_point_light("NeonMagenta1", Vector3(-8, 3, 8),   Color(1.0, 0.0, 0.8), 8.0, 18.0)
	ohao_viewport.add_point_light("NeonCyan2",    Vector3(8, 3, -8),   Color(0.0, 0.8, 1.0), 6.0, 16.0)
	ohao_viewport.add_point_light("NeonPurple",   Vector3(-8, 3, -8),  Color(0.8, 0.0, 1.0), 6.0, 16.0)
	ohao_viewport.add_point_light("NeonCenter",   Vector3(0, 4, 0),    Color(0.0, 1.0, 0.8), 5.0, 20.0)
	ohao_viewport.add_point_light("NeonGreen1",   Vector3(15, 2, 0),   Color(0.0, 1.0, 0.6), 5.0, 14.0)
	ohao_viewport.add_point_light("NeonPink1",    Vector3(-15, 2, 0),  Color(1.0, 0.2, 0.6), 5.0, 14.0)
	ohao_viewport.add_point_light("NeonBlue1",    Vector3(0, 2, 15),   Color(0.2, 0.6, 1.0), 4.0, 12.0)
	ohao_viewport.add_point_light("NeonViolet1",  Vector3(0, 2, -15),  Color(0.6, 0.0, 1.0), 4.0, 12.0)

	ohao_viewport.finish_sync()


# ── Player ─────────────────────────────────────────────────────────────

func _create_player() -> void:
	player = PlayerController.new()
	player.name = "Player"
	player.position = Vector3(0, 0, 18)
	player.mouse_sensitivity = 0.004
	player.add_to_group("player")

	var phys := OhaoPhysicsBody.new()
	phys.set_body_type(OhaoConst.BODY_KINEMATIC)
	phys.set_shape_type(OhaoConst.SHAPE_CAPSULE)
	phys.set_mass(80.0)
	player.add_child(phys)

	var weapons := WeaponSystem.new()
	weapons.name = "WeaponSystem"
	player.add_child(weapons)

	ohao_viewport.add_child(player)

func _customize_weapons() -> void:
	var ws: WeaponSystem = player.weapon_system
	if not ws:
		push_warning("NeonDefense: weapon_system not found on player")
		return

	ws.weapons.clear()

	# 1 - Pulse Pistol: fast, accurate, reliable sidearm
	ws.weapons.append(WeaponSystem.WeaponData.new(
		"Pulse Pistol", 30.0, 5.0, 15, 90, 1.2,
		0.5, 1.5, 0.3, 10.0, false, true
	))

	# 2 - Plasma Rifle: full-auto, high rate of fire
	ws.weapons.append(WeaponSystem.WeaponData.new(
		"Plasma Rifle", 20.0, 12.0, 40, 160, 2.0,
		1.5, 1.0, 0.8, 8.0, true, true
	))

	# 3 - Ion Shotgun: devastating close-range spread
	ws.weapons.append(WeaponSystem.WeaponData.new(
		"Ion Shotgun", 15.0, 1.2, 6, 24, 2.8,
		10.0, 5.0, 2.5, 4.0, false, true, 0.0, 25.0
	))

	ws.equip_weapon(0)


# ── Game Manager ───────────────────────────────────────────────────────

func _create_game_manager() -> void:
	game_manager = GameManager.new()
	game_manager.name = "GameManager"
	game_manager.enable_waves = true
	game_manager.enemies_per_wave = 3
	game_manager.wave_increase = 2
	game_manager.wave_delay = 5.0

	game_manager.spawn_points = [
		Vector3(20, 0, 20),  Vector3(-20, 0, 20),
		Vector3(20, 0, -20), Vector3(-20, 0, -20),
		Vector3(18, 0, 0),   Vector3(-18, 0, 0),
		Vector3(0, 0, -18),
	]

	game_manager.game_over.connect(_on_game_over)
	game_manager.wave_started.connect(_on_wave_started)
	game_manager.score_changed.connect(_on_score_changed)
	ohao_viewport.add_child(game_manager)


# ── HUD ────────────────────────────────────────────────────────────────

func _create_hud() -> void:
	hud = FPSHud.new()
	hud.name = "HUD"
	add_child(hud)

func _customize_hud() -> void:
	var cyan := Color(0.0, 1.0, 0.9)
	var magenta := Color(1.0, 0.0, 0.8)

	hud.crosshair_color = cyan
	hud.health_label.add_theme_color_override("font_color", cyan)
	hud.ammo_label.add_theme_color_override("font_color", cyan)
	hud.weapon_label.add_theme_color_override("font_color", Color(0.5, 0.8, 1.0))
	hud.score_label.add_theme_color_override("font_color", magenta)
	hud.wave_label.add_theme_color_override("font_color", magenta)

	# Show current weapon info (defaults would show "Pistol")
	if player.weapon_system and player.weapon_system.current_weapon:
		var w = player.weapon_system.current_weapon
		hud.weapon_label.text = w.name
		hud.ammo_label.text = "%d / %d" % [w.current_ammo, w.reserve_ammo]
	hud.health_label.text = "HP: %d" % int(player.health)


# ── Rendering ──────────────────────────────────────────────────────────

func _configure_rendering() -> void:
	# Start minimal, then enable only stable effects
	OhaoPresets.apply_rendering(ohao_viewport, "minimal")

	# Bloom — aggressive for neon glow (low threshold catches bright neon colors)
	ohao_viewport.set_bloom_enabled(true)
	ohao_viewport.set_bloom_threshold(0.15)
	ohao_viewport.set_bloom_intensity(2.5)

	# SSAO — strong depth in dark arena
	ohao_viewport.set_ssao_enabled(true)
	ohao_viewport.set_ssao_radius(0.7)
	ohao_viewport.set_ssao_intensity(1.5)

	# Volumetric fog — atmospheric neon haze
	ohao_viewport.set_volumetrics_enabled(true)
	ohao_viewport.set_volumetric_density(0.03)
	ohao_viewport.set_volumetric_scattering(0.7)
	ohao_viewport.set_fog_color(Color(0.1, 0.3, 0.5))

	# TAA — smooth edges
	ohao_viewport.set_taa_enabled(true)

	# Tonemapping — bright enough to see geometry, ACES for cinematic contrast
	ohao_viewport.set_tonemapping_enabled(true)
	ohao_viewport.set_tonemap_operator(OhaoConst.TONEMAP_ACES)
	ohao_viewport.set_exposure(1.8)
	ohao_viewport.set_gamma(2.2)


# ── Audio ──────────────────────────────────────────────────────────────

func _setup_audio() -> void:
	# Background music + ambient
	music_handle = Ohao.play_music(OhaoConst.MUSIC_COMBAT, 0.4)
	ambient_handle = Ohao.play_ambient_at(OhaoConst.AMBIENT_HUM, Vector3.ZERO, 0.3)

	# Weapon sounds
	if player and player.weapon_system:
		player.weapon_system.weapon_fired.connect(_on_weapon_fired)
		player.weapon_system.reload_started.connect(_on_reload_started)

	# Player damage sound
	if player:
		player.health_changed.connect(_on_player_hurt)

func _on_weapon_fired(_weapon_data: WeaponSystem.WeaponData) -> void:
	Ohao.play_sfx(OhaoConst.SFX_GUNSHOT, 0.8)

func _on_reload_started(_duration: float) -> void:
	Ohao.play_sfx(OhaoConst.SFX_RELOAD, 0.6)

func _on_player_hurt(_current: float, _maximum: float) -> void:
	Ohao.play_sfx(OhaoConst.SFX_HURT, 0.7)

func _on_wave_started(_wave_num: int) -> void:
	Ohao.play_sfx(OhaoConst.SFX_WHOOSH, 0.5)

func _on_score_changed(_score: int) -> void:
	# Score increases when enemy dies — play explosion
	Ohao.play_sfx(OhaoConst.SFX_EXPLOSION, 0.7)


# ── Events ─────────────────────────────────────────────────────────────

func _on_game_over(victory: bool) -> void:
	if music_handle > 0:
		Ohao.stop_sound(music_handle)
	if ambient_handle > 0:
		Ohao.stop_sound(ambient_handle)

	if victory:
		print("[NeonDefense] VICTORY!")
	else:
		print("[NeonDefense] Game Over — Score: %d" % game_manager.score)

func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		if game_manager and game_manager.game_state == GameManager.GameState.PAUSED:
			game_manager.resume_game()
		elif game_manager and game_manager.game_state == GameManager.GameState.PLAYING:
			if Input.get_mouse_mode() == Input.MOUSE_MODE_VISIBLE:
				game_manager.pause_game()
