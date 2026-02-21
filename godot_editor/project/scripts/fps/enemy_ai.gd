class_name EnemyAI
extends Node3D
## Enemy AI with patrol/chase/attack state machine for OHAO Engine FPS
## Uses OhaoPhysicsBody for collision and NavigationAgent3D for pathfinding.

signal enemy_died(enemy: EnemyAI)
signal state_changed(new_state: State)

enum State { IDLE, PATROL, CHASE, ATTACK, PAIN, DEAD }

# Stats
@export var max_health: float = 100.0
@export var move_speed: float = 3.5
@export var chase_speed: float = 5.5
@export var attack_damage: float = 15.0
@export var attack_rate: float = 1.0       # Attacks per second
@export var attack_range: float = 2.0      # Melee range
@export var detection_range: float = 20.0  # See player within this range
@export var lose_sight_range: float = 30.0 # Lose player beyond this range
@export var fov_degrees: float = 120.0     # Field of view for detection

# Patrol config
@export var patrol_points: Array[Vector3] = []
@export var patrol_wait_time: float = 2.0  # Wait at each patrol point

# Internal state
var state: State = State.IDLE
var health: float = 100.0
var target: Node3D  # Usually the player
var current_patrol_index: int = 0
var patrol_wait_timer: float = 0.0
var attack_cooldown: float = 0.0
var pain_timer: float = 0.0
var last_known_player_pos: Vector3

# References
var physics_body: OhaoPhysicsBody
var nav_agent: NavigationAgent3D
var ohao_viewport: OhaoViewport

func _ready() -> void:
	health = max_health
	add_to_group("enemies")

	# Find OhaoViewport
	var node = get_parent()
	while node:
		if node is OhaoViewport:
			ohao_viewport = node
			break
		node = node.get_parent()

	# Find/create physics body
	for child in get_children():
		if child is OhaoPhysicsBody:
			physics_body = child
			break

	# Find/create navigation agent
	for child in get_children():
		if child is NavigationAgent3D:
			nav_agent = child
			break

	if not nav_agent:
		nav_agent = NavigationAgent3D.new()
		nav_agent.path_desired_distance = 0.5
		nav_agent.target_desired_distance = 1.0
		add_child(nav_agent)

	# Setup default patrol if none specified
	if patrol_points.is_empty():
		patrol_points = [
			global_position + Vector3(5, 0, 0),
			global_position + Vector3(5, 0, 5),
			global_position + Vector3(-5, 0, 5),
			global_position + Vector3(-5, 0, 0),
		]

	# Start in patrol state
	_change_state(State.PATROL)

func _physics_process(delta: float) -> void:
	if state == State.DEAD:
		return

	# Update cooldowns
	if attack_cooldown > 0.0:
		attack_cooldown -= delta
	if pain_timer > 0.0:
		pain_timer -= delta
		if pain_timer <= 0.0 and state == State.PAIN:
			_change_state(State.CHASE)
		return

	# Find player target
	if not target:
		_find_player()

	# State machine
	match state:
		State.IDLE:
			_process_idle(delta)
		State.PATROL:
			_process_patrol(delta)
		State.CHASE:
			_process_chase(delta)
		State.ATTACK:
			_process_attack(delta)

func _process_idle(delta: float) -> void:
	if _can_see_player():
		_change_state(State.CHASE)
		return

	# Transition to patrol after a brief idle
	patrol_wait_timer -= delta
	if patrol_wait_timer <= 0.0:
		_change_state(State.PATROL)

func _process_patrol(delta: float) -> void:
	# Check for player
	if _can_see_player():
		_change_state(State.CHASE)
		return

	# Navigate to current patrol point
	if current_patrol_index >= patrol_points.size():
		current_patrol_index = 0

	var target_pos := patrol_points[current_patrol_index]
	nav_agent.target_position = target_pos

	if nav_agent.is_navigation_finished():
		# Arrived at patrol point - wait then move to next
		patrol_wait_timer = patrol_wait_time
		current_patrol_index = (current_patrol_index + 1) % patrol_points.size()
		_change_state(State.IDLE)
		return

	# Move toward next navigation point
	var next_pos := nav_agent.get_next_path_position()
	var direction := (next_pos - global_position).normalized()
	_move(direction, move_speed, delta)

func _process_chase(delta: float) -> void:
	if not target:
		_change_state(State.PATROL)
		return

	var dist := global_position.distance_to(target.global_position)

	# Lost the player
	if dist > lose_sight_range:
		# Go to last known position
		nav_agent.target_position = last_known_player_pos
		if nav_agent.is_navigation_finished():
			_change_state(State.PATROL)
		else:
			var next_pos := nav_agent.get_next_path_position()
			var direction := (next_pos - global_position).normalized()
			_move(direction, chase_speed, delta)
		return

	# Within attack range
	if dist <= attack_range:
		_change_state(State.ATTACK)
		return

	# Chase the player
	last_known_player_pos = target.global_position
	nav_agent.target_position = target.global_position

	var next_pos := nav_agent.get_next_path_position()
	var direction := (next_pos - global_position).normalized()
	_move(direction, chase_speed, delta)

	# Look at player
	_look_at_target(target.global_position, delta)

func _process_attack(delta: float) -> void:
	if not target:
		_change_state(State.PATROL)
		return

	var dist := global_position.distance_to(target.global_position)

	# Out of attack range - chase
	if dist > attack_range * 1.5:
		_change_state(State.CHASE)
		return

	# Face the player
	_look_at_target(target.global_position, delta)

	# Attack on cooldown
	if attack_cooldown <= 0.0:
		_perform_attack()
		attack_cooldown = 1.0 / attack_rate

func _move(direction: Vector3, speed: float, delta: float) -> void:
	if physics_body:
		var vel := direction * speed
		vel.y = physics_body.get_linear_velocity().y  # Preserve gravity
		physics_body.set_linear_velocity(vel)
	else:
		global_position += direction * speed * delta

func _look_at_target(target_pos: Vector3, delta: float) -> void:
	var look_dir := (target_pos - global_position)
	look_dir.y = 0  # Keep upright
	if look_dir.length_squared() > 0.001:
		var target_yaw := atan2(-look_dir.x, -look_dir.z)
		rotation.y = lerp_angle(rotation.y, target_yaw, 5.0 * delta)

func _can_see_player() -> bool:
	if not target:
		return false

	var to_player := target.global_position - global_position
	var dist := to_player.length()

	# Range check
	if dist > detection_range:
		return false

	# FOV check
	var forward := -global_transform.basis.z
	var angle := forward.angle_to(to_player.normalized())
	if rad_to_deg(angle) > fov_degrees * 0.5:
		return false

	# TODO: Line of sight raycast (would need OHAO raycast API)
	return true

func _perform_attack() -> void:
	if not target:
		return

	# Deal damage to player
	if target.has_method("take_damage"):
		var attack_dir := (target.global_position - global_position).normalized()
		target.take_damage(attack_damage, attack_dir)

	# Spawn attack particles (melee impact)
	if ohao_viewport:
		var impact_pos := (global_position + target.global_position) * 0.5
		ohao_viewport.spawn_particles(impact_pos, 1)  # IMPACT_SPARK

func take_damage(amount: float, from_direction: Vector3 = Vector3.ZERO) -> void:
	if state == State.DEAD:
		return

	health -= amount

	if health <= 0.0:
		health = 0.0
		_die(from_direction)
		return

	# Pain reaction
	pain_timer = 0.3
	_change_state(State.PAIN)

	# Spawn blood particles
	if ohao_viewport:
		ohao_viewport.spawn_particles_directed(
			global_position + Vector3(0, 1, 0), 1, from_direction  # IMPACT_SPARK
		)

	# Alert nearby enemies
	_alert_nearby(from_direction)

func _die(impulse_direction: Vector3) -> void:
	_change_state(State.DEAD)
	enemy_died.emit(self)

	# Switch to ragdoll physics
	if physics_body:
		physics_body.set_body_type(0)  # DYNAMIC
		physics_body.apply_impulse(impulse_direction * 10.0)

	# Spawn death particles
	if ohao_viewport:
		ohao_viewport.spawn_particles(global_position + Vector3(0, 1, 0), 2)  # EXPLOSION

	# Remove from enemies group
	remove_from_group("enemies")

	# Queue cleanup after a delay
	get_tree().create_timer(10.0).timeout.connect(queue_free)

func _alert_nearby(threat_direction: Vector3) -> void:
	for enemy in get_tree().get_nodes_in_group("enemies"):
		if enemy == self:
			continue
		if enemy is EnemyAI and enemy.state != State.DEAD:
			var dist := global_position.distance_to(enemy.global_position)
			if dist < detection_range * 0.7:
				enemy.on_alert(global_position)

func on_alert(alert_position: Vector3) -> void:
	if state == State.DEAD or state == State.CHASE or state == State.ATTACK:
		return

	# Go investigate the alert position
	last_known_player_pos = alert_position
	_find_player()
	if target:
		_change_state(State.CHASE)

func _find_player() -> void:
	var players := get_tree().get_nodes_in_group("player")
	if players.size() > 0:
		target = players[0]

func _change_state(new_state: State) -> void:
	state = new_state
	state_changed.emit(new_state)

func is_alive() -> bool:
	return state != State.DEAD
