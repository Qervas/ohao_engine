class_name OhaoGod
## OhaoGod - God-mode helper for MCP AI agents.
##
## Provides high-level perception and camera control for the MCP server.
## Used by OhaoServer's /god/* endpoints.


## Position camera to look at target from spherical coordinates.
## elevation_deg: angle above horizon (0 = level, 90 = top-down)
## azimuth_deg: horizontal angle (0 = +X, 90 = +Z)
static func look_at(vp: OhaoViewport, target: Vector3, distance: float = 10.0,
		elevation_deg: float = 30.0, azimuth_deg: float = 0.0) -> void:
	var elev_rad := deg_to_rad(clampf(elevation_deg, -89.0, 89.0))
	var azim_rad := deg_to_rad(azimuth_deg)

	var cam_pos := Vector3(
		target.x + distance * cos(elev_rad) * cos(azim_rad),
		target.y + distance * sin(elev_rad),
		target.z + distance * cos(elev_rad) * sin(azim_rad),
	)

	vp.set_camera_position(cam_pos)
	# Compute pitch/yaw to look at target
	var dir := (target - cam_pos).normalized()
	var pitch_deg := rad_to_deg(asin(dir.y))
	var yaw_deg := rad_to_deg(atan2(dir.x, -dir.z))
	vp.set_camera_rotation_deg(pitch_deg, yaw_deg)


## Save full snapshot (screenshot + state JSON) to output_dir.
## Returns {"screenshot": path, "state": path, "actor_count": N}
static func snapshot(vp: OhaoViewport, output_dir: String) -> Dictionary:
	# Ensure directory exists
	DirAccess.make_dir_recursive_absolute(output_dir)

	var screenshot_path := output_dir.path_join("screenshot.png")
	var state_path := output_dir.path_join("state.json")

	# Save screenshot
	var ok := vp.save_screenshot(screenshot_path)

	# Save state JSON
	var state := vp.get_scene_state()
	var json_str := JSON.stringify(state, "  ")
	var file := FileAccess.open(state_path, FileAccess.WRITE)
	if file:
		file.store_string(json_str)
		file.close()

	return {
		"screenshot": screenshot_path if ok else "",
		"state": state_path,
		"actor_count": state.get("actors", []).size(),
	}


## Multi-angle orbit capture around a target point.
## Returns array of screenshot file paths.
static func orbit_capture(vp: OhaoViewport, target: Vector3, distance: float,
		count: int, output_dir: String) -> Array:
	DirAccess.make_dir_recursive_absolute(output_dir)
	var paths: Array = []
	count = clampi(count, 1, 36)

	for i in count:
		var azimuth := (360.0 / count) * i
		look_at(vp, target, distance, 30.0, azimuth)
		# Wait one frame for render to update
		# Note: caller must use await if they want frame-accurate captures
		var path := output_dir.path_join("orbit_%02d.png" % i)
		vp.save_screenshot(path)
		paths.append(path)

	return paths


## List all actors with summary info from scene state.
static func list_actors(vp: OhaoViewport) -> Array:
	var state := vp.get_scene_state()
	var actors: Array = state.get("actors", [])
	var summaries: Array = []
	for actor in actors:
		summaries.append({
			"name": actor.get("name", ""),
			"type": actor.get("mesh", {}).get("primitive", "unknown"),
			"has_physics": actor.has("physics"),
			"has_light": actor.has("light"),
		})
	return summaries
