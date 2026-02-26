class_name GameManager
extends Node
## FPS Game Manager for OHAO Engine
## Orchestrates game state, enemy spawning, scoring, and HUD updates.

signal game_started
signal game_over(victory: bool)
signal score_changed(score: int)
signal wave_started(wave_number: int)
signal enemy_count_changed(alive: int, total: int)

enum GameState { MENU, PLAYING, PAUSED, GAME_OVER }

# Game settings
@export var enable_waves: bool = true
@export var enemies_per_wave: int = 5
@export var wave_increase: int = 2        # Additional enemies per wave
@export var wave_delay: float = 5.0       # Seconds between waves
@export var max_active_enemies: int = 15

# State
var game_state: GameState = GameState.MENU
var score: int = 0
var wave_number: int = 0
var enemies_alive: int = 0
var enemies_total_spawned: int = 0
var wave_timer: float = 0.0

# Spawn points
var spawn_points: Array[Vector3] = []

# References
var player: PlayerController
var ohao_viewport: OhaoViewport

func _ready() -> void:
	add_to_group("game_manager")

	# Find OhaoViewport
	var node = get_parent()
	while node:
		if node is OhaoViewport:
			ohao_viewport = node
			break
		node = node.get_parent()

	# Find player
	call_deferred("_find_player")

func _find_player() -> void:
	var players := get_tree().get_nodes_in_group("player")
	if players.size() > 0:
		player = players[0]
		player.player_died.connect(_on_player_died)

func _process(delta: float) -> void:
	if game_state != GameState.PLAYING:
		return

	# Wave management
	if enable_waves and enemies_alive <= 0:
		wave_timer -= delta
		if wave_timer <= 0.0:
			_start_next_wave()

func start_game() -> void:
	game_state = GameState.PLAYING
	score = 0
	wave_number = 0
	enemies_alive = 0
	enemies_total_spawned = 0
	wave_timer = 1.0  # Brief delay before first wave

	# Start physics
	if ohao_viewport:
		ohao_viewport.play_physics()

	game_started.emit()
	score_changed.emit(score)

func pause_game() -> void:
	if game_state == GameState.PLAYING:
		game_state = GameState.PAUSED
		get_tree().paused = true
		if ohao_viewport:
			ohao_viewport.pause_physics()

func resume_game() -> void:
	if game_state == GameState.PAUSED:
		game_state = GameState.PLAYING
		get_tree().paused = false
		if ohao_viewport:
			ohao_viewport.play_physics()

func end_game(victory: bool) -> void:
	game_state = GameState.GAME_OVER

	if ohao_viewport:
		ohao_viewport.pause_physics()

	game_over.emit(victory)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)

func _start_next_wave() -> void:
	wave_number += 1
	var enemy_count := enemies_per_wave + (wave_number - 1) * wave_increase
	enemy_count = mini(enemy_count, max_active_enemies - enemies_alive)

	wave_started.emit(wave_number)

	for i in range(enemy_count):
		_spawn_enemy()

	wave_timer = wave_delay

func _spawn_enemy() -> void:
	if not ohao_viewport:
		return

	# Choose spawn position
	var spawn_pos := _get_spawn_position()

	# Create enemy — EnemyAI is root so it owns position and finds its children
	var enemy := EnemyAI.new()
	enemy.name = "Enemy_%d" % enemies_total_spawned
	enemy.max_health = 100.0 + wave_number * 10.0  # Scale difficulty
	enemy.attack_damage = 10.0 + wave_number * 2.0
	enemy.move_speed = 3.0 + wave_number * 0.2
	enemy.chase_speed = 5.0 + wave_number * 0.3
	enemy.enemy_died.connect(_on_enemy_died)

	# Visual mesh as child (auto-creates OHAO actor, syncs transform, cleans up)
	var visual := OhaoMeshInstance.new()
	visual.mesh_type = OhaoConst.MESH_CUBE
	visual.mesh_color = Color(0.8, 0.2, 0.2)
	visual.mesh_scale = Vector3(0.5, 1.8, 0.5)
	enemy.add_child(visual)

	# Physics body as child of EnemyAI (so _ready() finds it)
	var phys_body := OhaoPhysicsBody.new()
	phys_body.set_body_type(OhaoConst.BODY_KINEMATIC)
	phys_body.set_shape_type(OhaoConst.SHAPE_CAPSULE)
	phys_body.set_mass(70.0)
	enemy.add_child(phys_body)

	# Add to scene FIRST, then set position (global_position requires tree)
	get_parent().add_child(enemy)
	enemy.global_position = spawn_pos

	enemies_alive += 1
	enemies_total_spawned += 1
	enemy_count_changed.emit(enemies_alive, enemies_total_spawned)

func _get_spawn_position() -> Vector3:
	if spawn_points.size() > 0:
		return spawn_points[randi() % spawn_points.size()]

	# Default: random position in a ring around the player
	var angle := randf() * TAU
	var dist := randf_range(15.0, 25.0)
	var player_pos := player.global_position if player else Vector3.ZERO
	return Vector3(
		player_pos.x + cos(angle) * dist,
		0.0,
		player_pos.z + sin(angle) * dist
	)

func add_spawn_point(pos: Vector3) -> void:
	spawn_points.append(pos)

func on_enemy_hit(actor_name: String, damage: float) -> void:
	# Find enemy by actor name and deal damage
	for enemy_node in get_tree().get_nodes_in_group("enemies"):
		if enemy_node is EnemyAI and enemy_node.name == actor_name:
			var hit_dir := Vector3.ZERO
			if player:
				hit_dir = (enemy_node.global_position - player.global_position).normalized()
			enemy_node.take_damage(damage, hit_dir)
			return

func _on_enemy_died(enemy: EnemyAI) -> void:
	enemies_alive -= 1
	score += 100
	score_changed.emit(score)
	enemy_count_changed.emit(enemies_alive, enemies_total_spawned)

	# Spawn death particles (OhaoMeshInstance auto-cleans up on queue_free)
	if ohao_viewport:
		ohao_viewport.spawn_particles(
			enemy.global_position + Vector3(0, 1, 0),
			2  # EXPLOSION
		)

func _on_player_died() -> void:
	end_game(false)
