extends Control
## Water Visual Test
##
## Comprehensively exercises all 8 water sub-features:
##   1  Gerstner Stokes wave asymmetry
##   2  Foam parallax depth
##   3  Caustics 3-layer + trough darkening
##   4  Ripple sources (up to 12 simultaneous)
##   5  Underwater distort params
##   6  Water splash particles
##   7  Adaptive grid LOD (32 / 64 / 128 / 256)
##
## Controls:
##   1-5     — Switch camera view
##   SPACE   — Spawn ripple burst + water splash at a random surface point
##   W       — Toggle Gerstner / FFT wave mode
##   G       — Cycle grid resolution  64 → 128 → 256 → 64
##   C       — Toggle caustics on/off
##   U       — Toggle underwater (submerge camera)
##   F       — Cycle foam intensity  low / mid / high
##   D       — Cycle distort frequency  low / mid / high
##   R       — Clear all ripples

@onready var vp: OhaoViewport = $OhaoViewport

# ── state ─────────────────────────────────────────────────────────────────────
var _view: int         = 1
var _wave_mode: int    = OhaoConst.WAVE_GERSTNER
var _grid_res: int     = 128
var _caustics_on: bool = true
var _underwater: bool  = false
var _foam_idx: int     = 1   # 0=low 1=mid 2=high
var _distort_idx: int  = 1   # 0=low 1=mid 2=high
var _ripple_count: int = 0
var _time: float       = 0.0
var _splash_pts: Array = []   # world positions for last spawned splashes
var _log_lines: Array[String] = []

# ── tuning tables ──────────────────────────────────────────────────────────────
const GRID_RESOLUTIONS  := [64, 128, 256]
const FOAM_LEVELS       := [0.25, 0.65, 1.0]
const DISTORT_FREQS     := [5.0, 12.0, 28.0]   # slow / default / fast
const DISTORT_LABELS    := ["slow", "default", "fast"]
const FOAM_LABELS       := ["low", "mid", "full"]

# ── view configurations ───────────────────────────────────────────────────────
## Each view: [pos, pitch_deg, yaw_deg, label, description]
const VIEWS := {
	1: {
		"pos":  Vector3(0, 14, 28),
		"pitch": -28.0, "yaw": 0.0,
		"label": "Wave Overview",
		"desc":  "Orbit view — Stokes asymmetry gives forward-leaning wave fronts"
	},
	2: {
		"pos":  Vector3(-30, 1.2, 0),
		"pitch": -3.0, "yaw": 90.0,
		"label": "Surface Skim",
		"desc":  "Side-on 1 m above surface — asymmetric wave fronts + foam parallax depth"
	},
	3: {
		"pos":  Vector3(0, -3.5, 12),
		"pitch": -12.0, "yaw": 180.0,
		"label": "Caustics",
		"desc":  "Underwater looking at seabed — 3-layer caustics + trough darkening"
	},
	4: {
		"pos":  Vector3(0, -5.0, 0),
		"pitch": -5.0, "yaw": 20.0,
		"label": "Underwater Fog",
		"desc":  "Deep underwater — lens warp, chromatic abberation, blue depth fog"
	},
	5: {
		"pos":  Vector3(0, 22, 2),
		"pitch": -86.0, "yaw": 0.0,
		"label": "Ripple / Splash",
		"desc":  "Top-down — press SPACE to spawn up to 12 simultaneous ripples + splash particles"
	},
}


func _ready() -> void:
	if not vp:
		push_error("OhaoViewport not found!")
		return
	call_deferred("_init_scene")


func _init_scene() -> void:
	vp.clear_scene()

	# ── Lighting ──────────────────────────────────────────────────────────────
	# Warm sun at shallow angle — gives strong specular glints on waves
	vp.add_directional_light("Sun", Vector3(0, 30, 0),
		Vector3(-0.4, -0.85, -0.35), Color(1.0, 0.96, 0.85), 3.8)
	# Cool fill from opposite side for colour contrast
	vp.add_point_light("FillBlue", Vector3(10, 8, -15),
		Color(0.4, 0.55, 1.0), 0.9, 40.0)

	# ── Post-processing ───────────────────────────────────────────────────────
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)
	vp.set_bloom_enabled(true)
	vp.set_bloom_threshold(0.65)
	vp.set_bloom_intensity(0.55)
	vp.set_ssao_enabled(true)
	vp.set_ssao_intensity(0.6)
	vp.set_ssao_radius(1.2)
	vp.set_exposure(1.05)

	# ── Terrain — hills that dip below water level ────────────────────────────
	# height_scale=6 with HILLS type puts valley floors around y=-4
	# which gives the caustics pass plenty of submerged geometry to write to
	vp.set_terrain_enabled(true)
	vp.set_terrain_type(OhaoConst.TERRAIN_HILLS)
	vp.set_terrain_size(200.0)
	vp.set_terrain_height_scale(6.0)
	vp.set_terrain_frequency(0.7)
	vp.set_terrain_octaves(5)
	vp.generate_terrain()

	# ── Reference geometry ────────────────────────────────────────────────────
	# Submerged flat slabs — best caustics surface (flat = undistorted pattern)
	vp.add_cube("Slab1",  Vector3( 6,  -4.8,  8), Vector3.ZERO, Vector3(5.0, 0.4, 5.0), Color(0.62, 0.58, 0.50))
	vp.add_cube("Slab2",  Vector3(-8,  -5.5, -6), Vector3.ZERO, Vector3(6.0, 0.4, 4.0), Color(0.58, 0.54, 0.48))
	vp.add_cube("Slab3",  Vector3( 2,  -3.8,  0), Vector3.ZERO, Vector3(3.0, 0.4, 7.0), Color(0.65, 0.60, 0.52))

	# Partially submerged sea-stacks — create shore-foam zones
	vp.add_cylinder("Stack1", Vector3(-9,  -1.0,  -4), Vector3.ZERO, Vector3(1.4, 5.0, 1.4), Color(0.52, 0.49, 0.45))
	vp.add_cylinder("Stack2", Vector3(-6,  -0.5,  -8), Vector3.ZERO, Vector3(0.9, 3.0, 0.9), Color(0.48, 0.45, 0.42))
	vp.add_cylinder("Stack3", Vector3(-11, -2.0,  -7), Vector3.ZERO, Vector3(1.6, 6.0, 1.6), Color(0.55, 0.51, 0.47))
	vp.add_cylinder("Stack4", Vector3( 14,  0.2,   3), Vector3.ZERO, Vector3(2.0, 4.0, 2.0), Color(0.50, 0.47, 0.44))

	# Shore wall for edge-foam banding
	vp.add_cube("ShoreWall", Vector3(16, 0.0, 0), Vector3.ZERO, Vector3(0.5, 4.0, 20.0), Color(0.6, 0.57, 0.52))

	# Observation platform above water (camera reference point)
	vp.add_cube("Platform",  Vector3(0, 1.2, 0), Vector3.ZERO, Vector3(4.0, 0.4, 4.0), Color(0.7, 0.65, 0.55))

	vp.finish_sync()

	# ── Water ─────────────────────────────────────────────────────────────────
	vp.set_water_enabled(true)
	vp.set_water_level(0.0)
	vp.set_water_size(200.0)
	vp.set_water_wave_amplitude(0.55)
	vp.set_water_foam_intensity(FOAM_LEVELS[_foam_idx])
	vp.set_water_sss_strength(0.45)

	# Adaptive grid — 128x128 with exponential vertex spacing
	vp.set_water_grid_resolution(_grid_res)

	# ── Caustics ──────────────────────────────────────────────────────────────
	vp.set_caustics_enabled(true)
	vp.set_caustics_intensity(0.85)
	vp.set_caustics_scale(0.06)  # new binding — controls caustic pattern size

	# ── Ripples ───────────────────────────────────────────────────────────────
	vp.set_water_ripples_enabled(true)
	vp.set_water_ripple_damping(0.003)  # new binding — slow decay for lingering rings
	vp.set_water_ripple_speed(9.5)      # new binding — wave propagation speed (m/s)

	# ── Underwater atmosphere ─────────────────────────────────────────────────
	vp.set_underwater_enabled(true)
	vp.set_underwater_fog_color(Color(0.04, 0.16, 0.32))
	vp.set_underwater_fog_density(0.14)
	vp.set_underwater_distort_frequency(DISTORT_FREQS[_distort_idx])  # new binding
	vp.set_underwater_distort_speed(1.1)  # new binding

	# ── Initial view ──────────────────────────────────────────────────────────
	_set_view(1)
	_log("Water Visual Test ready.  1-5=view  SPACE=ripple+splash  W=wave  G=grid  C=caustics  U=under  F=foam  D=distort")


# =============================================================================
# VIEW MANAGEMENT
# =============================================================================

func _set_view(n: int) -> void:
	_view = n
	if not VIEWS.has(n):
		return
	var cfg: Dictionary = VIEWS[n]
	vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)
	vp.set_camera_position(cfg["pos"])
	vp.set_camera_rotation_deg(cfg["pitch"], cfg["yaw"])
	# Auto-configure underwater state based on camera depth
	var cam_y: float = (cfg["pos"] as Vector3).y
	_set_underwater_state(cam_y < 0.0)
	_log("View %d: %s — %s" % [n, cfg["label"], cfg["desc"]])


func _set_underwater_state(submerged: bool) -> void:
	_underwater = submerged
	# Only bother enabling the pass when the camera is actually below waterLevel
	vp.set_underwater_enabled(submerged)
	if submerged:
		_log("  (camera submerged — underwater effects active)")


# =============================================================================
# INPUT
# =============================================================================

func _unhandled_input(event: InputEvent) -> void:
	if not vp:
		return
	if not (event is InputEventKey and event.pressed):
		return

	match event.keycode:
		KEY_1: _set_view(1)
		KEY_2: _set_view(2)
		KEY_3: _set_view(3)
		KEY_4: _set_view(4)
		KEY_5: _set_view(5)

		KEY_SPACE:
			_spawn_ripple_burst()

		KEY_W:
			_cycle_wave_mode()

		KEY_G:
			_cycle_grid_resolution()

		KEY_C:
			_toggle_caustics()

		KEY_U:
			_toggle_underwater_manual()

		KEY_F:
			_cycle_foam()

		KEY_D:
			_cycle_distort()

		KEY_R:
			vp.clear_water_ripples()
			_ripple_count = 0
			_log("Ripples cleared.")


# =============================================================================
# FEATURE TOGGLES
# =============================================================================

func _cycle_wave_mode() -> void:
	_wave_mode = 1 - _wave_mode   # toggle 0↔1
	vp.set_wave_mode(_wave_mode)
	var mode_str := "FFT (Tessendorf)" if _wave_mode == OhaoConst.WAVE_FFT else "Gerstner+Stokes"
	_log("Wave mode → %s" % mode_str)


func _cycle_grid_resolution() -> void:
	# Find current index
	var idx: int = GRID_RESOLUTIONS.find(_grid_res)
	idx = (idx + 1) % GRID_RESOLUTIONS.size()
	_grid_res = GRID_RESOLUTIONS[idx]
	vp.set_water_grid_resolution(_grid_res)
	_log("Grid LOD → %d×%d (exponential spacing, camera-centred)" % [_grid_res, _grid_res])


func _toggle_caustics() -> void:
	_caustics_on = !_caustics_on
	vp.set_caustics_enabled(_caustics_on)
	_log("Caustics → %s  (3-layer + trough darkening)" % ("ON" if _caustics_on else "OFF"))


func _toggle_underwater_manual() -> void:
	_underwater = !_underwater
	vp.set_underwater_enabled(_underwater)
	_log("Underwater effects → %s" % ("ON" if _underwater else "OFF"))


func _cycle_foam() -> void:
	_foam_idx = (_foam_idx + 1) % FOAM_LEVELS.size()
	vp.set_water_foam_intensity(FOAM_LEVELS[_foam_idx])
	_log("Foam intensity → %s (%.2f)  (parallax offset scales with crest height)" \
		% [FOAM_LABELS[_foam_idx], FOAM_LEVELS[_foam_idx]])


func _cycle_distort() -> void:
	_distort_idx = (_distort_idx + 1) % DISTORT_FREQS.size()
	var freq: float = DISTORT_FREQS[_distort_idx]
	vp.set_underwater_distort_frequency(freq)
	_log("Underwater distort → %s (freq=%.1f)  — compare lens warp pattern" \
		% [DISTORT_LABELS[_distort_idx], freq])


# =============================================================================
# RIPPLE BURST + WATER SPLASH
# =============================================================================

func _spawn_ripple_burst() -> void:
	_splash_pts.clear()

	# Spawn up to 8 ripples spread across the water surface to stress-test
	# the new 12-source limit (previously capped at 4)
	var positions: Array[Vector3] = [
		Vector3( 0.0,  0.0,  0.0),
		Vector3( 5.0,  0.0,  5.0),
		Vector3(-5.0,  0.0,  3.0),
		Vector3( 3.0,  0.0, -6.0),
		Vector3(-4.0,  0.0, -4.0),
		Vector3( 8.0,  0.0,  0.0),
		Vector3(-8.0,  0.0, -2.0),
		Vector3( 1.0,  0.0,  8.0),
	]

	for p in positions:
		var strength: float = randf_range(0.4, 1.0)
		vp.add_water_ripple(p, strength)
		_ripple_count += 1
		_splash_pts.append(p)

		# Water splash particles — new PARTICLE_WATER_SPLASH type
		vp.spawn_particles_directed(p, OhaoConst.PARTICLE_WATER_SPLASH, Vector3.UP)

	_log("Spawned %d ripples + splash particles  (capacity: 12 per frame)" \
		% positions.size())


# =============================================================================
# PROCESS / HUD
# =============================================================================

func _process(delta: float) -> void:
	_time += delta
	queue_redraw()


func _draw() -> void:
	if not vp:
		return

	var font  := ThemeDB.fallback_font
	var W: float = size.x
	var y := 10.0

	# ── Title ─────────────────────────────────────────────────────────────────
	_hud_line(font, 10, y, 17, "OHAO — Water System Visual Test", Color.WHITE)
	y += 26.0

	if VIEWS.has(_view):
		var v: Dictionary = VIEWS[_view]
		_hud_line(font, 10, y, 14, "View %d: %s" % [_view, v["label"]], Color(0.4, 0.9, 1.0))
		y += 20.0
		_hud_line(font, 14, y, 11, v["desc"], Color(0.65, 0.65, 0.65))
		y += 18.0

	# ── Feature status row ────────────────────────────────────────────────────
	y += 4.0
	var mode_str := "FFT" if _wave_mode == OhaoConst.WAVE_FFT else "Gerstner+Stokes"
	var status := [
		"Waves: %s"    % mode_str,
		"Grid: %d²"    % _grid_res,
		"Caustics: %s" % ("ON" if _caustics_on else "off"),
		"Foam: %s"     % FOAM_LABELS[_foam_idx],
		"Distort: %s"  % DISTORT_LABELS[_distort_idx],
		"Under: %s"    % ("ON" if _underwater else "off"),
		"Ripples this session: %d" % _ripple_count,
	]
	for s in status:
		_hud_line(font, 10, y, 11, s, Color(0.6, 0.85, 0.6))
		y += 14.0

	# ── Log ───────────────────────────────────────────────────────────────────
	y += 6.0
	var log_start := maxi(0, _log_lines.size() - 4)
	for i in range(log_start, _log_lines.size()):
		_hud_line(font, 10, y, 11, _log_lines[i], Color(0.45, 0.45, 0.45))
		y += 13.0

	# ── Controls (right side) ─────────────────────────────────────────────────
	var rx := W - 230.0
	var ry := 10.0
	var controls := [
		"── Views ──",
		"1  Wave overview",
		"2  Surface skim (asymmetry)",
		"3  Caustics underwater",
		"4  Deep underwater fog",
		"5  Top-down ripple spawn",
		"",
		"── Features ──",
		"SPACE  Ripple burst + splash",
		"W  Cycle wave mode",
		"G  Cycle grid LOD",
		"C  Toggle caustics",
		"F  Cycle foam intensity",
		"D  Cycle distort freq",
		"U  Toggle underwater",
		"R  Clear ripples",
	]
	for line in controls:
		var col := Color(0.35, 0.35, 0.35) if line.begins_with("─") else \
			(Color(0.5, 0.5, 0.5) if line != "" else Color.TRANSPARENT)
		if line != "":
			_hud_line(font, rx, ry, 11, line, col)
		ry += 14.0


func _hud_line(font: Font, x: float, y: float, size: int, text: String, col: Color) -> void:
	draw_string(font, Vector2(x, y + size), text,
		HORIZONTAL_ALIGNMENT_LEFT, -1, size, col)


# =============================================================================
# HELPERS
# =============================================================================

var _log_lines_raw: Array[String] = []

func _log(msg: String) -> void:
	print("[WaterTest] %s" % msg)
	_log_lines.append(msg)
	if _log_lines.size() > 40:
		_log_lines = _log_lines.slice(_log_lines.size() - 40)
