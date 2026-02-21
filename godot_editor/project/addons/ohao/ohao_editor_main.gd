@tool
extends Control
## OHAO Editor Main Screen - modular orchestrator with auto-sync
## Viewport is for render preview only. Editing happens in Godot's 3D editor.

const ViewportToolbar = preload("res://addons/ohao/ui/viewport_toolbar.gd")
const PostProcessPanel = preload("res://addons/ohao/ui/post_process_panel.gd")
const StatusBarScript = preload("res://addons/ohao/ui/status_bar.gd")

var ohao_viewport: OhaoViewport
var toolbar: HBoxContainer
var post_process_panel: PanelContainer
var status_bar: HBoxContainer
var context_menu: PopupMenu

var _is_ready := false

# Auto-sync state
var _sync_timer := 0.0
var _last_scene_hash := 0
var _fps_timer := 0.0
var _fps_count := 0
var _current_fps := 0

# Simulation state
var _is_playing := false

# Example scenes
var _example_scenes: Array[String] = ["basic_scene.tscn", "multi_light_scene.tscn"]

func _ready() -> void:
	if not Engine.is_editor_hint():
		return

	ohao_viewport = $VBoxContainer/ViewportArea/OhaoViewport

	_build_overlay_ui()
	_connect_signals()

	# Connect viewport signals if available
	if ohao_viewport.has_signal("actor_selected"):
		ohao_viewport.actor_selected.connect(_on_actor_selected)
	if ohao_viewport.has_signal("right_click_menu"):
		ohao_viewport.right_click_menu.connect(_on_viewport_right_click)

	_is_ready = true
	print("[OHAO] Editor main screen ready")

func _build_overlay_ui() -> void:
	var viewport_area = $VBoxContainer/ViewportArea

	# Toolbar (overlay at top of viewport)
	toolbar = HBoxContainer.new()
	toolbar.set_script(ViewportToolbar)
	toolbar.mouse_filter = Control.MOUSE_FILTER_STOP
	# Anchor to top, full width, fixed height
	toolbar.anchor_left = 0.0
	toolbar.anchor_right = 1.0
	toolbar.anchor_top = 0.0
	toolbar.anchor_bottom = 0.0
	toolbar.offset_bottom = 36
	viewport_area.add_child(toolbar)
	toolbar.setup(ohao_viewport)

	# Post-process panel (overlay at right, hidden by default)
	post_process_panel = PanelContainer.new()
	post_process_panel.set_script(PostProcessPanel)
	post_process_panel.mouse_filter = Control.MOUSE_FILTER_STOP
	post_process_panel.anchor_left = 1.0
	post_process_panel.anchor_right = 1.0
	post_process_panel.anchor_top = 0.05
	post_process_panel.anchor_bottom = 0.95
	post_process_panel.offset_left = -290
	post_process_panel.offset_right = -10
	post_process_panel.visible = false
	viewport_area.add_child(post_process_panel)
	post_process_panel.setup(ohao_viewport)

	# Status bar (below viewport, in the VBoxContainer)
	status_bar = HBoxContainer.new()
	status_bar.set_script(StatusBarScript)
	$VBoxContainer.add_child(status_bar)

	# Context menu
	context_menu = PopupMenu.new()
	context_menu.add_item("Reset Camera", 0)
	context_menu.add_item("Focus All", 1)
	context_menu.add_separator()
	context_menu.add_item("Toggle Post FX Panel", 3)
	context_menu.add_separator()
	# Examples submenu
	var examples_menu = PopupMenu.new()
	examples_menu.name = "ExamplesMenu"
	for i in _example_scenes.size():
		var display_name = String(_example_scenes[i]).replace(".tscn", "").replace("_", " ").capitalize()
		examples_menu.add_item(display_name, i)
	examples_menu.id_pressed.connect(_on_example_selected)
	context_menu.add_child(examples_menu)
	context_menu.add_submenu_item("Load Example", "ExamplesMenu", 5)
	context_menu.id_pressed.connect(_on_context_menu)
	add_child(context_menu)

func _connect_signals() -> void:
	toolbar.render_mode_changed.connect(func(idx: int) -> void:
		if ohao_viewport: ohao_viewport.set_render_mode(idx)
	)
	toolbar.camera_mode_changed.connect(func(idx: int) -> void:
		if ohao_viewport and ohao_viewport.has_method("set_camera_mode"):
			ohao_viewport.set_camera_mode(idx)
	)
	toolbar.play_pressed.connect(func() -> void:
		_is_playing = true
		toolbar.set_play_state(true)
		if ohao_viewport and ohao_viewport.has_method("play_physics"):
			ohao_viewport.play_physics()
	)
	toolbar.pause_pressed.connect(func() -> void:
		_is_playing = false
		toolbar.set_play_state(false)
		if ohao_viewport and ohao_viewport.has_method("pause_physics"):
			ohao_viewport.pause_physics()
	)
	toolbar.step_pressed.connect(func() -> void:
		if ohao_viewport and ohao_viewport.has_method("step_physics"):
			ohao_viewport.step_physics()
	)
	toolbar.speed_changed.connect(func(val: float) -> void:
		if ohao_viewport and ohao_viewport.has_method("set_physics_speed"):
			ohao_viewport.set_physics_speed(val)
	)
	toolbar.wireframe_toggled.connect(func(enabled: bool) -> void:
		if ohao_viewport and ohao_viewport.has_method("set_wireframe_enabled"):
			ohao_viewport.set_wireframe_enabled(enabled)
	)
	toolbar.grid_toggled.connect(func(enabled: bool) -> void:
		if ohao_viewport and ohao_viewport.has_method("set_grid_enabled"):
			ohao_viewport.set_grid_enabled(enabled)
	)
	toolbar.import_model_requested.connect(_on_import_model_requested)
	toolbar.post_process_toggled.connect(_toggle_post_process)

func _process(delta: float) -> void:
	if not Engine.is_editor_hint() or not ohao_viewport or not _is_ready:
		return

	# FPS counter
	_fps_timer += delta
	_fps_count += 1
	if _fps_timer >= 1.0:
		_current_fps = _fps_count
		_fps_count = 0
		_fps_timer = 0.0
		_update_stats()

	# Auto-sync polling (every 0.5s)
	_sync_timer += delta
	if _sync_timer >= 0.5:
		_sync_timer = 0.0
		_check_scene_changes()

func _update_stats() -> void:
	if not ohao_viewport or not toolbar or not status_bar:
		return
	var stats = ohao_viewport.get_render_stats()
	var w: int = stats.get("width", 0)
	var h: int = stats.get("height", 0)
	var objs: int = stats.get("synced_objects", 0)
	toolbar.update_stats(_current_fps, w, h, objs)
	status_bar.set_resolution(w, h)
	status_bar.set_object_count(objs)

func _check_scene_changes() -> void:
	if not status_bar:
		return
	var edited_scene = EditorInterface.get_edited_scene_root()
	if not edited_scene:
		status_bar.set_sync_state(0)
		return

	var new_hash = _compute_scene_hash(edited_scene)
	if new_hash != _last_scene_hash:
		_last_scene_hash = new_hash
		_sync_from_editor()

func _compute_scene_hash(node: Node) -> int:
	var h = hash(node.name)
	h ^= hash(node.get_child_count())
	var node3d = node as Node3D
	if node3d:
		var t = node3d.global_transform
		h ^= hash(snapped(t.origin.x, 0.001))
		h ^= hash(snapped(t.origin.y, 0.001))
		h ^= hash(snapped(t.origin.z, 0.001))
	var mesh_inst = node as MeshInstance3D
	if mesh_inst:
		var m = mesh_inst.mesh
		h ^= hash(m.resource_path if m else "")
	var light = node as Light3D
	if light:
		h ^= hash(light.light_color)
		h ^= hash(snapped(light.light_energy, 0.01))
	for i in node.get_child_count():
		h ^= _compute_scene_hash(node.get_child(i))
	return h

func _sync_from_editor() -> void:
	var edited_scene = EditorInterface.get_edited_scene_root()
	if not edited_scene or not ohao_viewport or not status_bar:
		return

	status_bar.set_sync_state(1)
	ohao_viewport.sync_from_godot(edited_scene)
	var count = ohao_viewport.get_synced_object_count()
	status_bar.set_sync_state(2, count)
	print("[OHAO] Auto-synced %d objects" % count)

func trigger_full_sync() -> void:
	if not _is_ready:
		return
	_last_scene_hash = 0
	_sync_from_editor()

func on_scene_changed() -> void:
	if not _is_ready:
		return
	_last_scene_hash = 0
	_sync_from_editor()

func on_scene_closed() -> void:
	if not _is_ready:
		return
	if ohao_viewport:
		ohao_viewport.clear_scene()
	if status_bar:
		status_bar.set_sync_state(0)
	_last_scene_hash = 0

# Context menu
func _on_viewport_right_click(pos: Vector2) -> void:
	context_menu.position = Vector2i(pos)
	context_menu.popup()

func _on_context_menu(id: int) -> void:
	match id:
		0:  # Reset Camera
			if ohao_viewport and ohao_viewport.has_method("set_camera_mode"):
				ohao_viewport.set_camera_mode(0)
		1:  # Focus All
			if ohao_viewport and ohao_viewport.has_method("focus_on_scene"):
				ohao_viewport.focus_on_scene()
		3:  # Toggle Post FX
			_toggle_post_process()

func _on_example_selected(index: int) -> void:
	var scene_name: String = _example_scenes[index]
	var path: String = "res://examples/" + scene_name
	if not ResourceLoader.exists(path):
		print("[OHAO] Example not found: ", path)
		return
	var packed = ResourceLoader.load(path) as PackedScene
	if not packed:
		return
	var root = packed.instantiate()
	add_child(root)
	ohao_viewport.sync_from_godot(root)
	var count = ohao_viewport.get_synced_object_count()
	print("[OHAO] Loaded example '%s' with %d objects" % [scene_name, count])
	remove_child(root)
	root.queue_free()
	if status_bar:
		status_bar.set_sync_state(2, count)

func _on_import_model_requested() -> void:
	var dialog := FileDialog.new()
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.filters = PackedStringArray(["*.obj ; OBJ Models", "*.gltf ; GLTF Models", "*.glb ; GLB Models"])
	dialog.title = "Import 3D Model"
	dialog.file_selected.connect(func(path: String) -> void:
		if ohao_viewport and ohao_viewport.has_method("import_model"):
			ohao_viewport.import_model(path)
		dialog.queue_free()
	)
	dialog.canceled.connect(func() -> void:
		dialog.queue_free()
	)
	add_child(dialog)
	dialog.popup_centered(Vector2i(800, 600))

func _toggle_post_process() -> void:
	post_process_panel.visible = not post_process_panel.visible

func _on_actor_selected(actor_name: String) -> void:
	var edited_scene = EditorInterface.get_edited_scene_root()
	if not edited_scene:
		return
	var node = _find_node_by_name(edited_scene, actor_name)
	if node:
		var selection = EditorInterface.get_selection()
		selection.clear()
		selection.add_node(node)

func _find_node_by_name(root: Node, target_name: String) -> Node:
	if root.name == target_name:
		return root
	for i in root.get_child_count():
		var found = _find_node_by_name(root.get_child(i), target_name)
		if found:
			return found
	return null
