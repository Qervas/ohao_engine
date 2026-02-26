extends Node
## OhaoAI - AI texture generation via Pollinations.ai (free ~5000 images/day).
##
## Registered as "OhaoAI" autoload. Generates PBR materials from text descriptions.
##
## Usage:
##   OhaoAI.apply_ai_material(vp, "Floor", "worn stone wall")
##   # Or async:
##   OhaoAI.generate_material("worn stone wall")
##   await OhaoAI.material_generated  # returns {description, albedo_path, normal_path, metallic, roughness}

signal material_generated(description: String, result: Dictionary)
signal generation_failed(description: String, error: String)

const API_BASE := "https://gen.pollinations.ai/image/"
const GENERATED_DIR := "res://textures/generated/"
const CACHE_FILE := "res://textures/generated/.cache.json"

var _api_key: String = ""
var _cache: Dictionary = {}  # prompt_hash -> {albedo_path, normal_path, metallic, roughness}
var _pending: Dictionary = {}  # prompt_hash -> {description, vp, actor_name, http}


func _ready() -> void:
	_ensure_dir()
	_load_api_key()
	_load_cache()
	if _api_key.is_empty():
		print("[OhaoAI] WARNING: No API key found!")
	else:
		print("[OhaoAI] Ready. API key loaded (%d chars)" % _api_key.length())


## Generate a PBR material from a text description (async, emits signal).
func generate_material(description: String) -> void:
	var h := _hash_prompt(description)

	# Cache hit
	if _cache.has(h):
		material_generated.emit(description, _cache[h])
		return

	# Already in-flight
	if _pending.has(h):
		return

	if _api_key.is_empty():
		print("[OhaoAI] ERROR: No API key!")
		generation_failed.emit(description, "No API key. Set POLLINATIONS_API_KEY env var or call set_api_key().")
		return

	var http := HTTPRequest.new()
	add_child(http)
	http.timeout = 120.0

	var prompt := _craft_prompt(description)
	var url := API_BASE + prompt.uri_encode() + "?width=1024&height=1024&nologo=true&model=flux&seed=%d&key=%s" % [(hash(description) & 0x7FFFFFFF), _api_key]

	_pending[h] = {"description": description, "vp": null, "actor_name": "", "http": http}

	http.request_completed.connect(_on_request_completed.bind(h))
	print("[OhaoAI] Generating: '%s' ..." % description)
	var err := http.request(url)
	if err != OK:
		print("[OhaoAI] ERROR: request() returned %d" % err)
		_cleanup_pending(h)
		generation_failed.emit(description, "HTTP request failed: %d" % err)


## Generate + apply material to an actor in one call.
func apply_ai_material(vp, actor_name: String, description: String) -> void:
	var h := _hash_prompt(description)

	# Cache hit — apply immediately
	if _cache.has(h):
		_apply_cached(vp, actor_name, _cache[h])
		material_generated.emit(description, _cache[h])
		return

	# Store target for deferred application
	if _pending.has(h):
		_pending[h]["vp"] = vp
		_pending[h]["actor_name"] = actor_name
		return

	# Start generation, then apply on completion
	generate_material(description)
	if _pending.has(h):
		_pending[h]["vp"] = vp
		_pending[h]["actor_name"] = actor_name


## Set the Pollinations API key at runtime.
func set_api_key(key: String) -> void:
	_api_key = key


# ── Prompt Crafting ──────────────────────────────────────────────

func _craft_prompt(description: String) -> String:
	return "seamless tileable PBR game texture of %s, top-down flat surface, uniform lighting, no text, no watermark" % description


# ── PBR Inference ────────────────────────────────────────────────

func _infer_pbr_params(description: String) -> Dictionary:
	var d := description.to_lower()
	# Metallic materials
	for kw in ["metal", "steel", "iron", "chrome", "aluminum", "copper", "brass", "gold", "silver", "titanium"]:
		if d.contains(kw):
			var roughness := 0.3
			if d.contains("rust") or d.contains("corroded") or d.contains("worn"):
				roughness = 0.7
			elif d.contains("brushed") or d.contains("scratched"):
				roughness = 0.5
			elif d.contains("polished") or d.contains("mirror"):
				roughness = 0.15
			return {"metallic": 1.0, "roughness": roughness}
	# Non-metallic
	var roughness := 0.5
	if d.contains("stone") or d.contains("rock") or d.contains("concrete") or d.contains("brick"):
		roughness = 0.85
	elif d.contains("wood") or d.contains("plank") or d.contains("bark"):
		roughness = 0.6
	elif d.contains("glass") or d.contains("ice") or d.contains("crystal"):
		roughness = 0.1
	elif d.contains("marble") or d.contains("ceramic") or d.contains("tile"):
		roughness = 0.25
	elif d.contains("rubber") or d.contains("fabric") or d.contains("cloth"):
		roughness = 0.9
	elif d.contains("dirt") or d.contains("mud") or d.contains("sand") or d.contains("grass"):
		roughness = 0.95
	elif d.contains("leather"):
		roughness = 0.7
	elif d.contains("plastic"):
		roughness = 0.4
	elif d.contains("asphalt") or d.contains("tar"):
		roughness = 0.8
	return {"metallic": 0.0, "roughness": roughness}


# ── HTTP Response Handler ────────────────────────────────────────

func _on_request_completed(result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray, prompt_hash: String) -> void:
	var info: Dictionary = _pending.get(prompt_hash, {})
	var description: String = info.get("description", "")

	print("[OhaoAI] Response: result=%d, http=%d, body=%d bytes" % [result, response_code, body.size()])

	if result != HTTPRequest.RESULT_SUCCESS:
		_cleanup_pending(prompt_hash)
		generation_failed.emit(description, "HTTP error: result=%d" % result)
		return

	if response_code != 200:
		_cleanup_pending(prompt_hash)
		generation_failed.emit(description, "API error %d: %s" % [response_code, body.get_string_from_utf8().left(200)])
		return

	if body.size() < 100:
		_cleanup_pending(prompt_hash)
		generation_failed.emit(description, "Response too small (%d bytes)" % body.size())
		return

	# Detect image format from magic bytes and decode
	var img := Image.new()
	var load_err: int = ERR_FILE_UNRECOGNIZED
	if body[0] == 0x89 and body[1] == 0x50:  # PNG magic: \x89P
		load_err = img.load_png_from_buffer(body)
	elif body[0] == 0xFF and body[1] == 0xD8:  # JPEG magic: \xFF\xD8
		load_err = img.load_jpg_from_buffer(body)
	elif body[0] == 0x52 and body[1] == 0x49:  # WebP magic: RIFF
		load_err = img.load_webp_from_buffer(body)

	if load_err != OK:
		_cleanup_pending(prompt_hash)
		generation_failed.emit(description, "Failed to decode image (first bytes: %02x %02x, size: %d)" % [body[0], body[1], body.size()])
		return

	# Save as PNG
	var albedo_path := GENERATED_DIR + prompt_hash + "_albedo.png"
	img.save_png(ProjectSettings.globalize_path(albedo_path))
	print("[OhaoAI] Albedo saved: %s (%dx%d)" % [albedo_path, img.get_width(), img.get_height()])

	# Generate normal map from albedo (Sobel filter)
	var normal_img := _generate_normal_map(img, 5.0)
	var normal_path := GENERATED_DIR + prompt_hash + "_normal.png"
	normal_img.save_png(ProjectSettings.globalize_path(normal_path))
	print("[OhaoAI] Normal saved: %s" % normal_path)

	# Infer PBR
	var pbr := _infer_pbr_params(description)

	var result_dict := {
		"description": description,
		"albedo_path": albedo_path,
		"normal_path": normal_path,
		"metallic": pbr["metallic"],
		"roughness": pbr["roughness"],
	}

	# Cache
	_cache[prompt_hash] = result_dict
	_save_cache()

	# Apply to target actor if specified
	var vp = info.get("vp")
	var actor_name: String = info.get("actor_name", "")
	if vp and not actor_name.is_empty():
		_apply_cached(vp, actor_name, result_dict)

	_cleanup_pending(prompt_hash)
	material_generated.emit(description, result_dict)


# ── Apply Cached Material ────────────────────────────────────────

func _apply_cached(vp, actor_name: String, mat: Dictionary) -> void:
	if not is_instance_valid(vp):
		return
	vp.set_actor_texture(actor_name, mat["albedo_path"])
	vp.set_actor_normal_map(actor_name, mat["normal_path"])
	vp.set_actor_pbr(actor_name, mat["metallic"], mat["roughness"])


# ── Normal Map Generation (Sobel filter) ─────────────────────────

func _generate_normal_map(src: Image, strength: float = 5.0) -> Image:
	var w := src.get_width()
	var h := src.get_height()
	var gray := src.duplicate()
	gray.convert(Image.FORMAT_L8)
	var normal := Image.create(w, h, false, Image.FORMAT_RGB8)
	for y in h:
		for x in w:
			var tl: float = gray.get_pixel((x - 1 + w) % w, (y - 1 + h) % h).r
			var t: float  = gray.get_pixel(x, (y - 1 + h) % h).r
			var tr: float = gray.get_pixel((x + 1) % w, (y - 1 + h) % h).r
			var l: float  = gray.get_pixel((x - 1 + w) % w, y).r
			var r: float  = gray.get_pixel((x + 1) % w, y).r
			var bl: float = gray.get_pixel((x - 1 + w) % w, (y + 1) % h).r
			var b: float  = gray.get_pixel(x, (y + 1) % h).r
			var br: float = gray.get_pixel((x + 1) % w, (y + 1) % h).r
			var dx: float = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl)
			var dy: float = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr)
			dx *= strength
			dy *= strength
			var len: float = sqrt(dx * dx + dy * dy + 1.0)
			var nx: float = (-dx / len) * 0.5 + 0.5
			var ny: float = (-dy / len) * 0.5 + 0.5
			var nz: float = (1.0 / len) * 0.5 + 0.5
			normal.set_pixel(x, y, Color(nx, ny, nz))
	return normal


# ── Hashing ──────────────────────────────────────────────────────

func _hash_prompt(description: String) -> String:
	return description.strip_edges().to_lower().md5_text().left(12)


# ── Cache Persistence ────────────────────────────────────────────

func _load_cache() -> void:
	var path := ProjectSettings.globalize_path(CACHE_FILE)
	if not FileAccess.file_exists(path):
		return
	var f := FileAccess.open(path, FileAccess.READ)
	if not f:
		return
	var json := JSON.new()
	if json.parse(f.get_as_text()) == OK and json.data is Dictionary:
		_cache = json.data
	f.close()


func _save_cache() -> void:
	var path := ProjectSettings.globalize_path(CACHE_FILE)
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f:
		f.store_string(JSON.stringify(_cache, "\t"))
		f.close()


# ── API Key Loading ──────────────────────────────────────────────

func _load_api_key() -> void:
	# 1. Environment variable
	var env_key := OS.get_environment("POLLINATIONS_API_KEY")
	if not env_key.is_empty():
		_api_key = env_key
		return

	# 2. .env file in project root
	var env_path := ProjectSettings.globalize_path("res://.env")
	if FileAccess.file_exists(env_path):
		var f := FileAccess.open(env_path, FileAccess.READ)
		if f:
			while not f.eof_reached():
				var line := f.get_line().strip_edges()
				if line.begins_with("POLLINATIONS_API_KEY="):
					_api_key = line.substr(21).strip_edges()
					break
			f.close()


# ── Directory Setup ──────────────────────────────────────────────

func _ensure_dir() -> void:
	var global_path := ProjectSettings.globalize_path(GENERATED_DIR)
	if not DirAccess.dir_exists_absolute(global_path):
		DirAccess.make_dir_recursive_absolute(global_path)


# ── Cleanup ──────────────────────────────────────────────────────

func _cleanup_pending(prompt_hash: String) -> void:
	if _pending.has(prompt_hash):
		var http = _pending[prompt_hash].get("http")
		if http and is_instance_valid(http):
			http.queue_free()
		_pending.erase(prompt_hash)
