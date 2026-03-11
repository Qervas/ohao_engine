"""
OHAO Engine MCP Server — AI God-mode interface.

Exposes 60+ tools organized by domain: perception, scene, camera, effects,
physics, terrain/water, audio, control templates, introspection, memory,
self-test, code generation, hot-reload, and workflow orchestration.

Any MCP-compatible AI agent can connect via stdio or HTTP.

Usage:
    python mcp/ohao_server.py          # stdio (Claude Code reads .mcp.json)
    python mcp/ohao_server.py --http   # Streamable HTTP on port 8756
"""

import base64
import sys

from mcp import types
from fastmcp import FastMCP
from fastmcp.tools import ToolResult

from engine_bridge import bridge

mcp = FastMCP(
    "ohao-engine",
    instructions=(
        "OHAO Game Engine — AI-native game engine with self-development capabilities. "
        "You can observe the full scene state, capture screenshots, build scenes, "
        "configure effects, control physics, introspect the codebase and render pipeline, "
        "generate new render passes from templates, hot-reload shaders at runtime, "
        "store persistent knowledge across sessions, run build validation and tests, "
        "and follow structured workflows for complex multi-step tasks. "
        "Some tools work offline (introspection, memory, generation). "
        "Runtime tools require the engine running in Godot."
    ),
)


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _vec3(v: list[float] | None) -> list[float]:
    """Ensure a 3-element float list."""
    if v is None:
        return [0.0, 0.0, 0.0]
    return [float(x) for x in v[:3]]


def _check_engine() -> dict | None:
    """Return error dict if engine not connected, else None."""
    if not bridge.is_connected():
        return {"error": "Engine not connected. Start Godot with OhaoViewport first."}
    return None


# ─────────────────────────────────────────────────────────────────────────────
# Perception (7 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def get_scene_state() -> dict:
    """Get complete scene graph: all actors with transforms, materials, physics,
    plus camera state, active effects, physics config, and environment settings."""
    err = _check_engine()
    if err:
        return err
    return bridge.get("/god/state")


@mcp.tool
def capture_screenshot() -> ToolResult:
    """Capture current viewport as PNG image. Returns the rendered frame
    exactly as it appears — with all post-processing, lighting, and effects."""
    err = _check_engine()
    if err:
        return ToolResult(content=[types.TextContent(type="text", text=str(err))])
    b64, w, h = bridge.capture_image_b64()
    return ToolResult(content=[
        types.ImageContent(type="image", data=b64, mimeType="image/png"),
        types.TextContent(type="text", text=f"Screenshot captured ({w}x{h})"),
    ])


@mcp.tool
def get_render_stats() -> dict:
    """Get rendering performance stats: resolution, render mode, enabled effects."""
    err = _check_engine()
    if err:
        return err
    state = bridge.get("/god/state")
    return state.get("render_stats", {})


@mcp.tool
def cast_ray(
    origin: list[float],
    direction: list[float],
    max_distance: float = 1000.0,
    layer_mask: int = 0xFFFF,
) -> dict:
    """Cast a ray and return hit info: position, normal, actor name, distance."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/physics/raycast", {
        "origin": _vec3(origin),
        "direction": _vec3(direction),
        "max_dist": max_distance,
        "layer_mask": layer_mask,
    })


@mcp.tool
def overlap_sphere(
    center: list[float],
    radius: float,
    layer_mask: int = 0xFFFF,
) -> dict:
    """Find all physics bodies within a sphere. Returns body handles and names."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/physics/overlap_sphere", {
        "center": _vec3(center),
        "radius": radius,
        "layer_mask": layer_mask,
    })


@mcp.tool
def overlap_box(
    center: list[float],
    half_extents: list[float],
    rotation_deg: list[float] | None = None,
    layer_mask: int = 0xFFFF,
) -> dict:
    """Find all physics bodies within a box. Returns body handles and names."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/physics/overlap_box", {
        "center": _vec3(center),
        "half_extents": _vec3(half_extents),
        "rotation_deg": _vec3(rotation_deg),
        "layer_mask": layer_mask,
    })


@mcp.tool
def snapshot(output_dir: str) -> dict:
    """Full god-mode snapshot: saves screenshot.png + state.json to output_dir.
    Returns file paths and summary stats."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/god/snapshot", {"output_dir": output_dir})


# ─────────────────────────────────────────────────────────────────────────────
# Scene Management (8 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def build_scene(config: dict) -> dict:
    """Build a complete scene from a configuration dict. Supports templates
    (arena, corridor, outdoor), rendering presets (horror, cyberpunk, bright),
    object lists, light lists, and control templates (fps, orbit, rts).

    Example config:
    {
        "template": "arena",
        "rendering": "cyberpunk",
        "objects": [{"type": "cube", "name": "Box", "pos": [0,1,0], "color": [1,0,0]}],
        "lights": [{"type": "point", "name": "L1", "pos": [3,4,0], "intensity": 2.0}],
        "control": "fps"
    }"""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/scene/build", config)


@mcp.tool
def add_actor(
    type: str,
    name: str,
    position: list[float],
    rotation: list[float] | None = None,
    scale: list[float] | None = None,
    color: list[float] | None = None,
) -> dict:
    """Add a mesh actor to the scene. type: cube, sphere, cylinder, or plane."""
    err = _check_engine()
    if err:
        return err
    body = {
        "type": type,
        "name": name,
        "pos": _vec3(position),
        "rot": _vec3(rotation),
        "scale": scale or [1, 1, 1],
        "color": color or [0.8, 0.8, 0.8, 1.0],
    }
    return bridge.post("/scene/actor", body)


@mcp.tool
def remove_actor(name: str) -> dict:
    """Remove an actor from the scene by name."""
    err = _check_engine()
    if err:
        return err
    return bridge.delete(f"/scene/actor?name={name}")


@mcp.tool
def clear_scene() -> dict:
    """Remove all actors from the scene."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/scene/clear")


@mcp.tool
def set_actor_transform(
    name: str,
    position: list[float] | None = None,
    rotation_deg: list[float] | None = None,
    scale: list[float] | None = None,
) -> dict:
    """Set actor position, rotation, and/or scale. Only provided fields are changed."""
    err = _check_engine()
    if err:
        return err
    body: dict = {"name": name}
    if position is not None:
        body["position"] = _vec3(position)
    if rotation_deg is not None:
        body["rotation_deg"] = _vec3(rotation_deg)
    if scale is not None:
        body["scale"] = [float(x) for x in scale[:3]]
    return bridge.post("/scene/actor/transform", body)


@mcp.tool
def set_actor_material(
    name: str,
    color: list[float] | None = None,
    metallic: float | None = None,
    roughness: float | None = None,
    texture_path: str | None = None,
) -> dict:
    """Set actor material properties: color, metallic, roughness, or texture."""
    err = _check_engine()
    if err:
        return err
    body: dict = {"name": name}
    if color is not None:
        body["color"] = [float(x) for x in color]
    if metallic is not None:
        body["metallic"] = metallic
    if roughness is not None:
        body["roughness"] = roughness
    if texture_path is not None:
        body["texture_path"] = texture_path
    return bridge.post("/scene/actor/material", body)


@mcp.tool
def import_model(path: str) -> dict:
    """Import a 3D model file (glTF/GLB) into the scene."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/scene/import", {"path": path})


@mcp.tool
def finish_sync() -> dict:
    """Upload pending scene changes to GPU. Call after batch modifications."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/scene/finish_sync")


# ─────────────────────────────────────────────────────────────────────────────
# Camera (4 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def set_camera(
    position: list[float],
    pitch_deg: float | None = None,
    yaw_deg: float | None = None,
) -> dict:
    """Set camera position and optionally rotation (pitch/yaw in degrees)."""
    err = _check_engine()
    if err:
        return err
    body: dict = {"position": _vec3(position)}
    if pitch_deg is not None or yaw_deg is not None:
        body["rotation_deg"] = [pitch_deg or 0.0, yaw_deg or 0.0, 0.0]
    return bridge.post("/camera", body)


@mcp.tool
def look_at(
    target: list[float],
    distance: float = 10.0,
    elevation_deg: float = 30.0,
    azimuth_deg: float = 0.0,
) -> dict:
    """Position camera to look at a target from spherical coordinates.
    elevation_deg: angle above horizon. azimuth_deg: horizontal angle."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/god/look_at", {
        "target": _vec3(target),
        "distance": distance,
        "elevation": elevation_deg,
        "azimuth": azimuth_deg,
    })


@mcp.tool
def focus_on_scene() -> dict:
    """Auto-frame camera to see the entire scene."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/camera", {"focus": True})


@mcp.tool
def orbit_capture(
    target: list[float],
    distance: float = 10.0,
    count: int = 4,
    output_dir: str | None = None,
) -> ToolResult:
    """Capture the scene from multiple angles around a target point.
    If output_dir is given, saves PNGs to disk and returns paths.
    Otherwise returns inline images."""
    err = _check_engine()
    if err:
        return ToolResult(content=[types.TextContent(type="text", text=str(err))])

    if output_dir:
        result = bridge.post("/god/orbit_capture", {
            "target": _vec3(target),
            "distance": distance,
            "count": count,
            "output_dir": output_dir,
        })
        return ToolResult(content=[
            types.TextContent(type="text", text=f"Captured {result.get('count', 0)} images to {output_dir}")
        ])

    # Inline capture: move camera to each position and capture
    import math
    contents = []
    for i in range(min(count, 8)):
        azimuth = (360.0 / count) * i
        bridge.post("/god/look_at", {
            "target": _vec3(target),
            "distance": distance,
            "elevation": 30.0,
            "azimuth": azimuth,
        })
        b64, w, h = bridge.capture_image_b64()
        contents.append(types.TextContent(type="text", text=f"Angle {azimuth:.0f}° ({w}x{h}):"))
        contents.append(types.ImageContent(type="image", data=b64, mimeType="image/png"))

    return ToolResult(content=contents)


# ─────────────────────────────────────────────────────────────────────────────
# Effects & Environment (5 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def set_effect(name: str, enabled: bool = True, params: dict | None = None) -> dict:
    """Enable/disable and configure a post-processing effect.
    Effects: bloom, ssao, ssgi, ssr, volumetrics, motion_blur, dof, taa, tonemapping.

    Example params per effect:
    - bloom: {bloom_threshold: 0.4, bloom_intensity: 1.2}
    - ssao: {ssao_radius: 0.5, ssao_intensity: 1.0}
    - dof: {dof_focus_distance: 5.0, dof_aperture: 0.1, dof_max_blur: 1.0}"""
    err = _check_engine()
    if err:
        return err
    body = {name: {"enabled": enabled}}
    if params:
        body[name].update(params)
    return bridge.post("/effects", body)


@mcp.tool
def apply_rendering_preset(preset: str) -> dict:
    """Apply a rendering preset: horror, cyberpunk, bright, cinematic,
    fps_action, or minimal. Sets multiple effects at once."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/effects/preset", {"rendering": preset})


@mcp.tool
def set_time_of_day(hours: float) -> dict:
    """Set time of day (0-24). Controls sun position, sky color, stars, and moon.
    Examples: 6.0=sunrise, 12.0=noon, 18.0=sunset, 0.0=midnight."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/environment/time", {"hours": hours})


@mcp.tool
def set_weather(config: dict) -> dict:
    """Configure weather effects. Available keys:
    rain_enabled (bool), rain_intensity (0-1), snow_enabled (bool),
    snow_intensity (0-1), lightning_enabled (bool), cloud_coverage (0-1),
    cloud_enabled (bool), fog_color ([r,g,b])."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/environment/weather", config)


@mcp.tool
def set_environment(config: dict) -> dict:
    """Configure environment forces. Keys:
    wind: {direction: [x,y,z], strength: float, turbulence: float},
    water: {level: float, density: float, drag: float},
    gravity: [x,y,z]."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/environment/forces", config)


# ─────────────────────────────────────────────────────────────────────────────
# Physics (6 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def physics_control(action: str) -> dict:
    """Control physics simulation. action: play, pause, step, or stop."""
    err = _check_engine()
    if err:
        return err
    valid = {"play", "pause", "step", "stop"}
    if action not in valid:
        return {"error": f"Invalid action '{action}'. Use: {', '.join(valid)}"}
    return bridge.post(f"/physics/{action}")


@mcp.tool
def set_gravity(gravity: list[float]) -> dict:
    """Set world gravity vector. Default is [0, -9.81, 0]."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/physics/gravity", {"gravity": _vec3(gravity)})


@mcp.tool
def set_actor_physics(
    name: str,
    body_type: str | None = None,
    mass: float | None = None,
    friction: float | None = None,
    restitution: float | None = None,
    gravity_enabled: bool | None = None,
) -> dict:
    """Configure actor physics properties.
    body_type: dynamic, static, or kinematic."""
    err = _check_engine()
    if err:
        return err
    body: dict = {"name": name}
    if body_type is not None:
        body["body_type"] = body_type
    if mass is not None:
        body["mass"] = mass
    if friction is not None:
        body["friction"] = friction
    if restitution is not None:
        body["restitution"] = restitution
    if gravity_enabled is not None:
        body["gravity_enabled"] = gravity_enabled
    return bridge.post("/physics/actor", body)


@mcp.tool
def apply_force(
    name: str,
    force: list[float],
    relative_position: list[float] | None = None,
) -> dict:
    """Apply a continuous force to an actor at a relative position."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/physics/force", {
        "name": name,
        "force": _vec3(force),
        "relative_position": _vec3(relative_position),
    })


@mcp.tool
def apply_impulse(
    name: str,
    impulse: list[float],
    relative_position: list[float] | None = None,
) -> dict:
    """Apply an instant impulse to an actor (one-time velocity change)."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/physics/impulse", {
        "name": name,
        "impulse": _vec3(impulse),
        "relative_position": _vec3(relative_position),
    })


@mcp.tool
def create_constraint(
    type: str,
    body1: str,
    body2: str,
    params: dict | None = None,
) -> dict:
    """Create a physics constraint between two bodies.
    type: fixed, hinge, slider, point, distance, or cone.
    params vary by type (e.g., hinge: {axis, limit_min, limit_max})."""
    err = _check_engine()
    if err:
        return err
    body = {
        "type": type,
        "body1": body1,
        "body2": body2,
    }
    if params:
        body["params"] = params
    return bridge.post("/physics/constraint", body)


# ─────────────────────────────────────────────────────────────────────────────
# Terrain & Water (4 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def set_terrain(config: dict) -> dict:
    """Configure terrain. Keys:
    enabled (bool), type (flat/hills/mountains/canyon/desert/arctic),
    height_scale (float), size (float), frequency (float), octaves (int)."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/terrain/config", config)


@mcp.tool
def generate_terrain() -> dict:
    """Generate procedural terrain based on current terrain config."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/terrain/generate")


@mcp.tool
def set_water(config: dict) -> dict:
    """Configure water. Keys:
    enabled (bool), level (float), size (float),
    wave_mode (gerstner/fft), foam_intensity (float),
    wave_amplitude (float), fft_wind_speed (float)."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/water/config", config)


@mcp.tool
def set_foliage(config: dict) -> dict:
    """Configure foliage/grass. Keys:
    enabled (bool), grass_texture (path), cull_distance (float),
    clusters (array of cluster dicts)."""
    err = _check_engine()
    if err:
        return err
    return bridge.post("/foliage/config", config)


# ─────────────────────────────────────────────────────────────────────────────
# Audio (2 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def play_sound(
    path: str,
    position: list[float] | None = None,
    category: str = "sfx",
    loop: bool = False,
    volume: float = 1.0,
) -> dict:
    """Play a sound. If position is given, plays as 3D spatial audio.
    category: sfx, music, or ambient. Path relative to res://sounds/."""
    err = _check_engine()
    if err:
        return err
    body: dict = {
        "path": path,
        "category": category,
        "loop": loop,
        "volume": volume,
    }
    if position is not None:
        body["position"] = _vec3(position)
    return bridge.post("/audio/play", body)


@mcp.tool
def stop_sound(handle: int | None = None, category: str | None = None) -> dict:
    """Stop a sound by handle, or all sounds in a category, or all sounds
    if neither is specified."""
    err = _check_engine()
    if err:
        return err
    body: dict = {}
    if handle is not None:
        body["handle"] = handle
    if category is not None:
        body["category"] = category
    return bridge.post("/audio/stop", body)


# ─────────────────────────────────────────────────────────────────────────────
# Control Templates (2 tools)
# ─────────────────────────────────────────────────────────────────────────────

@mcp.tool
def apply_control_template(template: str, params: dict | None = None) -> dict:
    """Apply a control template: fps, orbit, rts, physics_sandbox, cinematic,
    vehicle, top_down, or puzzle. Optional param overrides (e.g., move_speed)."""
    err = _check_engine()
    if err:
        return err
    body: dict = {"template": template}
    if params:
        body["params"] = params
    return bridge.post("/control/apply", body)


@mcp.tool
def set_input_mode(mode: str) -> dict:
    """Set input mode: 'editor' (C++ camera controls) or 'game' (GDScript drives)."""
    err = _check_engine()
    if err:
        return err
    mode_int = 1 if mode.lower() == "game" else 0
    return bridge.post("/camera", {"mode": mode_int})


# ─────────────────────────────────────────────────────────────────────────────
# Self-development modules (introspect, remember, test, generate, reload, orchestrate)
# ─────────────────────────────────────────────────────────────────────────────

import introspect
import memory
import selftest
import generate
import hotreload
import orchestrate
import events
import visual_test
import ai_player
import experiment
from agents import comms as agent_comms

introspect.register(mcp, bridge)
memory.register(mcp, bridge)
selftest.register(mcp, bridge)
generate.register(mcp, bridge)
hotreload.register(mcp, bridge)
orchestrate.register(mcp, bridge)
events.register(mcp, bridge)
visual_test.register(mcp, bridge)
ai_player.register(mcp, bridge)
experiment.register(mcp, bridge)
agent_comms.register(mcp, bridge)

# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if "--http" in sys.argv:
        mcp.run(transport="streamable-http", host="0.0.0.0", port=8756)
    else:
        mcp.run()  # stdio (default for Claude Code)
