extends Control
## FPS Game Scene - Root script for playable FPS mode
## Sets up the OHAO viewport with player, weapons, enemies, and HUD.
## Attach this to a Control node with an OhaoViewport child.

@onready var ohao_viewport: OhaoViewport = $OhaoViewport

var player: PlayerController
var game_manager: GameManager
var hud: FPSHud

func _ready() -> void:
	if not ohao_viewport:
		push_error("FPS Game: OhaoViewport not found")
		return

	# Wait for viewport to initialize
	call_deferred("_setup_game")

func _setup_game() -> void:
	# Build the level geometry in OHAO
	_build_level()

	# Create player
	_create_player()

	# Create game manager
	_create_game_manager()

	# Create HUD
	_create_hud()

	# Configure OHAO post-processing for FPS feel
	_configure_rendering()

	# Start the game
	game_manager.start_game()

func _build_level() -> void:
	ohao_viewport.clear_scene()

	# Ground
	ohao_viewport.add_plane("Ground", Vector3(0, -0.5, 0), Vector3.ZERO,
		Vector3(50, 1, 50), Color(0.3, 0.35, 0.3))

	# Arena walls
	var wall_color := Color(0.5, 0.45, 0.4)
	var wall_height := 4.0
	var arena_size := 25.0

	# North wall
	ohao_viewport.add_cube("WallNorth", Vector3(0, wall_height/2, -arena_size),
		Vector3.ZERO, Vector3(arena_size*2, wall_height, 1), wall_color)
	# South wall
	ohao_viewport.add_cube("WallSouth", Vector3(0, wall_height/2, arena_size),
		Vector3.ZERO, Vector3(arena_size*2, wall_height, 1), wall_color)
	# East wall
	ohao_viewport.add_cube("WallEast", Vector3(arena_size, wall_height/2, 0),
		Vector3.ZERO, Vector3(1, wall_height, arena_size*2), wall_color)
	# West wall
	ohao_viewport.add_cube("WallWest", Vector3(-arena_size, wall_height/2, 0),
		Vector3.ZERO, Vector3(1, wall_height, arena_size*2), wall_color)

	# Cover objects (boxes scattered around)
	var cover_color := Color(0.4, 0.3, 0.25)
	ohao_viewport.add_cube("Cover1", Vector3(5, 1, 5), Vector3.ZERO,
		Vector3(2, 2, 2), cover_color)
	ohao_viewport.add_cube("Cover2", Vector3(-8, 0.75, -3), Vector3.ZERO,
		Vector3(3, 1.5, 1), cover_color)
	ohao_viewport.add_cube("Cover3", Vector3(0, 1.5, -10), Vector3.ZERO,
		Vector3(1, 3, 4), cover_color)
	ohao_viewport.add_cube("Cover4", Vector3(-5, 1, 8), Vector3(0, 0.5, 0),
		Vector3(2, 2, 1), cover_color)
	ohao_viewport.add_cube("Cover5", Vector3(10, 0.5, -8), Vector3.ZERO,
		Vector3(4, 1, 2), cover_color)

	# Pillars
	var pillar_color := Color(0.6, 0.55, 0.5)
	ohao_viewport.add_cylinder("Pillar1", Vector3(8, 2, 0), Vector3.ZERO,
		Vector3(0.5, 4, 0.5), pillar_color)
	ohao_viewport.add_cylinder("Pillar2", Vector3(-8, 2, 0), Vector3.ZERO,
		Vector3(0.5, 4, 0.5), pillar_color)
	ohao_viewport.add_cylinder("Pillar3", Vector3(0, 2, 8), Vector3.ZERO,
		Vector3(0.5, 4, 0.5), pillar_color)
	ohao_viewport.add_cylinder("Pillar4", Vector3(0, 2, -8), Vector3.ZERO,
		Vector3(0.5, 4, 0.5), pillar_color)

	# Lighting
	ohao_viewport.add_directional_light("Sun", Vector3(10, 20, 10),
		Vector3(-0.4, -0.8, -0.3), Color(1.0, 0.95, 0.9), 1.2)
	ohao_viewport.add_point_light("Light1", Vector3(0, 3, 0),
		Color(1.0, 0.9, 0.7), 2.0, 15.0)
	ohao_viewport.add_point_light("Light2", Vector3(10, 3, 10),
		Color(0.7, 0.8, 1.0), 1.5, 12.0)
	ohao_viewport.add_point_light("Light3", Vector3(-10, 3, -10),
		Color(1.0, 0.6, 0.4), 1.5, 12.0)

	ohao_viewport.finish_sync()

func _create_player() -> void:
	player = PlayerController.new()
	player.name = "Player"
	player.global_position = Vector3(0, 0, 15)  # Start near south wall
	player.add_to_group("player")

	# Player physics body (capsule)
	var phys := OhaoPhysicsBody.new()
	phys.set_body_type(2)   # KINEMATIC (player-controlled)
	phys.set_shape_type(2)  # CAPSULE
	phys.set_mass(80.0)
	player.add_child(phys)

	# Weapon system
	var weapons := WeaponSystem.new()
	weapons.name = "WeaponSystem"
	player.add_child(weapons)

	# Add player to viewport's subtree so it can find OhaoViewport
	ohao_viewport.add_child(player)

func _create_game_manager() -> void:
	game_manager = GameManager.new()
	game_manager.name = "GameManager"
	game_manager.enable_waves = true
	game_manager.enemies_per_wave = 3
	game_manager.wave_increase = 2
	game_manager.wave_delay = 5.0

	# Define spawn points (corners and edges of arena)
	game_manager.spawn_points = [
		Vector3(20, 0, 20),
		Vector3(-20, 0, 20),
		Vector3(20, 0, -20),
		Vector3(-20, 0, -20),
		Vector3(15, 0, 0),
		Vector3(-15, 0, 0),
		Vector3(0, 0, -15),
	]

	game_manager.game_over.connect(_on_game_over)
	ohao_viewport.add_child(game_manager)

func _create_hud() -> void:
	hud = FPSHud.new()
	hud.name = "HUD"
	add_child(hud)

func _configure_rendering() -> void:
	# Enable AAA post-processing
	ohao_viewport.set_bloom_enabled(true)
	ohao_viewport.set_bloom_threshold(1.0)
	ohao_viewport.set_bloom_intensity(0.3)

	ohao_viewport.set_taa_enabled(true)

	ohao_viewport.set_ssao_enabled(true)
	ohao_viewport.set_ssao_radius(0.5)
	ohao_viewport.set_ssao_intensity(1.5)

	ohao_viewport.set_motion_blur_enabled(true)
	ohao_viewport.set_motion_blur_intensity(0.3)
	ohao_viewport.set_motion_blur_samples(8)

	ohao_viewport.set_tonemapping_enabled(true)
	ohao_viewport.set_tonemap_operator(2)  # ACES
	ohao_viewport.set_exposure(1.0)
	ohao_viewport.set_gamma(2.2)

	# Set FPS camera mode
	ohao_viewport.set_camera_mode(0)  # FPS

func _on_game_over(victory: bool) -> void:
	if victory:
		print("[OHAO FPS] Victory!")
	else:
		print("[OHAO FPS] Game Over - Score: %d" % game_manager.score)

func _input(event: InputEvent) -> void:
	# Pause with Escape (when mouse is visible)
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		if game_manager and game_manager.game_state == GameManager.GameState.PAUSED:
			game_manager.resume_game()
		elif game_manager and game_manager.game_state == GameManager.GameState.PLAYING:
			if Input.get_mouse_mode() == Input.MOUSE_MODE_VISIBLE:
				game_manager.pause_game()
