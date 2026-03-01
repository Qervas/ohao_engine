extends Node
## Ohao - Autoload singleton for the OHAO Engine helper layer.
##
## Registered as "Ohao" autoload. Provides quick access to viewport,
## scene building, presets, and physics factories.
##
## Usage:
##   var vp = Ohao.viewport(self)
##   Ohao.scene().build(vp, {"template": "arena", "rendering": "horror"})
##   var body = Ohao.make_physics_body(OhaoConst.BODY_KINEMATIC, OhaoConst.SHAPE_CAPSULE, 80.0)

var _viewport_cache: WeakRef = weakref(null)


## Find the OhaoViewport in the scene tree.
## Searches upward from `from_node`, then falls back to tree-wide search.
## Result is cached for performance.
func viewport(from_node: Node = null) -> OhaoViewport:
	# Check cache first
	var cached = _viewport_cache.get_ref()
	if cached is OhaoViewport and is_instance_valid(cached):
		return cached

	# Search upward from node
	if from_node:
		var node := from_node.get_parent()
		while node:
			if node is OhaoViewport:
				_viewport_cache = weakref(node)
				return node
			node = node.get_parent()

	# Fallback: search entire tree
	var root := get_tree().root
	var result := _find_viewport_recursive(root)
	if result:
		_viewport_cache = weakref(result)
	return result


func _find_viewport_recursive(node: Node) -> OhaoViewport:
	if node is OhaoViewport:
		return node
	for child in node.get_children():
		var found := _find_viewport_recursive(child)
		if found:
			return found
	return null


## Enter game mode: C++ input disabled, GDScript drives camera.
## Also sets parent Controls to MOUSE_FILTER_IGNORE so mouse events
## reach _unhandled_input (Godot's GUI system eats them otherwise).
func enter_game_mode(vp: OhaoViewport = null) -> void:
	if not vp: vp = viewport()
	if not vp: return
	vp.set_input_mode(OhaoConst.INPUT_GAME)
	# Make parent Controls transparent to mouse events
	var node := vp.get_parent()
	while node:
		if node is Control:
			node.set_meta("_ohao_prev_mouse_filter", node.mouse_filter)
			node.mouse_filter = Control.MOUSE_FILTER_IGNORE
		node = node.get_parent()


## Exit game mode: restore C++ editor input and parent mouse filters.
func exit_game_mode(vp: OhaoViewport = null) -> void:
	if not vp: vp = viewport()
	if not vp: return
	vp.set_input_mode(OhaoConst.INPUT_EDITOR)
	# Restore parent Controls' mouse filters
	var node := vp.get_parent()
	while node:
		if node is Control and node.has_meta("_ohao_prev_mouse_filter"):
			node.mouse_filter = node.get_meta("_ohao_prev_mouse_filter")
			node.remove_meta("_ohao_prev_mouse_filter")
		node = node.get_parent()


## Get the scene builder (static class, returned for chaining convenience).
func scene() -> GDScript:
	return OhaoSceneBuilder


## Get the presets (static class).
func presets() -> GDScript:
	return OhaoPresets


## Get the settings/effect catalog (static class).
func settings() -> GDScript:
	return load("res://addons/ohao_helpers/ohao_settings.gd")


## Get the control template system (static class).
## OhaoControl.apply(vp, "fps") / OhaoControl.list() / OhaoControl.recommended_for([...])
func control() -> GDScript:
	return load("res://addons/ohao_helpers/ohao_control.gd")


## Get the weather/day-night system (static class).
## OhaoWeather.set_time(vp, 14.5)           # 2:30 PM
## OhaoWeather.preset(vp, "sunset")          # named preset
## OhaoWeather.create_clock(vp, 6.0, 0.05)  # auto-advancing clock Node
## OhaoWeather.weather_preset(vp, "golden_hour")
func weather() -> GDScript:
	return load("res://addons/ohao_helpers/ohao_weather.gd")


## Create an in-game settings panel and optionally add it to a parent node.
func create_settings_panel(parent: Node = null) -> CanvasLayer:
	var PanelClass = load("res://addons/ohao_helpers/ohao_settings_panel.gd")
	var panel = PanelClass.new()
	if parent:
		parent.add_child(panel)
	return panel


## Quick factory for physics bodies.
func make_physics_body(body_type: int = OhaoConst.BODY_DYNAMIC,
		shape_type: int = OhaoConst.SHAPE_BOX,
		mass: float = 1.0, friction: float = 0.5,
		restitution: float = 0.0) -> OhaoPhysicsBody:
	var body := OhaoPhysicsBody.new()
	body.set_body_type(body_type)
	body.set_shape_type(shape_type)
	body.set_mass(mass)
	body.set_friction(friction)
	body.set_restitution(restitution)
	return body


## Quick factory for visual mesh instances.
func make_mesh(mesh_type: int = OhaoConst.MESH_CUBE,
		color: Color = Color(0.7, 0.7, 0.8),
		scale: Vector3 = Vector3.ONE,
		metallic: float = 0.0, roughness: float = 0.5) -> OhaoMeshInstance:
	var mesh := OhaoMeshInstance.new()
	mesh.set_mesh_type(mesh_type)
	mesh.set_mesh_color(color)
	mesh.set_mesh_scale(scale)
	mesh.set_metallic(metallic)
	mesh.set_roughness(roughness)
	return mesh


## Get the AI texture generator (OhaoAI autoload).
func ai() -> Node:
	return get_node_or_null("/root/OhaoAI")


## Shortcut: spawn particles at position.
func particles(pos: Vector3, type: int = OhaoConst.PARTICLE_EXPLOSION) -> void:
	var vp := viewport()
	if vp:
		vp.spawn_particles(pos, type)


## Shortcut: spawn directed particles.
func particles_directed(pos: Vector3, type: int, dir: Vector3) -> void:
	var vp := viewport()
	if vp:
		vp.spawn_particles_directed(pos, type, dir)


# === Audio Shortcuts ===

## Play a 2D sound effect (non-positional).
func play_sfx(filename: String, volume: float = 1.0) -> int:
	var vp := viewport()
	if vp:
		return vp.play_sound("res://sounds/" + filename, OhaoConst.AUDIO_SFX, false, volume)
	return 0


## Play a 3D positional sound effect.
func play_sfx_at(filename: String, pos: Vector3, volume: float = 1.0) -> int:
	var vp := viewport()
	if vp:
		return vp.play_sound_at("res://sounds/" + filename, pos, OhaoConst.AUDIO_SFX, false, volume)
	return 0


## Play background music (looping, non-positional).
func play_music(filename: String, volume: float = 0.5) -> int:
	var vp := viewport()
	if vp:
		return vp.play_sound("res://sounds/" + filename, OhaoConst.AUDIO_MUSIC, true, volume)
	return 0


## Play a 3D positional ambient loop.
func play_ambient_at(filename: String, pos: Vector3, volume: float = 0.7) -> int:
	var vp := viewport()
	if vp:
		return vp.play_sound_at("res://sounds/" + filename, pos, OhaoConst.AUDIO_AMBIENT, true, volume)
	return 0


## Stop a sound by handle.
func stop_sound(handle: int) -> void:
	var vp := viewport()
	if vp:
		vp.stop_sound(handle)
