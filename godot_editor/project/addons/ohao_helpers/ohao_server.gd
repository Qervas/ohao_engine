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
	# Track objects from the build dict
	if body.has("objects"):
		for obj in body["objects"]:
			if obj.has("name"):
				_actors[obj["name"]] = _actor_entry_from_dict(obj)
	OhaoSceneBuilder.build(vp, body)
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
