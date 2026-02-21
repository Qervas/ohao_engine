class_name FPSHud
extends CanvasLayer
## HUD overlay for OHAO Engine FPS
## Displays crosshair, health, ammo, score, and wave information.

# Crosshair settings
@export var crosshair_size: float = 12.0
@export var crosshair_gap: float = 4.0
@export var crosshair_thickness: float = 2.0
@export var crosshair_color: Color = Color.WHITE
@export var hit_marker_duration: float = 0.2

# References
var player: PlayerController
var weapon_system: WeaponSystem
var game_manager: GameManager

# UI elements
var health_label: Label
var ammo_label: Label
var weapon_label: Label
var score_label: Label
var wave_label: Label
var reload_bar: ProgressBar
var crosshair_panel: Control
var hit_marker_timer: float = 0.0
var damage_overlay: ColorRect

func _ready() -> void:
	_create_ui()
	call_deferred("_find_references")

func _find_references() -> void:
	# Find player
	var players := get_tree().get_nodes_in_group("player")
	if players.size() > 0:
		player = players[0]
		player.health_changed.connect(_on_health_changed)
		player.player_died.connect(_on_player_died)

		# Find weapon system as child of player
		for child in player.get_children():
			if child is WeaponSystem:
				weapon_system = child
				weapon_system.ammo_changed.connect(_on_ammo_changed)
				weapon_system.weapon_changed.connect(_on_weapon_changed)
				weapon_system.weapon_fired.connect(_on_weapon_fired)
				weapon_system.reload_started.connect(_on_reload_started)
				weapon_system.reload_finished.connect(_on_reload_finished)
				break

	# Find game manager
	var gm = get_tree().get_first_node_in_group("game_manager")
	if gm is GameManager:
		game_manager = gm
		game_manager.score_changed.connect(_on_score_changed)
		game_manager.wave_started.connect(_on_wave_started)
		game_manager.game_over.connect(_on_game_over)

func _create_ui() -> void:
	# Health display (bottom-left)
	health_label = Label.new()
	health_label.text = "HP: 100"
	health_label.add_theme_font_size_override("font_size", 24)
	health_label.add_theme_color_override("font_color", Color(0.2, 1.0, 0.3))
	health_label.position = Vector2(20, -50)
	health_label.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	add_child(health_label)

	# Ammo display (bottom-right)
	ammo_label = Label.new()
	ammo_label.text = "12 / 60"
	ammo_label.add_theme_font_size_override("font_size", 28)
	ammo_label.add_theme_color_override("font_color", Color.WHITE)
	ammo_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	ammo_label.position = Vector2(-180, -50)
	ammo_label.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	add_child(ammo_label)

	# Weapon name (bottom-right, above ammo)
	weapon_label = Label.new()
	weapon_label.text = "Pistol"
	weapon_label.add_theme_font_size_override("font_size", 16)
	weapon_label.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	weapon_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	weapon_label.position = Vector2(-180, -75)
	weapon_label.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	add_child(weapon_label)

	# Score (top-right)
	score_label = Label.new()
	score_label.text = "Score: 0"
	score_label.add_theme_font_size_override("font_size", 20)
	score_label.add_theme_color_override("font_color", Color(1.0, 0.9, 0.3))
	score_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	score_label.position = Vector2(-180, 20)
	score_label.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	add_child(score_label)

	# Wave indicator (top-center)
	wave_label = Label.new()
	wave_label.text = ""
	wave_label.add_theme_font_size_override("font_size", 32)
	wave_label.add_theme_color_override("font_color", Color(1.0, 0.4, 0.2))
	wave_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	wave_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	wave_label.position = Vector2(-100, 30)
	wave_label.custom_minimum_size = Vector2(200, 40)
	add_child(wave_label)

	# Reload progress bar (center, below crosshair)
	reload_bar = ProgressBar.new()
	reload_bar.visible = false
	reload_bar.custom_minimum_size = Vector2(120, 8)
	reload_bar.set_anchors_preset(Control.PRESET_CENTER)
	reload_bar.position = Vector2(-60, 30)
	add_child(reload_bar)

	# Damage vignette overlay
	damage_overlay = ColorRect.new()
	damage_overlay.color = Color(0.8, 0, 0, 0)
	damage_overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	damage_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(damage_overlay)

	# Crosshair drawn via _draw on a centered control
	crosshair_panel = Control.new()
	crosshair_panel.set_anchors_preset(Control.PRESET_FULL_RECT)
	crosshair_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	crosshair_panel.draw.connect(_draw_crosshair.bind(crosshair_panel))
	add_child(crosshair_panel)

func _process(delta: float) -> void:
	# Hit marker fade
	if hit_marker_timer > 0.0:
		hit_marker_timer -= delta
		crosshair_panel.queue_redraw()

	# Damage overlay fade
	if damage_overlay.color.a > 0.0:
		damage_overlay.color.a = maxf(0.0, damage_overlay.color.a - delta * 2.0)

	# Reload bar
	if reload_bar.visible and weapon_system and weapon_system.is_reloading:
		var weapon := weapon_system.current_weapon
		if weapon:
			var progress := 1.0 - (weapon_system.reload_timer / weapon.reload_time)
			reload_bar.value = progress * 100.0

func _draw_crosshair(panel: Control) -> void:
	var center := panel.size / 2.0
	var s := crosshair_size
	var g := crosshair_gap
	var t := crosshair_thickness

	var color := crosshair_color

	# Hit marker: flash white/red
	if hit_marker_timer > 0.0:
		color = Color(1.0, 0.3, 0.3)
		s *= 1.3

	# Draw 4 crosshair lines
	# Top
	panel.draw_rect(Rect2(center.x - t/2, center.y - s - g, t, s), color)
	# Bottom
	panel.draw_rect(Rect2(center.x - t/2, center.y + g, t, s), color)
	# Left
	panel.draw_rect(Rect2(center.x - s - g, center.y - t/2, s, t), color)
	# Right
	panel.draw_rect(Rect2(center.x + g, center.y - t/2, s, t), color)

	# Center dot
	panel.draw_circle(center, 1.5, color)

func _on_health_changed(current: float, maximum: float) -> void:
	health_label.text = "HP: %d" % int(current)

	# Color based on health percentage
	var ratio := current / maximum
	if ratio > 0.6:
		health_label.add_theme_color_override("font_color", Color(0.2, 1.0, 0.3))
	elif ratio > 0.3:
		health_label.add_theme_color_override("font_color", Color(1.0, 0.8, 0.2))
	else:
		health_label.add_theme_color_override("font_color", Color(1.0, 0.2, 0.2))

	# Flash damage overlay
	if ratio < 1.0:
		damage_overlay.color.a = clampf(0.3 * (1.0 - ratio), 0.0, 0.5)

func _on_ammo_changed(current: int, reserve: int) -> void:
	ammo_label.text = "%d / %d" % [current, reserve]

	if current <= 0:
		ammo_label.add_theme_color_override("font_color", Color(1.0, 0.3, 0.3))
	else:
		ammo_label.add_theme_color_override("font_color", Color.WHITE)

func _on_weapon_changed(weapon_data: WeaponSystem.WeaponData) -> void:
	weapon_label.text = weapon_data.name
	ammo_label.text = "%d / %d" % [weapon_data.current_ammo, weapon_data.reserve_ammo]

func _on_weapon_fired(_weapon_data: WeaponSystem.WeaponData) -> void:
	hit_marker_timer = hit_marker_duration
	crosshair_panel.queue_redraw()

func _on_reload_started(duration: float) -> void:
	reload_bar.visible = true
	reload_bar.value = 0

func _on_reload_finished() -> void:
	reload_bar.visible = false

func _on_score_changed(new_score: int) -> void:
	score_label.text = "Score: %d" % new_score

func _on_wave_started(wave_num: int) -> void:
	wave_label.text = "WAVE %d" % wave_num
	# Fade out wave text after 3 seconds
	get_tree().create_timer(3.0).timeout.connect(func(): wave_label.text = "")

func _on_game_over(victory: bool) -> void:
	var game_over_label := Label.new()
	game_over_label.add_theme_font_size_override("font_size", 64)
	game_over_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	game_over_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	game_over_label.set_anchors_preset(Control.PRESET_CENTER)
	game_over_label.custom_minimum_size = Vector2(600, 100)
	game_over_label.position = Vector2(-300, -50)

	if victory:
		game_over_label.text = "VICTORY"
		game_over_label.add_theme_color_override("font_color", Color(1.0, 0.9, 0.3))
	else:
		game_over_label.text = "GAME OVER"
		game_over_label.add_theme_color_override("font_color", Color(1.0, 0.2, 0.2))

	add_child(game_over_label)

func _on_player_died() -> void:
	# Red screen flash
	damage_overlay.color = Color(0.6, 0, 0, 0.6)
