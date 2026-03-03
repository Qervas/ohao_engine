extends Control
## Sky / Weather System Test

const OhaoWeather = preload("res://addons/ohao_helpers/ohao_weather.gd")
##
## Tests:
##   Sky rendering at every time-of-day period
##   All 8 weather states (clear → sandstorm) with instant snap and timed cycle
##   God rays, rainbow, heat haze, aurora
##   Auto time advance at configurable speed
##
## Controls:
##   LEFT / RIGHT   — Step time backward / forward  30 min
##   A              — Toggle auto time advance
##   1-8            — Snap to weather state
##   T              — Cycle through named time presets
##   L              — Trigger lightning (works in stormy/rain)
##   G              — Toggle god rays
##   R              — Toggle rainbow
##   H              — Toggle heat haze
##   U              — Toggle aurora

@onready var vp: OhaoViewport = $OhaoViewport

const TIME_PRESETS := [
	"midnight", "predawn", "dawn", "sunrise",
	"morning", "noon", "afternoon", "golden",
	"sunset", "dusk", "evening", "night"
]
const WEATHER_STATES := [
	"clear", "partly_cloudy", "overcast", "foggy",
	"rain", "stormy", "blizzard", "sandstorm"
]

var _time_preset_idx: int  = 6   # afternoon
var _weather_idx: int      = 0   # clear
var _auto_advance: bool    = false
var _time_speed: float     = 0.2  # hours per second (fast for testing)
var _god_rays: bool        = false
var _rainbow: bool         = false
var _heat_haze: bool       = false
var _aurora: bool          = false
var _log_lines: Array[String] = []


func _ready() -> void:
	if not vp:
		push_error("OhaoViewport not found!")
		return
	call_deferred("_init_scene")


func _init_scene() -> void:
	vp.set_render_mode(1)  # Deferred — required for sky, weather, and post-processing
	vp.clear_scene()

	# Ground plane — large flat surface to catch sky colour and shadows
	vp.add_cube("Ground", Vector3(0, -0.1, 0), Vector3.ZERO,
		Vector3(60.0, 0.2, 60.0), Color(0.55, 0.52, 0.48))

	# Reference geometry — vivid colours to verify material pipeline under weather
	vp.add_cube("BoxLow",    Vector3(-8, 0.5, -4),  Vector3.ZERO, Vector3(2,1,2),   Color(0.85, 0.25, 0.20))
	vp.add_cube("BoxMid",    Vector3( 0, 1.0,  0),  Vector3.ZERO, Vector3(2,2,2),   Color(0.20, 0.65, 0.30))
	vp.add_cube("BoxTall",   Vector3( 8, 2.0,  4),  Vector3.ZERO, Vector3(2,4,2),   Color(0.20, 0.35, 0.85))
	vp.add_sphere("Ball",    Vector3( 0, 1.5, -8),  Vector3.ZERO, Vector3(1.5,1.5,1.5), Color(0.90, 0.80, 0.15))
	vp.add_cylinder("Pillar",Vector3(-4, 2.5,  6),  Vector3.ZERO, Vector3(0.8,5,0.8),   Color(0.70, 0.20, 0.70))

	vp.finish_sync()

	# Post-processing — just tonemapping and mild bloom
	vp.set_tonemapping_enabled(true)
	vp.set_tonemap_operator(OhaoConst.TONEMAP_ACES)
	vp.set_bloom_enabled(true)
	vp.set_bloom_threshold(0.70)
	vp.set_bloom_intensity(0.40)
	vp.set_exposure(1.0)

	# Sky on, clouds off initially
	vp.set_sky_enabled(true)
	vp.set_cloud_enabled(false)

	# Camera — wide orbit view showing ground + sky
	vp.set_camera_mode(OhaoConst.CAMERA_ORBIT)
	vp.set_camera_position(Vector3(0, 8, 28))
	vp.set_camera_rotation_deg(-16.0, 0.0)

	# Start at afternoon — angled sun lights vertical faces nicely
	vp.set_time_of_day(15.0)
	OhaoWeather.weather_preset(vp, "clear")

	_log("Sky/Weather test ready.")
	_log("LEFT/RIGHT=time  A=auto  1-8=weather  T=time-preset  L=lightning  G/R/H/U=effects")


# =============================================================================
# INPUT
# =============================================================================

func _unhandled_input(event: InputEvent) -> void:
	if not vp:
		return
	if not (event is InputEventKey and event.pressed):
		return

	match event.keycode:
		KEY_LEFT:
			var h := fposmod(vp.get_time_of_day() - 0.5, 24.0)
			vp.set_time_of_day(h)
			OhaoWeather.weather_preset(vp, WEATHER_STATES[_weather_idx])
			_log("Time → %.1fh  (%s)" % [h, _period_name(h)])

		KEY_RIGHT:
			var h := fposmod(vp.get_time_of_day() + 0.5, 24.0)
			vp.set_time_of_day(h)
			OhaoWeather.weather_preset(vp, WEATHER_STATES[_weather_idx])
			_log("Time → %.1fh  (%s)" % [h, _period_name(h)])

		KEY_A:
			_auto_advance = !_auto_advance
			_log("Auto advance → %s  (%.1f h/s)" % [("ON" if _auto_advance else "OFF"), _time_speed])

		KEY_T:
			_time_preset_idx = (_time_preset_idx + 1) % TIME_PRESETS.size()
			var pname: String = TIME_PRESETS[_time_preset_idx]
			OhaoWeather.preset(vp, pname)
			OhaoWeather.weather_preset(vp, WEATHER_STATES[_weather_idx])
			_log("Time preset → %s (%.1fh)" % [pname, vp.get_time_of_day()])

		KEY_1: _set_weather(0)
		KEY_2: _set_weather(1)
		KEY_3: _set_weather(2)
		KEY_4: _set_weather(3)
		KEY_5: _set_weather(4)
		KEY_6: _set_weather(5)
		KEY_7: _set_weather(6)
		KEY_8: _set_weather(7)

		KEY_L:
			vp.trigger_lightning()
			_log("Lightning triggered!")

		KEY_G:
			_god_rays = !_god_rays
			vp.set_god_rays_enabled(_god_rays)
			if _god_rays:
				vp.set_god_rays_intensity(0.7)
			_log("God rays → %s" % ("ON" if _god_rays else "OFF"))

		KEY_R:
			_rainbow = !_rainbow
			vp.set_rainbow_enabled(_rainbow)
			_log("Rainbow → %s" % ("ON" if _rainbow else "OFF"))

		KEY_H:
			_heat_haze = !_heat_haze
			vp.set_heat_haze_enabled(_heat_haze)
			if _heat_haze:
				vp.set_heat_haze_intensity(0.6)
			_log("Heat haze → %s" % ("ON" if _heat_haze else "OFF"))

		KEY_U:
			_aurora = !_aurora
			vp.set_aurora_enabled(_aurora)
			if _aurora:
				vp.set_aurora_intensity(0.9)
			_log("Aurora → %s  (best at night/predawn)" % ("ON" if _aurora else "OFF"))


func _set_weather(idx: int) -> void:
	_weather_idx = idx
	OhaoWeather.weather_preset(vp, WEATHER_STATES[idx])
	_log("Weather → %s" % WEATHER_STATES[idx])


# =============================================================================
# PROCESS
# =============================================================================

func _process(delta: float) -> void:
	if not vp:
		return
	if _auto_advance:
		var h := fposmod(vp.get_time_of_day() + _time_speed * delta, 24.0)
		vp.set_time_of_day(h)
		OhaoWeather.weather_preset(vp, WEATHER_STATES[_weather_idx])
	queue_redraw()


# =============================================================================
# HUD
# =============================================================================

func _draw() -> void:
	if not vp:
		return
	var font := ThemeDB.fallback_font
	var W: float = size.x
	var y := 10.0

	_hud("OHAO — Sky / Weather Test", 10, y, 17, Color.WHITE);  y += 28.0

	var h: float = vp.get_time_of_day()
	_hud("Time:  %.2fh  (%s)%s" % [h, _period_name(h),
		"  [AUTO]" if _auto_advance else ""],
		10, y, 13, Color(1.0, 0.9, 0.5));  y += 18.0

	_hud("Weather:  %s" % WEATHER_STATES[_weather_idx],
		10, y, 13, Color(0.5, 0.9, 1.0));  y += 18.0

	var effects: Array[String] = []
	if _god_rays: effects.append("GodRays")
	if _rainbow:  effects.append("Rainbow")
	if _heat_haze:effects.append("HeatHaze")
	if _aurora:   effects.append("Aurora")
	if effects.size() > 0:
		_hud("Effects:  " + ", ".join(effects), 10, y, 12, Color(0.6, 1.0, 0.6));  y += 16.0

	y += 6.0
	for i in range(maxi(0, _log_lines.size()-5), _log_lines.size()):
		_hud(_log_lines[i], 10, y, 11, Color(0.45, 0.45, 0.45));  y += 13.0

	# Controls on right
	var rx := W - 240.0
	var ry := 10.0
	var ctrl := [
		"── Time ──",
		"LEFT / RIGHT   ±30 min",
		"T              next preset",
		"A              auto advance",
		"",
		"── Weather ──",
		"1  clear",
		"2  partly cloudy",
		"3  overcast",
		"4  foggy",
		"5  rain",
		"6  stormy",
		"7  blizzard",
		"8  sandstorm",
		"",
		"── Effects ──",
		"L  lightning",
		"G  god rays",
		"R  rainbow",
		"H  heat haze",
		"U  aurora",
	]
	for line in ctrl:
		var col: Color
		if line.begins_with("─"):
			col = Color(0.4, 0.4, 0.4)
		elif line == "":
			col = Color.TRANSPARENT
		else:
			col = Color(0.55, 0.55, 0.55)
		if line != "":
			_hud(line, rx, ry, 11, col)
		ry += 14.0


func _hud(text: String, x: float, y: float, sz: int, col: Color) -> void:
	draw_string(ThemeDB.fallback_font, Vector2(x, y + sz), text,
		HORIZONTAL_ALIGNMENT_LEFT, -1, sz, col)


# =============================================================================
# HELPERS
# =============================================================================

func _period_name(h: float) -> String:
	if   h < 5.0:  return "night"
	elif h < 6.0:  return "predawn"
	elif h < 7.5:  return "sunrise"
	elif h < 11.0: return "morning"
	elif h < 14.0: return "noon"
	elif h < 16.5: return "afternoon"
	elif h < 18.5: return "golden hour"
	elif h < 20.0: return "sunset"
	elif h < 21.5: return "dusk"
	else:           return "night"


func _log(msg: String) -> void:
	print("[SkyTest] %s" % msg)
	_log_lines.append(msg)
	if _log_lines.size() > 40:
		_log_lines = _log_lines.slice(_log_lines.size() - 40)
