## OhaoWeather - Day/Night cycle + Weather state machine.
##
## ── STATIC TIME API ──────────────────────────────────────────────────────────
##   OhaoWeather.set_time(vp, 14.5)               # 2:30 PM
##   OhaoWeather.preset(vp, "sunset")              # named time preset
##   OhaoWeather.lerp_time(vp, 18.0, 2.0, delta)  # smooth advance per frame
##   OhaoWeather.get_sky_info(vp) -> Dict
##
## ── CLOCK NODE (day/night only) ──────────────────────────────────────────────
##   var clock = OhaoWeather.create_clock(vp, 6.0, 0.05)
##   clock.pause() / resume() / stop()
##   clock.set_time(12.0) / get_time() / is_day()
##
## ── WEATHER CONTROLLER NODE (day/night + weather transitions) ────────────────
##   var wx = OhaoWeather.create_controller(vp, "clear", 12.0, 0.05)
##   wx.transition_to("stormy", 10.0)   # smooth 10-second transition
##   wx.snap_to("foggy")                # instant change
##   wx.get_current() -> String         # "stormy"
##   wx.get_target()  -> String         # while transitioning
##   wx.get_blend()   -> float          # 0..1 transition progress
##   wx.is_transitioning() -> bool
##   wx.set_time(8.0) / get_time() / set_time_scale(0.05) / is_day()
##   wx.pause_time() / resume_time()
##   wx.stop()
##
## ── WEATHER STATES ────────────────────────────────────────────────────────────
##   clear, partly_cloudy, overcast, foggy, rain, stormy, blizzard
##
## ── TIME PRESETS ──────────────────────────────────────────────────────────────
##   midnight, predawn, dawn, sunrise, morning, noon, afternoon,
##   golden, sunset, dusk, evening, night

class_name OhaoWeather


# ─── Named time presets (hours 0-24) ─────────────────────────────────────────
const TIMES: Dictionary = {
	"midnight":   0.0,
	"predawn":    4.5,
	"dawn":       5.5,
	"sunrise":    6.5,
	"morning":    9.0,
	"noon":      12.0,
	"afternoon": 15.0,
	"golden":    17.0,
	"sunset":    18.0,
	"dusk":      19.5,
	"evening":   21.0,
	"night":     22.5,
}


# ─── Weather states ───────────────────────────────────────────────────────────
# turb_add     : added to the time-of-day turbidity  (moisture, pollution)
# int_mult     : multiplied with time-of-day intensity (cloud dimming)
# cloud_*      : coverage [0-1], density, speed
# fog_density  : volumetric fog density (0=off, 0.02=light, 0.1=thick)
# fog_scatter  : in-scatter strength (0-1)
# fog_r/g/b    : fog/ambient color tint
const WEATHER_STATES: Dictionary = {
	"clear": {
		"turb_add":  0.0,  "int_mult":  1.00,
		"cloud_coverage": 0.00, "cloud_density": 0.45, "cloud_speed": 1.0,
		"fog_density": 0.000, "fog_scatter": 0.60,
		"fog_r": 0.70, "fog_g": 0.80, "fog_b": 1.00,
		"rain_intensity": 0.0,  "wind_x": 0.00,
		"snow_intensity": 0.0,  "snow_wind_x": 0.00,
	},
	"partly_cloudy": {
		"turb_add":  0.6,  "int_mult":  0.88,
		"cloud_coverage": 0.38, "cloud_density": 0.32, "cloud_speed": 1.5,
		"fog_density": 0.005, "fog_scatter": 0.50,
		"fog_r": 0.75, "fog_g": 0.82, "fog_b": 1.00,
		"rain_intensity": 0.0,  "wind_x": -0.04,
		"snow_intensity": 0.0,  "snow_wind_x": -0.04,
	},
	"overcast": {
		"turb_add":  3.0,  "int_mult":  0.42,
		"cloud_coverage": 0.87, "cloud_density": 0.62, "cloud_speed": 2.0,
		"fog_density": 0.018, "fog_scatter": 0.70,
		"fog_r": 0.65, "fog_g": 0.70, "fog_b": 0.75,
		"rain_intensity": 0.0,  "wind_x": -0.06,
		"snow_intensity": 0.0,  "snow_wind_x": -0.06,
	},
	"foggy": {
		"turb_add":  2.0,  "int_mult":  0.55,
		"cloud_coverage": 0.55, "cloud_density": 0.30, "cloud_speed": 0.3,
		"fog_density": 0.065, "fog_scatter": 0.92,
		"fog_r": 0.80, "fog_g": 0.82, "fog_b": 0.85,
		"rain_intensity": 0.0,  "wind_x": 0.00,
		"snow_intensity": 0.0,  "snow_wind_x": 0.00,
	},
	"rain": {
		"turb_add":  4.0,  "int_mult":  0.32,
		"cloud_coverage": 0.90, "cloud_density": 0.75, "cloud_speed": 3.0,
		"fog_density": 0.030, "fog_scatter": 0.85,
		"fog_r": 0.50, "fog_g": 0.55, "fog_b": 0.60,
		"rain_intensity": 0.75, "wind_x": -0.10,
		"snow_intensity": 0.0,  "snow_wind_x": -0.10,
	},
	"stormy": {
		"turb_add":  6.5,  "int_mult":  0.18,
		"cloud_coverage": 0.96, "cloud_density": 0.90, "cloud_speed": 6.0,
		"fog_density": 0.045, "fog_scatter": 0.80,
		"fog_r": 0.28, "fog_g": 0.32, "fog_b": 0.38,
		"rain_intensity": 1.00, "wind_x": -0.18,
		"snow_intensity": 0.0,  "snow_wind_x": -0.18,
	},
	"blizzard": {
		"turb_add":  3.5,  "int_mult":  0.50,
		"cloud_coverage": 0.92, "cloud_density": 0.70, "cloud_speed": 4.5,
		"fog_density": 0.090, "fog_scatter": 0.95,
		"fog_r": 0.88, "fog_g": 0.90, "fog_b": 0.95,
		"rain_intensity": 0.0,  "wind_x": -0.12,  # blizzard uses fog, not liquid rain
		"snow_intensity": 1.0,  "snow_wind_x": -0.12,
	},
}


# ─── Static time API ──────────────────────────────────────────────────────────

## Set the time of day directly (0.0 = midnight, 12.0 = noon, 23.99 = late night).
static func set_time(vp: OhaoViewport, hours: float) -> void:
	if vp:
		vp.set_time_of_day(fposmod(hours, 24.0))


## Jump to a named time preset (see TIMES).
static func preset(vp: OhaoViewport, name: String) -> void:
	if name in TIMES:
		set_time(vp, TIMES[name])
	else:
		push_warning("OhaoWeather.preset: unknown '%s'. Valid: %s" \
			% [name, ", ".join(TIMES.keys())])


## Apply a weather state immediately (no transition).
## Also resets time-of-day base so weather offset is applied cleanly.
static func weather_preset(vp: OhaoViewport, name: String) -> void:
	if not vp:
		return
	if name not in WEATHER_STATES:
		push_warning("OhaoWeather.weather_preset: unknown '%s'. Valid: %s" \
			% [name, ", ".join(WEATHER_STATES.keys())])
		return
	# Reset turbidity/intensity to pure time-of-day base
	vp.set_time_of_day(vp.get_time_of_day())
	_apply_state(vp, WEATHER_STATES[name])


## Smoothly advance time toward target_hour at speed (game-hours/sec).
## Call each frame; returns true when reached.
## Correctly wraps through midnight (e.g. 22 → 2).
static func lerp_time(vp: OhaoViewport, target_hour: float, speed: float,
		delta: float) -> bool:
	if not vp:
		return true
	var current := vp.get_time_of_day()
	var diff := fposmod(target_hour - current + 12.0, 24.0) - 12.0
	if absf(diff) < 0.02:
		vp.set_time_of_day(target_hour)
		return true
	var step := signf(diff) * minf(absf(diff), speed * delta)
	vp.set_time_of_day(fposmod(current + step, 24.0))
	return false


## Return a dict describing the current sky period.
## {"hours": 6.5, "period": "sunrise", "is_day": true}
static func get_sky_info(vp: OhaoViewport) -> Dictionary:
	if not vp:
		return {}
	var h := vp.get_time_of_day()
	var period: String
	if   h < 5.0:  period = "night"
	elif h < 6.0:  period = "predawn"
	elif h < 7.5:  period = "sunrise"
	elif h < 11.0: period = "morning"
	elif h < 14.0: period = "noon"
	elif h < 16.5: period = "afternoon"
	elif h < 18.5: period = "golden"
	elif h < 20.0: period = "sunset"
	elif h < 21.5: period = "dusk"
	else:           period = "night"
	return {"hours": h, "period": period, "is_day": h >= 5.5 and h < 20.5}


## List available time preset names.
static func list_time_presets() -> Array:
	return TIMES.keys()


## List available weather state names.
static func list_weather_states() -> Array:
	return WEATHER_STATES.keys()


# ─── Clock factory ────────────────────────────────────────────────────────────

## Create an auto-advancing day/night clock Node, added as child of vp.
## time_scale: game-hours per real second.
##   0.0028 → 1h per 6 min  (full day in ~2.4h real time)
##   0.0167 → 1h per 1 min  (full day in 24 real minutes)
##   0.05   → full day in ~8 min  (good for demos)
##   0.5    → full day in ~48 sec
static func create_clock(vp: OhaoViewport, start_hour: float = 12.0,
		time_scale: float = 0.05) -> Node:
	var clock := _WeatherClock.new()
	clock.name = "OhaoWeatherClock"
	clock._vp = vp
	clock._time_hours = fposmod(start_hour, 24.0)
	clock._time_scale = time_scale
	clock._running = time_scale > 0.0
	vp.add_child(clock)
	clock._apply()
	return clock


# ─── Weather controller factory ───────────────────────────────────────────────

## Create a full weather controller (day/night + weather transitions).
## Replaces create_clock() when you also want weather state management.
## initial_state: one of WEATHER_STATES keys (default "clear")
## start_hour: 0-24 (default 12.0 = noon)
## time_scale: game-hours per real second (default 0.05, 0 = frozen)
static func create_controller(vp: OhaoViewport, initial_state: String = "clear",
		start_hour: float = 12.0, time_scale: float = 0.05) -> Node:
	if initial_state not in WEATHER_STATES:
		push_warning("OhaoWeather.create_controller: unknown state '%s', using 'clear'" \
			% initial_state)
		initial_state = "clear"
	var ctrl := _WeatherController.new()
	ctrl.name = "OhaoWeatherController"
	ctrl._vp         = vp
	ctrl._time_hours = fposmod(start_hour, 24.0)
	ctrl._time_scale = time_scale
	ctrl._time_running = time_scale > 0.0
	ctrl._current    = initial_state
	ctrl._target     = initial_state
	ctrl._from_params = WEATHER_STATES[initial_state].duplicate()
	ctrl._to_params   = WEATHER_STATES[initial_state].duplicate()
	ctrl._blend       = 1.0
	vp.add_child(ctrl)
	ctrl._apply_lerped(ctrl._to_params)
	return ctrl


# ─── Internal: apply a weather param dict to a viewport ──────────────────────
# Assumes set_time_of_day() was already called this frame (base turb/int set).
static func _apply_state(vp: OhaoViewport, p: Dictionary) -> void:
	# Sky: read time-of-day base, apply weather offset/multiplier
	var base_turb: float = vp.get_sky_turbidity()
	var base_int:  float = vp.get_sky_intensity()
	vp.set_sky_turbidity(clampf(base_turb + p.get("turb_add", 0.0), 1.0, 10.0))
	vp.set_sky_intensity(maxf(0.01, base_int * p.get("int_mult", 1.0)))

	# Clouds
	var coverage: float = p.get("cloud_coverage", 0.0)
	vp.set_cloud_enabled(coverage > 0.01)
	vp.set_cloud_coverage(maxf(coverage, 0.0))
	vp.set_cloud_density(p.get("cloud_density", 0.45))
	vp.set_cloud_speed(p.get("cloud_speed", 1.0))

	# Volumetric fog
	var fog: float = p.get("fog_density", 0.0)
	vp.set_volumetrics_enabled(fog > 0.001)
	vp.set_volumetric_density(maxf(fog, 0.0))
	vp.set_volumetric_scattering(p.get("fog_scatter", 0.7))
	vp.set_fog_color(Color(p.get("fog_r", 0.7), p.get("fog_g", 0.8), p.get("fog_b", 1.0)))

	# Rain
	var rain: float = p.get("rain_intensity", 0.0)
	vp.set_rain_enabled(rain > 0.01)
	vp.set_rain_intensity(maxf(rain, 0.0))
	vp.set_rain_wind_x(p.get("wind_x", -0.08))

	# Snow
	var snow: float = p.get("snow_intensity", 0.0)
	vp.set_snow_enabled(snow > 0.01)
	vp.set_snow_intensity(maxf(snow, 0.0))
	vp.set_snow_wind_x(p.get("snow_wind_x", 0.0))


# ─────────────────────────────────────────────────────────────────────────────
# _WeatherClock — simple day/night clock Node (no weather).
# ─────────────────────────────────────────────────────────────────────────────
class _WeatherClock extends Node:
	var _vp: OhaoViewport = null
	var _time_hours: float = 12.0
	var _time_scale: float = 0.05
	var _running: bool = true

	func pause() -> void:   _running = false
	func resume() -> void:  _running = _time_scale > 0.0

	func stop() -> void:
		_running = false
		queue_free()

	func set_time_scale(scale: float) -> void:
		_time_scale = scale
		_running = scale > 0.0

	func get_time() -> float:
		return _time_hours

	func set_time(hours: float) -> void:
		_time_hours = fposmod(hours, 24.0)
		_apply()

	func is_day() -> bool:
		return _time_hours >= 5.5 and _time_hours < 20.5

	func _process(delta: float) -> void:
		if not _running or not is_instance_valid(_vp):
			return
		_time_hours = fposmod(_time_hours + _time_scale * delta, 24.0)
		_apply()

	func _apply() -> void:
		if is_instance_valid(_vp):
			_vp.set_time_of_day(_time_hours)


# ─────────────────────────────────────────────────────────────────────────────
# _WeatherController — day/night clock + weather state machine.
# Owns both time advancement and weather transitions.
# ─────────────────────────────────────────────────────────────────────────────
class _WeatherController extends Node:
	var _vp: OhaoViewport = null

	# Time
	var _time_hours:   float = 12.0
	var _time_scale:   float = 0.05
	var _time_running: bool  = true

	# Weather state machine
	var _current:    String = "clear"
	var _target:     String = "clear"
	var _blend:      float  = 1.0       # 0 = at _current, 1 = at _target
	var _duration:   float  = 5.0
	var _elapsed:    float  = 0.0
	var _from_params: Dictionary = {}
	var _to_params:   Dictionary = {}


	## Start a smooth transition to a new weather state.
	## duration: seconds for the full crossfade.
	func transition_to(weather_name: String, duration: float = 5.0) -> void:
		if weather_name not in OhaoWeather.WEATHER_STATES:
			push_warning("OhaoWeather: unknown state '%s'. Valid: %s" \
				% [weather_name, ", ".join(OhaoWeather.WEATHER_STATES.keys())])
			return
		# Snapshot current interpolated params as the FROM anchor
		_from_params = _lerp_params(_from_params, _to_params, _blend)
		_target  = weather_name
		_to_params = OhaoWeather.WEATHER_STATES[weather_name].duplicate()
		_duration = maxf(duration, 0.01)
		_elapsed  = 0.0
		_blend    = 0.0


	## Apply a weather state instantly (no crossfade).
	func snap_to(weather_name: String) -> void:
		if weather_name not in OhaoWeather.WEATHER_STATES:
			push_warning("OhaoWeather: unknown state '%s'" % weather_name)
			return
		_current   = weather_name
		_target    = weather_name
		_blend     = 1.0
		_from_params = OhaoWeather.WEATHER_STATES[weather_name].duplicate()
		_to_params   = _from_params
		_apply_lerped(_to_params)


	## Query current/target states.
	func get_current() -> String:     return _current
	func get_target()  -> String:     return _target
	func get_blend()   -> float:      return _blend
	func is_transitioning() -> bool:  return _blend < 1.0


	## Time controls.
	func set_time(hours: float) -> void:
		_time_hours = fposmod(hours, 24.0)

	func get_time() -> float:
		return _time_hours

	func set_time_scale(scale: float) -> void:
		_time_scale = scale
		_time_running = scale > 0.0

	func pause_time() -> void:   _time_running = false
	func resume_time() -> void:  _time_running = _time_scale > 0.0
	func is_day() -> bool:       return _time_hours >= 5.5 and _time_hours < 20.5


	## Remove this controller from the scene.
	func stop() -> void:
		_time_running = false
		queue_free()


	func _process(delta: float) -> void:
		if not is_instance_valid(_vp):
			return

		# 1. Advance time
		if _time_running:
			_time_hours = fposmod(_time_hours + _time_scale * delta, 24.0)

		# 2. Advance transition blend
		if _blend < 1.0:
			_elapsed += delta
			_blend = clampf(_elapsed / _duration, 0.0, 1.0)
			if _blend >= 1.0:
				_current = _target

		# 3. Interpolate weather params (smoothstep easing)
		var t := _smoothstep(_blend)
		var lerped := _lerp_params(_from_params, _to_params, t)

		# 4. Apply time-of-day (sets sun direction + base turbidity/intensity)
		_vp.set_time_of_day(_time_hours)

		# 5. Apply weather on top
		_apply_lerped(lerped)


	func _apply_lerped(p: Dictionary) -> void:
		# Sky: read back time-of-day base and apply weather offset/multiplier
		var base_turb: float = _vp.get_sky_turbidity()
		var base_int:  float = _vp.get_sky_intensity()
		_vp.set_sky_turbidity(clampf(base_turb + p.get("turb_add", 0.0), 1.0, 10.0))
		_vp.set_sky_intensity(maxf(0.01, base_int * p.get("int_mult", 1.0)))

		# Clouds
		var coverage: float = p.get("cloud_coverage", 0.0)
		_vp.set_cloud_enabled(coverage > 0.01)
		_vp.set_cloud_coverage(maxf(coverage, 0.0))
		_vp.set_cloud_density(p.get("cloud_density", 0.45))
		_vp.set_cloud_speed(p.get("cloud_speed", 1.0))

		# Volumetric fog
		var fog: float = p.get("fog_density", 0.0)
		_vp.set_volumetrics_enabled(fog > 0.001)
		_vp.set_volumetric_density(maxf(fog, 0.0))
		_vp.set_volumetric_scattering(p.get("fog_scatter", 0.7))
		_vp.set_fog_color(Color(p.get("fog_r", 0.7), p.get("fog_g", 0.8), p.get("fog_b", 1.0)))

		# Rain
		var rain: float = p.get("rain_intensity", 0.0)
		_vp.set_rain_enabled(rain > 0.01)
		_vp.set_rain_intensity(maxf(rain, 0.0))
		_vp.set_rain_wind_x(p.get("wind_x", -0.08))

		# Snow
		var snow: float = p.get("snow_intensity", 0.0)
		_vp.set_snow_enabled(snow > 0.01)
		_vp.set_snow_intensity(maxf(snow, 0.0))
		_vp.set_snow_wind_x(p.get("snow_wind_x", 0.0))


	func _lerp_params(a: Dictionary, b: Dictionary, t: float) -> Dictionary:
		return {
			"turb_add":       lerpf(a.get("turb_add",  0.0), b.get("turb_add",  0.0), t),
			"int_mult":       lerpf(a.get("int_mult",  1.0), b.get("int_mult",  1.0), t),
			"cloud_coverage": lerpf(a.get("cloud_coverage", 0.0), b.get("cloud_coverage", 0.0), t),
			"cloud_density":  lerpf(a.get("cloud_density",  0.45),b.get("cloud_density",  0.45),t),
			"cloud_speed":    lerpf(a.get("cloud_speed",    1.0), b.get("cloud_speed",    1.0), t),
			"fog_density":    lerpf(a.get("fog_density",    0.0), b.get("fog_density",    0.0), t),
			"fog_scatter":    lerpf(a.get("fog_scatter",    0.7), b.get("fog_scatter",    0.7), t),
			"fog_r":          lerpf(a.get("fog_r", 0.7), b.get("fog_r", 0.7), t),
			"fog_g":          lerpf(a.get("fog_g", 0.8), b.get("fog_g", 0.8), t),
			"fog_b":          lerpf(a.get("fog_b", 1.0), b.get("fog_b", 1.0), t),
			"rain_intensity": lerpf(a.get("rain_intensity", 0.0), b.get("rain_intensity", 0.0), t),
			"wind_x":         lerpf(a.get("wind_x", 0.0),         b.get("wind_x", 0.0),         t),
			"snow_intensity": lerpf(a.get("snow_intensity", 0.0), b.get("snow_intensity", 0.0), t),
			"snow_wind_x":    lerpf(a.get("snow_wind_x", 0.0),    b.get("snow_wind_x", 0.0),    t),
		}


	func _smoothstep(t: float) -> float:
		return t * t * (3.0 - 2.0 * t)
