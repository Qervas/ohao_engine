## OhaoWeather - Day/Night cycle and weather controller.
##
## Static API — access via Ohao.weather():
##   OhaoWeather.set_time(vp, 14.5)           # 2:30 PM
##   OhaoWeather.preset(vp, "sunset")          # jump to named time
##   OhaoWeather.get_sky_info(vp) -> Dict      # current period info
##
## Auto-advancing clock:
##   var clock = OhaoWeather.create_clock(vp, 6.0, 0.05)
##   # time_scale = game-hours per real second
##   # 0.05 → full 24h day in 480 real seconds (8 minutes)
##   clock.pause() / clock.resume() / clock.stop()
##   clock.set_time(12.0) / clock.get_time() -> float
##   clock.set_time_scale(0.1)
##   clock.is_day() -> bool
##
## Named time transitions (call each frame until done):
##   var done = OhaoWeather.lerp_time(vp, 18.0, 2.0, delta)
##
## Weather presets (placeholder — full weather system coming):
##   OhaoWeather.weather_preset(vp, "golden_hour")

class_name OhaoWeather

## Named times (hours, 0-24)
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

## Named weather combinations (time + sky params)
const WEATHER_PRESETS: Dictionary = {
	"clear_noon":    {"time": 12.0, "turbidity": 2.0, "intensity": 1.0},
	"golden_hour":   {"time": 17.0, "turbidity": 4.5, "intensity": 0.7},
	"overcast_day":  {"time": 12.0, "turbidity": 8.0, "intensity": 0.4},
	"stormy_eve":    {"time": 18.5, "turbidity": 9.0, "intensity": 0.25},
	"blue_hour":     {"time": 19.8, "turbidity": 3.0, "intensity": 0.12},
	"moonlit_night": {"time":  1.0, "turbidity": 2.0, "intensity": 0.05},
}


## Set the time of day directly (0.0 = midnight, 12.0 = noon).
## The C++ side computes sun direction, turbidity, and intensity automatically.
static func set_time(vp: OhaoViewport, hours: float) -> void:
	if vp:
		vp.set_time_of_day(fposmod(hours, 24.0))


## Jump to a named time preset.
## Valid names: midnight, predawn, dawn, sunrise, morning, noon,
##              afternoon, golden, sunset, dusk, evening, night
static func preset(vp: OhaoViewport, name: String) -> void:
	if name in TIMES:
		set_time(vp, TIMES[name])
	else:
		push_warning("OhaoWeather.preset: unknown preset '%s'. Valid: %s" \
			% [name, ", ".join(TIMES.keys())])


## Apply a combined weather preset (time + sky overrides).
## Valid names: clear_noon, golden_hour, overcast_day, stormy_eve,
##              blue_hour, moonlit_night
static func weather_preset(vp: OhaoViewport, name: String) -> void:
	if not vp:
		return
	if name not in WEATHER_PRESETS:
		push_warning("OhaoWeather.weather_preset: unknown preset '%s'. Valid: %s" \
			% [name, ", ".join(WEATHER_PRESETS.keys())])
		return
	var p: Dictionary = WEATHER_PRESETS[name]
	# Set time first (auto-computes sun direction + baseline sky params)
	vp.set_time_of_day(p["time"])
	# Override turbidity and intensity with preset values
	vp.set_sky_turbidity(p["turbidity"])
	vp.set_sky_intensity(p["intensity"])


## Create an auto-advancing clock Node attached to vp.
## time_scale: game-hours per real second.
##   Common values:
##     0.0028  → 1 game hour per ~6 real minutes (very slow, ambient)
##     0.0167  → 1 game hour per 1 real minute
##     0.05    → full day in ~8 real minutes (good for testing)
##     0.5     → full day in ~48 real seconds (fast)
##     1.0     → 1 game hour per real second (very fast)
## Returns the clock Node so you can control it later.
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


## Smoothly advance time toward target_hour at speed game-hours/sec.
## Call each frame; returns true when the target is reached.
## Wraps around midnight correctly.
##   while not OhaoWeather.lerp_time(vp, 18.0, 2.0, delta): pass
static func lerp_time(vp: OhaoViewport, target_hour: float, speed: float,
		delta: float) -> bool:
	if not vp:
		return true
	var current := vp.get_time_of_day()
	# Shortest-path difference on the 24h circle
	var diff := fposmod(target_hour - current + 12.0, 24.0) - 12.0
	if absf(diff) < 0.02:
		vp.set_time_of_day(target_hour)
		return true
	var step := signf(diff) * minf(absf(diff), speed * delta)
	vp.set_time_of_day(fposmod(current + step, 24.0))
	return false


## Return a dict describing the current sky period.
## {"hours": 14.5, "period": "afternoon", "is_day": true}
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
	return {
		"hours":   h,
		"period":  period,
		"is_day":  h >= 5.5 and h < 20.5,
	}


## List available named time presets.
static func list_presets() -> Array:
	return TIMES.keys()


## List available weather presets.
static func list_weather_presets() -> Array:
	return WEATHER_PRESETS.keys()


# ─────────────────────────────────────────────────────────────────────────────
# Inner clock Node — auto-advances time each frame.
# Created by create_clock(); users hold a reference to control it.
# ─────────────────────────────────────────────────────────────────────────────
class _WeatherClock extends Node:
	var _vp: OhaoViewport = null
	var _time_hours: float = 12.0
	var _time_scale: float = 0.05  # game-hours per real second
	var _running: bool = true

	## Pause time advancement (does not reset time).
	func pause() -> void:
		_running = false

	## Resume time advancement.
	func resume() -> void:
		_running = _time_scale > 0.0

	## Stop clock and remove it from the scene.
	func stop() -> void:
		_running = false
		queue_free()

	## Change advancement speed.
	func set_time_scale(scale: float) -> void:
		_time_scale = scale
		_running = scale > 0.0

	## Get current time (hours, 0-24).
	func get_time() -> float:
		return _time_hours

	## Jump to a specific time.
	func set_time(hours: float) -> void:
		_time_hours = fposmod(hours, 24.0)
		_apply()

	## True if sun is above the horizon.
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
