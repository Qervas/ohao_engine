extends Node
## OhaoServer - Localhost HTTP server for AI agent access to the live engine.
##
## Starts automatically on _ready(). Listens on 127.0.0.1:9756.
## Agents use WebFetch or curl to read/write scene state, effects,
## physics, and camera without loading all documentation.
##
## Quick test:  curl http://localhost:9756/
## Agent usage: WebFetch("http://localhost:9756/scene", "list actors")

const PORT := 9756
const HOST := "127.0.0.1"

# Tracked scene state (actors added via this server)
var _actors: Dictionary = {}   # name → {type, pos, rot, scale, color}
var _physics_state: String = "stopped"

var _server := TCPServer.new()
# Each entry: {peer: StreamPeerTCP, buf: String}
var _clients: Array = []

var _settings_script = null   # loaded lazily

# Event system
var _event_buffer: Array = []          # Ring buffer of recent events
var _event_buffer_max: int = 500       # Max events to keep
var _subscriptions: Dictionary = {}     # sub_id -> {event_types, cursor}
var _next_sub_id: int = 1
var _events_connected: bool = false


func _ready() -> void:
	_settings_script = load("res://addons/ohao_helpers/ohao_settings.gd")
	var err := _server.listen(PORT, HOST)
	if err == OK:
		print("[OhaoServer] Listening on http://%s:%d/" % [HOST, PORT])
	else:
		push_warning("[OhaoServer] Failed to bind port %d (err %d) — already running?" % [PORT, err])


func _process(_delta: float) -> void:
	# Accept new connections
	while _server.is_connection_available():
		var peer: StreamPeerTCP = _server.take_connection()
		_clients.append({"peer": peer, "buf": ""})

	# Service existing connections
	for entry in _clients.duplicate():
		var peer: StreamPeerTCP = entry["peer"]
		if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
			_clients.erase(entry)
			continue
		var available := peer.get_available_bytes()
		if available > 0:
			var chunk := peer.get_utf8_string(available)
			entry["buf"] += chunk
		if _is_complete(entry["buf"]):
			var response := _dispatch(entry["buf"])
			peer.put_data(response)
			peer.disconnect_from_host()
			_clients.erase(entry)


# ─────────────────────────────────────────────────────────────────────────────
# HTTP parsing helpers
# ─────────────────────────────────────────────────────────────────────────────

func _is_complete(raw: String) -> bool:
	var sep := raw.find("\r\n\r\n")
	if sep < 0:
		return false
	# For requests with a body, ensure Content-Length bytes arrived
	var content_length := _get_content_length(raw)
	if content_length > 0:
		return raw.length() >= sep + 4 + content_length
	return true


func _get_content_length(raw: String) -> int:
	var lower := raw.to_lower()
	var idx := lower.find("content-length:")
	if idx < 0:
		return 0
	var line_end := lower.find("\r\n", idx)
	var value := raw.substr(idx + 15, line_end - idx - 15).strip_edges()
	return value.to_int()


func _parse_request(raw: String) -> Dictionary:
	var header_end := raw.find("\r\n\r\n")
	var header_block := raw.substr(0, header_end) if header_end >= 0 else raw
	var body := raw.substr(header_end + 4) if header_end >= 0 else ""

	var lines := header_block.split("\r\n")
	var first := lines[0].split(" ")
	var method := first[0] if first.size() > 0 else "GET"
	var full_path := first[1] if first.size() > 1 else "/"

	# Split path and query string
	var path := full_path
	var query_dict: Dictionary = {}
	var q := full_path.find("?")
	if q >= 0:
		path = full_path.substr(0, q)
		var qs := full_path.substr(q + 1)
		for pair in qs.split("&"):
			var kv := pair.split("=")
			if kv.size() == 2:
				query_dict[kv[0]] = kv[1].uri_decode()

	var body_json: Variant = null
	if body.length() > 0:
		var parser := JSON.new()
		if parser.parse(body) == OK:
			body_json = parser.get_data()

	return {
		"method": method,
		"path": path,
		"query": query_dict,
		"body": body_json,
	}


func _ok(data: Variant) -> PackedByteArray:
	return _respond(200, "OK", data)


func _err(msg: String, code: int = 400) -> PackedByteArray:
	var status_text := "Bad Request" if code == 400 else "Not Found"
	return _respond(code, status_text, {"error": msg})


func _respond(code: int, status: String, data: Variant) -> PackedByteArray:
	var body := JSON.stringify(data, "  ")
	var header := (
		"HTTP/1.1 %d %s\r\n" % [code, status] +
		"Content-Type: application/json\r\n" +
		"Content-Length: %d\r\n" % body.length() +
		"Access-Control-Allow-Origin: *\r\n" +
		"Connection: close\r\n" +
		"\r\n"
	)
	return (header + body).to_utf8_buffer()


# ─────────────────────────────────────────────────────────────────────────────
# Router
# ─────────────────────────────────────────────────────────────────────────────

func _dispatch(raw: String) -> PackedByteArray:
	var req := _parse_request(raw)
	var method: String = req["method"]
	var path: String = req["path"]
	var query: Dictionary = req["query"]
	var body: Variant = req["body"]

	match [method, path]:
		["GET",  "/"]:             return _handle_root()
		["GET",  "/scene"]:        return _handle_get_scene()
		["POST", "/scene/build"]:  return _handle_scene_build(body)
		["POST", "/scene/actor"]:  return _handle_add_actor(body)
		["POST", "/scene/clear"]:  return _handle_clear_scene()
		["GET",  "/camera"]:       return _handle_get_camera()
		["POST", "/camera"]:       return _handle_set_camera(body)
		["GET",  "/effects"]:      return _handle_get_effects()
		["POST", "/effects"]:      return _handle_set_effects(body)
		["POST", "/effects/preset"]: return _handle_effects_preset(body)
		["GET",  "/physics"]:      return _handle_get_physics()
		["POST", "/physics/play"]: return _handle_physics_play()
		["POST", "/physics/pause"]:return _handle_physics_pause()
		["POST", "/physics/step"]: return _handle_physics_step(body)
		["POST", "/physics/stop"]: return _handle_physics_stop()
		["POST", "/physics/raycast"]: return _handle_raycast(body)

		# God-mode perception endpoints (MCP)
		["GET",  "/god/state"]:     return _handle_god_state()
		["POST", "/god/screenshot"]:return _handle_god_screenshot(body)
		["POST", "/god/capture"]:   return _handle_god_capture()
		["POST", "/god/snapshot"]:  return _handle_god_snapshot(body)
		["POST", "/god/look_at"]:   return _handle_god_look_at(body)
		["POST", "/god/orbit_capture"]: return _handle_god_orbit_capture(body)
		# Introspection (AI self-development)
		["GET",  "/introspect/pipeline"]: return _handle_introspect_pipeline()
		["GET",  "/introspect/perf"]:     return _handle_introspect_perf()
		# Hot-reload (AI shader experiments)
		["POST", "/hot_reload/shader"]:   return _handle_hot_reload_shader(body)
		["POST", "/hot_reload/script"]:   return _handle_hot_reload_script(body)
		# Event subscription system (AI real-time observation)
		["POST", "/events/subscribe"]:    return _handle_events_subscribe(body)
		["GET",  "/events/poll"]:         return _handle_events_poll(query)
		["POST", "/events/unsubscribe"]:  return _handle_events_unsubscribe(body)
		["GET",  "/events/history"]:      return _handle_events_history(query)
		# AI Player — autonomous gameplay (no human in the loop)
		["POST", "/god/play"]:            return _handle_god_play(body)
		["GET",  "/god/game_state"]:      return _handle_god_game_state()
		["POST", "/god/step_and_observe"]:return _handle_god_step_and_observe(body)

	# DELETE /scene/actor?name=Box
	if method == "DELETE" and path == "/scene/actor":
		return _handle_remove_actor(query.get("name", ""))

	return _err("Unknown endpoint: %s %s" % [method, path], 404)


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Discovery
# ─────────────────────────────────────────────────────────────────────────────

func _handle_root() -> PackedByteArray:
	return _ok({
		"engine": "OHAO Engine",
		"port": PORT,
		"endpoints": [
			{"method": "GET",    "path": "/",                  "desc": "This manifest"},
			{"method": "GET",    "path": "/scene",             "desc": "List all actors and lights"},
			{"method": "POST",   "path": "/scene/build",       "desc": "Build scene from dict (template, objects, lights, rendering)"},
			{"method": "POST",   "path": "/scene/actor",       "desc": "Add actor: {type, name, pos, rot, scale, color, material}"},
			{"method": "DELETE", "path": "/scene/actor?name=X","desc": "Remove actor by name"},
			{"method": "POST",   "path": "/scene/clear",       "desc": "Clear all actors"},
			{"method": "GET",    "path": "/camera",            "desc": "Camera position, rotation, mode, forward"},
			{"method": "POST",   "path": "/camera",            "desc": "Set camera: {position, rotation_deg, mode}"},
			{"method": "GET",    "path": "/effects",           "desc": "All post-processing effect states"},
			{"method": "POST",   "path": "/effects",           "desc": "Set effects: {bloom: {enabled, threshold, intensity}, ssao: {...}, ...}"},
			{"method": "POST",   "path": "/effects/preset",    "desc": "Apply named preset: {rendering: 'horror'}"},
			{"method": "GET",    "path": "/physics",           "desc": "Physics simulation state"},
			{"method": "POST",   "path": "/physics/play",      "desc": "Start simulation"},
			{"method": "POST",   "path": "/physics/pause",     "desc": "Pause simulation"},
			{"method": "POST",   "path": "/physics/step",      "desc": "Step N frames: {steps: 10}"},
			{"method": "POST",   "path": "/physics/stop",      "desc": "Stop and reset simulation"},
			{"method": "POST",   "path": "/physics/raycast",   "desc": "Cast ray: {origin, direction, max_dist, layer_mask}"},
			{"method": "GET",    "path": "/god/state",          "desc": "Full scene state: actors, camera, effects, physics, environment"},
			{"method": "POST",   "path": "/god/screenshot",     "desc": "Save screenshot to disk: {path: 'C:/tmp/shot.png'}"},
			{"method": "POST",   "path": "/god/capture",        "desc": "Capture viewport as base64 PNG inline"},
			{"method": "POST",   "path": "/god/snapshot",       "desc": "Save state.json + screenshot.png: {output_dir: '...'}"},
			{"method": "POST",   "path": "/god/look_at",        "desc": "Position camera: {target, distance, elevation, azimuth}"},
			{"method": "POST",   "path": "/god/orbit_capture",  "desc": "Multi-angle capture: {target, distance, count, output_dir}"},
			{"method": "GET",    "path": "/introspect/pipeline","desc": "Render pipeline state: pass names, order, enabled flags"},
			{"method": "GET",    "path": "/introspect/perf",    "desc": "Performance stats: fps, delta_time, active passes, effects"},
			{"method": "POST",   "path": "/hot_reload/shader",  "desc": "Hot-reload shader: {pass_name, spv_path}"},
			{"method": "POST",   "path": "/hot_reload/script",  "desc": "Reload GDScript: {path}"},
			{"method": "POST",   "path": "/events/subscribe",   "desc": "Subscribe to events: {event_types: ['collision','lightning',...]}"},
			{"method": "GET",    "path": "/events/poll",         "desc": "Poll events: ?sub=<id> (or omit for recent)"},
			{"method": "POST",   "path": "/events/unsubscribe", "desc": "Unsubscribe: {subscription_id}"},
			{"method": "GET",    "path": "/events/history",      "desc": "Event history: ?count=50&type=collision"},
			{"method": "POST",   "path": "/god/play",           "desc": "Inject game input: {actions: ['move_forward','jump'], mouse_delta?: [dx,dy]}"},
			{"method": "GET",    "path": "/god/game_state",     "desc": "Game state from game_manager: health, score, ammo, enemies, custom data"},
			{"method": "POST",   "path": "/god/step_and_observe","desc": "Atomic play loop: inject actions, step N frames, return state+screenshot"},
		]
	})


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Scene
# ─────────────────────────────────────────────────────────────────────────────

func _handle_get_scene() -> PackedByteArray:
	return _ok({
		"actors": _actors.values(),
		"actor_count": _actors.size(),
	})


func _handle_scene_build(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be a JSON object (scene dict)")
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found in scene tree")
	# Convert JSON arrays to Vector3/Color for OhaoSceneBuilder
	var converted: Dictionary = body.duplicate(true)
	if converted.has("objects"):
		for i in converted["objects"].size():
			var obj: Dictionary = converted["objects"][i]
			if obj.has("pos"):   obj["pos"]   = _to_v3(obj["pos"])
			if obj.has("rot"):   obj["rot"]   = _to_v3(obj["rot"])
			if obj.has("scale"): obj["scale"] = _to_v3(obj["scale"])
			if obj.has("color"): obj["color"] = _to_color(obj["color"])
			if obj.has("dir"):   obj["dir"]   = _to_v3(obj["dir"])
	if converted.has("lights"):
		for i in converted["lights"].size():
			var light: Dictionary = converted["lights"][i]
			if light.has("pos"):   light["pos"]   = _to_v3(light["pos"])
			if light.has("dir"):   light["dir"]   = _to_v3(light["dir"])
			if light.has("color"): light["color"] = _to_color(light["color"])
	# Track objects
	if body.has("objects"):
		for obj in body["objects"]:
			if obj.has("name"):
				_actors[obj["name"]] = _actor_entry_from_dict(obj)
	OhaoSceneBuilder.build(vp, converted)
	return _ok({"built": true, "actor_count": _actors.size()})


func _handle_add_actor(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {type, name, pos, rot, scale, color}")
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found in scene tree")
	var type: String  = body.get("type", "cube")
	var name: String  = body.get("name", "Actor_%d" % _actors.size())
	var pos: Vector3  = _to_v3(body.get("pos",   [0,0,0]))
	var rot: Vector3  = _to_v3(body.get("rot",   [0,0,0]))
	var scale: Vector3= _to_v3(body.get("scale", [1,1,1]))
	var color: Color  = _to_color(body.get("color", [0.7,0.7,0.8,1.0]))

	match type:
		"cube":     vp.add_cube(name, pos, rot, scale, color)
		"sphere":   vp.add_sphere(name, pos, rot, scale, color)
		"plane":    vp.add_plane(name, pos, rot, scale, color)
		"cylinder": vp.add_cylinder(name, pos, rot, scale, color)
		_:          return _err("Unknown actor type: %s (cube|sphere|plane|cylinder)" % type)

	vp.finish_sync()

	if body.has("material"):
		vp.set_actor_material_preset(name, body["material"])

	_actors[name] = _actor_entry_from_dict(body)
	return _ok({"added": name, "type": type, "pos": _v3_to_a(pos)})


func _handle_remove_actor(name: String) -> PackedByteArray:
	if name.is_empty():
		return _err("Query param 'name' required: DELETE /scene/actor?name=Box")
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found in scene tree")
	vp.remove_actor(name)
	_actors.erase(name)
	return _ok({"removed": name})


func _handle_clear_scene() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found in scene tree")
	vp.clear_scene()
	_actors.clear()
	return _ok({"cleared": true})


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Camera
# ─────────────────────────────────────────────────────────────────────────────

func _handle_get_camera() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	var pos := vp.get_camera_position()
	var fwd := vp.get_camera_forward()
	return _ok({
		"position":   _v3_to_a(pos),
		"forward":    _v3_to_a(fwd),
		"mode":       vp.get_input_mode(),
	})


func _handle_set_camera(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {position?, rotation_deg?, mode?}")
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	if body.has("position"):
		vp.set_camera_position(_to_v3(body["position"]))
	if body.has("rotation_deg"):
		var r := _to_v3(body["rotation_deg"])
		vp.set_camera_rotation_deg(r.x, r.y)
	if body.has("mode"):
		vp.set_camera_mode(int(body["mode"]))
	if body.has("focus"):
		vp.focus_on_scene()
	return _ok({"set": true})


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Effects
# ─────────────────────────────────────────────────────────────────────────────

const _ALL_EFFECTS := ["bloom", "ssao", "ssgi", "ssr", "taa",
					   "volumetrics", "motion_blur", "dof", "tonemapping"]

func _handle_get_effects() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	var result: Dictionary = {}
	for effect in _ALL_EFFECTS:
		result[effect] = _settings_script.get_effect_status(vp, effect)
	return _ok(result)


func _handle_set_effects(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {bloom: {enabled, threshold, intensity}, ...}")
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	var applied: Array = []
	for effect in body.keys():
		if effect in _ALL_EFFECTS:
			_settings_script.apply_effect(vp, effect, body[effect])
			applied.append(effect)
	return _ok({"applied": applied})


func _handle_effects_preset(body: Variant) -> PackedByteArray:
	if not body is Dictionary or not body.has("rendering"):
		return _err("Body must be JSON: {rendering: 'horror'}")
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	OhaoPresets.apply_rendering(vp, body["rendering"])
	return _ok({"preset": body["rendering"]})


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Physics
# ─────────────────────────────────────────────────────────────────────────────

func _handle_get_physics() -> PackedByteArray:
	return _ok({"state": _physics_state})


func _handle_physics_play() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp: return _err("No OhaoViewport found")
	vp.play_physics()
	_physics_state = "playing"
	return _ok({"state": "playing"})


func _handle_physics_pause() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp: return _err("No OhaoViewport found")
	vp.pause_physics()
	_physics_state = "paused"
	return _ok({"state": "paused"})


func _handle_physics_stop() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp: return _err("No OhaoViewport found")
	vp.stop_physics()
	_physics_state = "stopped"
	return _ok({"state": "stopped"})


func _handle_physics_step(body: Variant) -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp: return _err("No OhaoViewport found")
	var steps := 1
	if body is Dictionary and body.has("steps"):
		steps = int(body["steps"])
	steps = clampi(steps, 1, 300)
	for i in steps:
		vp.step_physics()
	_physics_state = "paused"
	return _ok({"stepped": steps, "state": "paused"})


func _handle_raycast(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {origin, direction, max_dist?, layer_mask?}")
	var vp := Ohao.viewport()
	if not vp: return _err("No OhaoViewport found")
	var origin    := _to_v3(body.get("origin",    [0,0,0]))
	var direction := _to_v3(body.get("direction", [0,0,-1]))
	var max_dist: float = float(body.get("max_dist", 100.0))
	var mask: int       = int(body.get("layer_mask", 0xFFFF))
	var hit := vp.cast_ray(origin, direction, max_dist, mask)
	if hit.get("hit", false):
		return _ok({
			"hit": true,
			"position":    _v3_to_a(hit["position"]),
			"normal":      _v3_to_a(hit["normal"]),
			"fraction":    hit.get("fraction", 0.0),
			"body_handle": hit.get("body_handle", -1),
		})
	return _ok({"hit": false})


# ─────────────────────────────────────────────────────────────────────────────
# Conversion helpers
# ─────────────────────────────────────────────────────────────────────────────

func _to_v3(v: Variant) -> Vector3:
	if v is Vector3: return v
	if v is Array and v.size() >= 3:
		return Vector3(float(v[0]), float(v[1]), float(v[2]))
	return Vector3.ZERO


func _to_color(v: Variant) -> Color:
	if v is Color: return v
	if v is Array:
		if v.size() >= 4: return Color(float(v[0]), float(v[1]), float(v[2]), float(v[3]))
		if v.size() >= 3: return Color(float(v[0]), float(v[1]), float(v[2]))
	return Color(0.7, 0.7, 0.8)


func _v3_to_a(v: Vector3) -> Array:
	return [snappedf(v.x, 0.001), snappedf(v.y, 0.001), snappedf(v.z, 0.001)]


func _actor_entry_from_dict(d: Dictionary) -> Dictionary:
	return {
		"name":  d.get("name", ""),
		"type":  d.get("type", "cube"),
		"pos":   _v3_to_a(_to_v3(d.get("pos",   [0,0,0]))),
		"rot":   _v3_to_a(_to_v3(d.get("rot",   [0,0,0]))),
		"scale": _v3_to_a(_to_v3(d.get("scale", [1,1,1]))),
	}


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — God Mode (MCP Perception)
# ─────────────────────────────────────────────────────────────────────────────

func _handle_god_state() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	return _ok(vp.get_scene_state())


func _handle_god_screenshot(body: Variant) -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	if not body is Dictionary or not body.has("path"):
		return _err("Body must be JSON: {path: 'C:/tmp/shot.png'}")
	var path: String = body["path"]
	var ok := vp.save_screenshot(path)
	if ok:
		return _ok({"saved": true, "path": path})
	return _err("Failed to save screenshot to: " + path)


func _handle_god_capture() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	var png_bytes := vp.capture_screenshot()
	if png_bytes.is_empty():
		return _err("Failed to capture screenshot (no image data)")
	var size := vp.get_viewport_size()
	return _ok({
		"image_base64": Marshalls.raw_to_base64(png_bytes),
		"format": "png",
		"width": size.x,
		"height": size.y,
	})


func _handle_god_snapshot(body: Variant) -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	if not body is Dictionary or not body.has("output_dir"):
		return _err("Body must be JSON: {output_dir: 'C:/tmp/snap'}")
	var result := OhaoGod.snapshot(vp, body["output_dir"])
	return _ok(result)


func _handle_god_look_at(body: Variant) -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	if not body is Dictionary or not body.has("target"):
		return _err("Body must be JSON: {target: [x,y,z], distance?, elevation?, azimuth?}")
	var target := _to_v3(body["target"])
	var distance: float = float(body.get("distance", 10.0))
	var elevation: float = float(body.get("elevation", 30.0))
	var azimuth: float = float(body.get("azimuth", 0.0))
	OhaoGod.look_at(vp, target, distance, elevation, azimuth)
	return _ok({"target": _v3_to_a(target), "distance": distance,
				"elevation": elevation, "azimuth": azimuth})


func _handle_god_orbit_capture(body: Variant) -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	if not body is Dictionary or not body.has("target") or not body.has("output_dir"):
		return _err("Body must be JSON: {target, output_dir, distance?, count?}")
	var target := _to_v3(body["target"])
	var distance: float = float(body.get("distance", 10.0))
	var count: int = int(body.get("count", 4))
	var output_dir: String = body["output_dir"]
	var paths := OhaoGod.orbit_capture(vp, target, distance, count, output_dir)
	return _ok({"paths": paths, "count": paths.size()})


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Introspection (AI self-development)
# ─────────────────────────────────────────────────────────────────────────────

func _handle_introspect_pipeline() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	return _ok(vp.get_pipeline_info())


func _handle_introspect_perf() -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	return _ok(vp.get_perf_stats())


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Hot-reload (AI shader experiments)
# ─────────────────────────────────────────────────────────────────────────────

func _handle_hot_reload_shader(body: Variant) -> PackedByteArray:
	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")
	if not body is Dictionary or not body.has("pass_name") or not body.has("spv_path"):
		return _err("Body must be JSON: {pass_name, spv_path}")
	var pass_name: String = body["pass_name"]
	var spv_path: String = body["spv_path"]
	var ok: bool = vp.reload_shader(pass_name, spv_path)
	if ok:
		return _ok({"reloaded": true, "pass": pass_name})
	else:
		return _err("Hot-reload failed for pass: %s" % pass_name)


func _handle_hot_reload_script(body: Variant) -> PackedByteArray:
	if not body is Dictionary or not body.has("path"):
		return _err("Body must be JSON: {path}")
	var script_path: String = body["path"]
	# Force reload the script resource
	var res = ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if res:
		return _ok({"reloaded": true, "path": script_path})
	else:
		return _err("Failed to reload script: %s" % script_path)


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — Event Subscription System
# ─────────────────────────────────────────────────────────────────────────────

func _connect_events() -> void:
	if _events_connected:
		return
	var vp = Ohao.viewport()
	if not vp:
		return
	if vp.has_signal("body_entered"):
		vp.body_entered.connect(_on_body_entered)
	if vp.has_signal("body_exited"):
		vp.body_exited.connect(_on_body_exited)
	if vp.has_signal("lightning_struck"):
		vp.lightning_struck.connect(_on_lightning_struck)
	if vp.has_signal("actor_selected"):
		vp.actor_selected.connect(_on_actor_selected)
	_events_connected = true


func _on_body_entered(body1: String, body2: String, contact: Vector3, impulse: float) -> void:
	_push_event("collision", {"body1": body1, "body2": body2,
		"contact": [contact.x, contact.y, contact.z], "impulse": impulse})


func _on_body_exited(body1: String, body2: String) -> void:
	_push_event("collision_end", {"body1": body1, "body2": body2})


func _on_lightning_struck() -> void:
	_push_event("lightning", {})


func _on_actor_selected(name: String) -> void:
	_push_event("actor_selected", {"name": name})


func _push_event(type: String, data: Dictionary) -> void:
	var evt := {"type": type, "timestamp": Time.get_unix_time_from_system(), "data": data}
	_event_buffer.append(evt)
	if _event_buffer.size() > _event_buffer_max:
		_event_buffer.pop_front()


func _handle_events_subscribe(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {event_types: ['collision', ...]}")
	if not _events_connected:
		_connect_events()
	var types: Array = body.get("event_types", ["all"])
	var sub_id := str(_next_sub_id)
	_next_sub_id += 1
	_subscriptions[sub_id] = {"event_types": types, "cursor": _event_buffer.size()}
	return _ok({"subscription_id": sub_id, "event_types": types})


func _handle_events_poll(query: Dictionary) -> PackedByteArray:
	var sub_id: String = query.get("sub", "")
	if sub_id == "":
		# No subscription — return last 20 events
		var start := max(0, _event_buffer.size() - 20)
		var recent := _event_buffer.slice(start)
		return _ok({"events": recent, "count": recent.size()})
	if sub_id not in _subscriptions:
		return _err("Unknown subscription: " + sub_id)
	var sub: Dictionary = _subscriptions[sub_id]
	var cursor: int = sub["cursor"]
	var types: Array = sub["event_types"]
	var events: Array = []
	for i in range(cursor, _event_buffer.size()):
		var evt: Dictionary = _event_buffer[i]
		if "all" in types or evt["type"] in types:
			events.append(evt)
	sub["cursor"] = _event_buffer.size()
	return _ok({"events": events, "count": events.size(), "subscription_id": sub_id})


func _handle_events_unsubscribe(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {subscription_id: '1'}")
	var sub_id: String = body.get("subscription_id", "")
	if sub_id in _subscriptions:
		_subscriptions.erase(sub_id)
		return _ok({"unsubscribed": sub_id})
	return _err("Unknown subscription: " + sub_id)


func _handle_events_history(query: Dictionary) -> PackedByteArray:
	var count: int = int(query.get("count", "50"))
	count = clampi(count, 1, 200)
	var type_filter: String = query.get("type", "")
	var events: Array = []
	var start := max(0, _event_buffer.size() - count)
	for i in range(start, _event_buffer.size()):
		var evt: Dictionary = _event_buffer[i]
		if type_filter == "" or evt["type"] == type_filter:
			events.append(evt)
	return _ok({"events": events, "count": events.size()})


# ─────────────────────────────────────────────────────────────────────────────
# Handlers — AI Player (autonomous gameplay)
# ─────────────────────────────────────────────────────────────────────────────

func _handle_god_play(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {actions: ['move_forward','jump'], mouse_delta?: [dx,dy]}")

	# Inject named input actions
	var actions: Array = body.get("actions", [])
	var released: Array = body.get("release", [])
	for action_name in released:
		if InputMap.has_action(action_name):
			Input.action_release(action_name)
	for action_name in actions:
		if InputMap.has_action(action_name):
			Input.action_press(action_name)

	# Inject mouse look delta (for FPS camera)
	if body.has("mouse_delta"):
		var delta: Array = body["mouse_delta"]
		if delta.size() >= 2:
			var event := InputEventMouseMotion.new()
			event.relative = Vector2(float(delta[0]), float(delta[1]))
			Input.parse_input_event(event)

	# Direct camera rotation (easier for AI than pixel deltas)
	if body.has("look_at_position"):
		var vp := Ohao.viewport()
		if vp:
			var target := _to_v3(body["look_at_position"])
			var cam_pos := vp.get_camera_position()
			var dir := (target - cam_pos).normalized()
			var pitch := rad_to_deg(asin(dir.y))
			var yaw := rad_to_deg(atan2(dir.x, -dir.z))
			vp.set_camera_rotation_deg(pitch, -yaw - 90.0)

	return _ok({"pressed": actions, "released": released})


func _handle_god_game_state() -> PackedByteArray:
	# Convention: game managers in "game_manager" group implement get_game_state()
	var managers := get_tree().get_nodes_in_group("game_manager")
	var game_state: Dictionary = {}

	if managers.size() > 0:
		var mgr = managers[0]
		if mgr.has_method("get_game_state"):
			game_state = mgr.get_game_state()

	# Always include basic engine info
	var vp := Ohao.viewport()
	if vp:
		var stats := vp.get_render_stats()
		game_state["engine"] = {
			"fps": stats.get("fps", 0),
			"physics_state": _physics_state,
			"actor_count": _actors.size(),
		}
		# Camera info for spatial awareness
		game_state["camera"] = {
			"position": _v3_to_a(vp.get_camera_position()),
			"forward": _v3_to_a(vp.get_camera_forward()),
		}

	# Collect info from enemies group
	var enemies := get_tree().get_nodes_in_group("enemies")
	if enemies.size() > 0:
		var enemy_data: Array = []
		for e in enemies:
			# Prefer get_game_state() for full info, else build manually
			if e.has_method("get_game_state"):
				enemy_data.append(e.get_game_state())
			else:
				var info: Dictionary = {"name": e.name}
				if e.has_method("is_alive"):
					info["alive"] = e.is_alive()
				# Position: check parent Node3D (enemy AI is a plain Node)
				if e.has_method("get_position"):
					info["position"] = _v3_to_a(e.get_position())
				elif "global_position" in e:
					info["position"] = _v3_to_a(e.global_position)
				elif e.get_parent() is Node3D:
					info["position"] = _v3_to_a(e.get_parent().position)
				enemy_data.append(info)
		game_state["enemies"] = enemy_data
		game_state["enemies_alive"] = enemies.filter(func(e): return not e.has_method("is_alive") or e.is_alive()).size()

	# Collect info from player group
	var players := get_tree().get_nodes_in_group("player")
	if players.size() > 0:
		var p = players[0]
		if p.has_method("get_game_state"):
			game_state["player"] = p.get_game_state()
		else:
			# OhaoCharacter is a child — search children for get_game_state
			for child in p.get_children():
				if child.has_method("get_game_state"):
					game_state["player"] = child.get_game_state()
					break
		if not game_state.has("player") and "global_position" in p:
			game_state["player"] = {"position": _v3_to_a(p.global_position)}

	return _ok(game_state)


func _handle_god_step_and_observe(body: Variant) -> PackedByteArray:
	if not body is Dictionary:
		return _err("Body must be JSON: {actions?, steps?, capture?}")

	var vp := Ohao.viewport()
	if not vp:
		return _err("No OhaoViewport found")

	# 1. Inject actions (same as /god/play)
	var actions: Array = body.get("actions", [])
	var released: Array = body.get("release", [])
	for action_name in released:
		if InputMap.has_action(action_name):
			Input.action_release(action_name)
	for action_name in actions:
		if InputMap.has_action(action_name):
			Input.action_press(action_name)

	if body.has("mouse_delta"):
		var delta: Array = body["mouse_delta"]
		if delta.size() >= 2:
			var event := InputEventMouseMotion.new()
			event.relative = Vector2(float(delta[0]), float(delta[1]))
			Input.parse_input_event(event)

	if body.has("look_at_position"):
		var target := _to_v3(body["look_at_position"])
		var cam_pos := vp.get_camera_position()
		var dir := (target - cam_pos).normalized()
		var pitch := rad_to_deg(asin(dir.y))
		var yaw := rad_to_deg(atan2(dir.x, -dir.z))
		vp.set_camera_rotation_deg(pitch, -yaw - 90.0)

	# 2. Step physics
	var steps: int = int(body.get("steps", 1))
	steps = clampi(steps, 1, 60)
	for i in steps:
		vp.step_physics()

	# 3. Build observation response
	var result: Dictionary = {
		"stepped": steps,
		"actions": actions,
	}

	# Game state
	var game_state_response := _handle_god_game_state()
	# Parse the game state from our own response
	var game_json := JSON.new()
	var response_str := game_state_response.get_string_from_utf8()
	var body_start := response_str.find("\r\n\r\n")
	if body_start >= 0:
		var json_body := response_str.substr(body_start + 4)
		if game_json.parse(json_body) == OK:
			result["game_state"] = game_json.get_data()

	# 4. Optional screenshot (expensive — only if requested)
	var should_capture: bool = body.get("capture", false)
	if should_capture:
		var png_bytes := vp.capture_screenshot()
		if not png_bytes.is_empty():
			result["image_base64"] = Marshalls.raw_to_base64(png_bytes)
			result["format"] = "png"
			var size := vp.get_viewport_size()
			result["width"] = size.x
			result["height"] = size.y

	# 5. Recent events (collisions, etc.) since last step
	var recent_events: Array = []
	var start := max(0, _event_buffer.size() - 10)
	for i in range(start, _event_buffer.size()):
		recent_events.append(_event_buffer[i])
	result["events"] = recent_events

	return _ok(result)
