class_name WeaponSystem
extends Node
## Weapon management system for OHAO Engine FPS
## Handles weapon switching, firing, reloading, and visual effects.

signal weapon_changed(weapon_data: WeaponData)
signal ammo_changed(current: int, reserve: int)
signal weapon_fired(weapon_data: WeaponData)
signal reload_started(duration: float)
signal reload_finished

# Weapon data resource
class WeaponData:
	var name: String
	var damage: float
	var fire_rate: float          # Shots per second
	var magazine_size: int
	var reserve_ammo: int
	var reload_time: float        # Seconds
	var spread: float             # Degrees of spread
	var recoil_vertical: float    # Degrees per shot
	var recoil_horizontal: float  # Degrees per shot (random +/-)
	var recoil_recovery: float    # Degrees per second recovery
	var is_automatic: bool
	var is_hitscan: bool          # true = hitscan, false = projectile
	var projectile_speed: float   # Only for projectile weapons
	var range_max: float          # Max effective range
	var particle_type: int        # 0=MUZZLE_FLASH, 1=IMPACT_SPARK, 2=EXPLOSION, 3=SMOKE
	var current_ammo: int

	func _init(n: String, dmg: float, rate: float, mag: int, reserve: int,
			   rld: float, sprd: float, rv: float, rh: float, rr: float,
			   auto_fire: bool, hitscan: bool, proj_speed: float = 0.0,
			   rng: float = 100.0, particle: int = 0) -> void:
		name = n
		damage = dmg
		fire_rate = rate
		magazine_size = mag
		reserve_ammo = reserve
		reload_time = rld
		spread = sprd
		recoil_vertical = rv
		recoil_horizontal = rh
		recoil_recovery = rr
		is_automatic = auto_fire
		is_hitscan = hitscan
		projectile_speed = proj_speed
		range_max = rng
		particle_type = particle
		current_ammo = mag

# Weapon inventory
var weapons: Array[WeaponData] = []
var current_weapon_index: int = -1
var current_weapon: WeaponData

# State
var can_fire: bool = true
var is_reloading: bool = false
var fire_cooldown: float = 0.0
var reload_timer: float = 0.0
var accumulated_recoil: Vector2 = Vector2.ZERO

# References
var player: PlayerController
var ohao_viewport: OhaoViewport

func _ready() -> void:
	# Find player controller (parent)
	if get_parent() is PlayerController:
		player = get_parent()

	# Find OhaoViewport
	var node = get_parent()
	while node:
		if node is OhaoViewport:
			ohao_viewport = node
			break
		node = node.get_parent()

	# Setup default weapon loadout
	_setup_default_weapons()

	# Equip first weapon
	if weapons.size() > 0:
		equip_weapon(0)

func _setup_default_weapons() -> void:
	# Pistol - semi-auto, reliable, low recoil
	weapons.append(WeaponData.new(
		"Pistol", 25.0, 4.0, 12, 60, 1.5,
		1.0, 2.0, 0.5, 8.0, false, true
	))

	# Assault Rifle - full-auto, medium damage, moderate recoil
	weapons.append(WeaponData.new(
		"Assault Rifle", 18.0, 10.0, 30, 120, 2.5,
		2.0, 1.5, 1.0, 6.0, true, true
	))

	# Shotgun - semi-auto, high damage, high spread, close range
	weapons.append(WeaponData.new(
		"Shotgun", 12.0, 1.5, 8, 32, 3.0,
		8.0, 4.0, 2.0, 5.0, false, true, 0.0, 30.0
	))

func _process(delta: float) -> void:
	# Fire cooldown
	if fire_cooldown > 0.0:
		fire_cooldown -= delta
		if fire_cooldown <= 0.0:
			can_fire = true

	# Reload timer
	if is_reloading:
		reload_timer -= delta
		if reload_timer <= 0.0:
			_finish_reload()

	# Recoil recovery
	if accumulated_recoil.length() > 0.01 and current_weapon:
		var recovery := current_weapon.recoil_recovery * delta
		accumulated_recoil = accumulated_recoil.move_toward(Vector2.ZERO, recovery)

	# Input handling
	if not player or not player.is_alive:
		return

	# Weapon switching
	if Input.is_action_just_pressed("weapon_1") and weapons.size() > 0:
		equip_weapon(0)
	elif Input.is_action_just_pressed("weapon_2") and weapons.size() > 1:
		equip_weapon(1)
	elif Input.is_action_just_pressed("weapon_3") and weapons.size() > 2:
		equip_weapon(2)

	# Firing
	if current_weapon:
		if current_weapon.is_automatic:
			if Input.is_action_pressed("shoot"):
				try_fire()
		else:
			if Input.is_action_just_pressed("shoot"):
				try_fire()

	# Reload
	if Input.is_action_just_pressed("reload"):
		try_reload()

func equip_weapon(index: int) -> void:
	if index < 0 or index >= weapons.size():
		return
	if index == current_weapon_index and not is_reloading:
		return

	# Cancel reload if switching weapons
	is_reloading = false

	current_weapon_index = index
	current_weapon = weapons[index]
	can_fire = true
	fire_cooldown = 0.0
	accumulated_recoil = Vector2.ZERO

	weapon_changed.emit(current_weapon)
	ammo_changed.emit(current_weapon.current_ammo, current_weapon.reserve_ammo)

func try_fire() -> void:
	if not current_weapon or not can_fire or is_reloading:
		return

	if current_weapon.current_ammo <= 0:
		try_reload()
		return

	_fire()

func _fire() -> void:
	current_weapon.current_ammo -= 1
	can_fire = false
	fire_cooldown = 1.0 / current_weapon.fire_rate

	# Get fire direction from player
	var fire_origin := player.get_muzzle_position()
	var fire_dir := player.get_look_direction()

	# Apply spread
	if current_weapon.spread > 0.0:
		var spread_rad := deg_to_rad(current_weapon.spread)
		fire_dir.x += randf_range(-spread_rad, spread_rad)
		fire_dir.y += randf_range(-spread_rad, spread_rad)
		fire_dir = fire_dir.normalized()

	# Spawn muzzle flash particles
	if ohao_viewport:
		ohao_viewport.spawn_particles_directed(
			fire_origin, current_weapon.particle_type, fire_dir
		)

	# Hitscan or projectile
	if current_weapon.is_hitscan:
		_hitscan_fire(fire_origin, fire_dir)
	else:
		_projectile_fire(fire_origin, fire_dir)

	# Apply recoil
	_apply_recoil()

	weapon_fired.emit(current_weapon)
	ammo_changed.emit(current_weapon.current_ammo, current_weapon.reserve_ammo)

	# Auto-reload on empty
	if current_weapon.current_ammo <= 0 and current_weapon.reserve_ammo > 0:
		call_deferred("try_reload")

func _hitscan_fire(origin: Vector3, direction: Vector3) -> void:
	if not ohao_viewport:
		return

	# Use OHAO's picking system to detect hits
	# Convert fire direction to screen-space for pick_object_at
	# For now, pick at screen center (crosshair)
	var viewport_size := ohao_viewport.get_viewport_size()
	var center := Vector2(viewport_size.x / 2.0, viewport_size.y / 2.0)

	ohao_viewport.pick_object_at(center)
	var hit_name: String = ohao_viewport.get_selected_actor_name()

	if hit_name != "":
		# Spawn impact particles at hit location
		var impact_pos := origin + direction * 10.0  # Approximate
		ohao_viewport.spawn_particles(impact_pos, 1)  # IMPACT_SPARK

		# Notify game manager of hit (for enemy damage)
		_on_hit(hit_name, current_weapon.damage)

func _projectile_fire(origin: Vector3, direction: Vector3) -> void:
	# Spawn a projectile physics body
	# This would create an OhaoPhysicsBody in the scene tree
	# For now, simplified - treated as fast hitscan
	_hitscan_fire(origin, direction)

func _apply_recoil() -> void:
	if not current_weapon or not player:
		return

	var recoil_v := deg_to_rad(current_weapon.recoil_vertical)
	var recoil_h := deg_to_rad(randf_range(
		-current_weapon.recoil_horizontal,
		current_weapon.recoil_horizontal
	))

	accumulated_recoil += Vector2(recoil_h, recoil_v)

	# Apply to camera
	player.camera_pitch += recoil_v
	player.camera_yaw += recoil_h

func try_reload() -> void:
	if not current_weapon or is_reloading:
		return
	if current_weapon.current_ammo >= current_weapon.magazine_size:
		return
	if current_weapon.reserve_ammo <= 0:
		return

	is_reloading = true
	can_fire = false
	reload_timer = current_weapon.reload_time
	reload_started.emit(current_weapon.reload_time)

func _finish_reload() -> void:
	is_reloading = false
	can_fire = true

	var ammo_needed := current_weapon.magazine_size - current_weapon.current_ammo
	var ammo_available := mini(ammo_needed, current_weapon.reserve_ammo)
	current_weapon.current_ammo += ammo_available
	current_weapon.reserve_ammo -= ammo_available

	ammo_changed.emit(current_weapon.current_ammo, current_weapon.reserve_ammo)
	reload_finished.emit()

func _on_hit(actor_name: String, damage: float) -> void:
	# Propagate hit to game manager
	var game_manager = _find_game_manager()
	if game_manager:
		game_manager.on_enemy_hit(actor_name, damage)

func _find_game_manager() -> Node:
	return get_tree().get_first_node_in_group("game_manager")

func get_crosshair_info() -> Dictionary:
	return {
		"weapon_name": current_weapon.name if current_weapon else "",
		"ammo": current_weapon.current_ammo if current_weapon else 0,
		"reserve": current_weapon.reserve_ammo if current_weapon else 0,
		"reloading": is_reloading,
		"can_fire": can_fire and not is_reloading
	}
