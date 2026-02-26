extends "res://scripts/fps/game_manager.gd"
## Game manager that spawns neon-colored android enemies with audio feedback.

var neon_enemy_colors := [
	Color(1.0, 0.0, 0.6),   # hot pink
	Color(0.8, 0.0, 1.0),   # purple
	Color(1.0, 0.3, 0.0),   # orange
	Color(0.6, 0.0, 0.8),   # dark purple
	Color(1.0, 0.0, 0.4),   # red-pink
]

func _spawn_enemy() -> void:
	if not ohao_viewport:
		return

	var spawn_pos := _get_spawn_position()

	# EnemyAI IS the root — its name matches the OHAO actor name
	var enemy := EnemyAI.new()
	enemy.name = "Enemy_%d" % enemies_total_spawned
	enemy.max_health = 80.0 + wave_number * 15.0
	enemy.attack_damage = 10.0 + wave_number * 3.0
	enemy.move_speed = 3.5 + wave_number * 0.3
	enemy.chase_speed = 5.5 + wave_number * 0.4
	enemy.detection_range = 22.0
	enemy.enemy_died.connect(_on_enemy_died)

	# Neon-colored visual mesh (auto-creates OHAO actor, syncs transform, cleans up)
	var enemy_color := neon_enemy_colors[enemies_total_spawned % neon_enemy_colors.size()]
	var visual := OhaoMeshInstance.new()
	visual.mesh_type = OhaoConst.MESH_CUBE
	visual.mesh_color = enemy_color
	visual.mesh_scale = Vector3(0.5, 1.8, 0.5)
	visual.metallic = 0.7
	visual.roughness = 0.3
	enemy.add_child(visual)

	# Physics body as CHILD of EnemyAI (so _ready() finds it)
	var phys_body := OhaoPhysicsBody.new()
	phys_body.set_body_type(OhaoConst.BODY_KINEMATIC)
	phys_body.set_shape_type(OhaoConst.SHAPE_CAPSULE)
	phys_body.set_mass(70.0)
	enemy.add_child(phys_body)

	# Add to scene tree FIRST, then set position (global_position requires tree)
	get_parent().add_child(enemy)
	enemy.global_position = spawn_pos

	enemies_alive += 1
	enemies_total_spawned += 1
	enemy_count_changed.emit(enemies_alive, enemies_total_spawned)

func _on_enemy_died(enemy: EnemyAI) -> void:
	enemies_alive -= 1
	score += 100
	score_changed.emit(score)
	enemy_count_changed.emit(enemies_alive, enemies_total_spawned)

	# Death particles (OhaoMeshInstance auto-cleans up on queue_free)
	if ohao_viewport:
		var pos := enemy.global_position + Vector3(0, 1, 0)
		ohao_viewport.spawn_particles(pos, OhaoConst.PARTICLE_EXPLOSION)

	# Explosion sound at enemy position
	Ohao.play_sfx_at(OhaoConst.SFX_EXPLOSION, enemy.global_position, 0.9)
