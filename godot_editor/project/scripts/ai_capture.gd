extends Control
## AI Capture — Headless scene capture for autonomous agent workflows.
##
## Usage (from godot_editor/project/):
##   godot scenes/ai_capture.tscn -- --output C:/tmp/capture
##   godot scenes/ai_capture.tscn -- --output C:/tmp/capture --scene '{"template":"arena"}'
##   godot scenes/ai_capture.tscn -- --output C:/tmp/capture --camera "5,8,5" --look "0,0,0"
##
## Writes state.json + screenshot.png to --output dir, then exits.

var _frames_waited := 0
const WAIT_FRAMES := 5  # Wait for renderer to stabilize

# Parsed CLI args
var _output_dir := ""
var _scene_json := ""
var _camera_pos := Vector3.INF
var _look_target := Vector3.INF


func _ready() -> void:
	_parse_args()
	if _output_dir.is_empty():
		push_error("[AICapture] --output <dir> is required")
		get_tree().quit(1)
		return

	# Build scene if provided
	if not _scene_json.is_empty():
		var parser := JSON.new()
		if parser.parse(_scene_json) == OK:
			var vp := _get_vp()
			if vp:
				OhaoSceneBuilder.build(vp, parser.get_data())
		else:
			push_warning("[AICapture] Failed to parse --scene JSON: %s" % parser.get_error_message())


func _process(_delta: float) -> void:
	_frames_waited += 1
	if _frames_waited < WAIT_FRAMES:
		return

	# Only run capture once
	set_process(false)

	var vp := _get_vp()
	if not vp:
		push_error("[AICapture] No OhaoViewport found")
		get_tree().quit(1)
		return

	# Set camera if requested
	if _camera_pos != Vector3.INF:
		vp.set_camera_position(_camera_pos)
	if _look_target != Vector3.INF:
		OhaoGod.look_at(vp, _look_target, 10.0, 30.0, 0.0)

	# Capture
	var result := OhaoGod.snapshot(vp, _output_dir)
	print("[AICapture] Saved to: %s (%d actors)" % [_output_dir, result.get("actor_count", 0)])
	get_tree().quit(0)


func _get_vp() -> OhaoViewport:
	return Ohao.viewport(self)


func _parse_args() -> void:
	var args := OS.get_cmdline_user_args()
	var i := 0
	while i < args.size():
		match args[i]:
			"--output":
				i += 1
				if i < args.size():
					_output_dir = args[i]
			"--scene":
				i += 1
				if i < args.size():
					_scene_json = args[i]
			"--camera":
				i += 1
				if i < args.size():
					_camera_pos = _parse_v3(args[i])
			"--look":
				i += 1
				if i < args.size():
					_look_target = _parse_v3(args[i])
		i += 1


func _parse_v3(s: String) -> Vector3:
	var parts := s.split(",")
	if parts.size() >= 3:
		return Vector3(float(parts[0]), float(parts[1]), float(parts[2]))
	return Vector3.INF
