"""AI Player — autonomous gameplay without human in the loop.

AI agents inject game inputs, step the simulation, observe results,
and iterate. The core primitive is step_and_observe: one atomic
action-perception cycle.

GDScript convention: game managers in "game_manager" group implement
get_game_state() → Dictionary. Player nodes in "player" group implement
get_game_state() for health/ammo/etc.
"""

import base64


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register AI player tools on the MCP server."""

    @mcp.tool
    def game_action(
        actions: list[str] | None = None,
        release: list[str] | None = None,
        mouse_delta: list[float] | None = None,
        look_at_position: list[float] | None = None,
    ) -> dict:
        """Inject game inputs — press actions, release actions, move mouse.

        actions: list of Godot input actions to press (e.g., ['move_forward', 'jump'])
        release: list of actions to release
        mouse_delta: [dx, dy] pixels for mouse look (FPS camera)
        look_at_position: [x, y, z] world position to aim camera at (easier than mouse_delta)

        Common actions: move_forward, move_back, move_left, move_right, jump, shoot,
        reload, interact, weapon_1, weapon_2, weapon_3, crouch, sprint

        Call this to press/release inputs. They stay pressed until released."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}
        body = {}
        if actions:
            body["actions"] = actions
        if release:
            body["release"] = release
        if mouse_delta:
            body["mouse_delta"] = mouse_delta
        if look_at_position:
            body["look_at_position"] = look_at_position
        return bridge.post("/god/play", body)

    @mcp.tool
    def get_game_state() -> dict:
        """Get current game state: player health/ammo/score, enemy positions, physics state.

        Returns data from game_manager.get_game_state() (if implemented),
        plus engine info (fps, physics), camera position, and enemy list.

        Convention: game scripts in 'game_manager' group implement get_game_state().
        Player scripts in 'player' group implement get_game_state() for health/ammo."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}
        return bridge.get("/god/game_state")

    @mcp.tool
    def step_and_observe(
        actions: list[str] | None = None,
        release: list[str] | None = None,
        mouse_delta: list[float] | None = None,
        look_at_position: list[float] | None = None,
        steps: int = 1,
        capture: bool = False,
    ) -> dict:
        """Atomic play cycle: inject inputs → step physics → return observation.

        This is THE fundamental AI gameplay primitive. One call = one game tick.

        actions: input actions to press this tick
        release: input actions to release this tick
        mouse_delta: mouse look movement [dx, dy]
        look_at_position: aim camera at world position [x,y,z]
        steps: number of physics frames to advance (1-60)
        capture: if True, includes base64 PNG screenshot (expensive, use sparingly)

        Returns: {game_state, events, stepped, [image_base64 if capture=True]}

        Usage pattern for autonomous play:
          1. step_and_observe(actions=['move_forward'], steps=5) — walk forward 5 ticks
          2. Check game_state.enemies — see who's nearby
          3. step_and_observe(look_at_position=enemy_pos, actions=['shoot']) — aim and fire
          4. Repeat

        Use capture=True every ~30 steps for visual verification, not every step."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}
        body = {"steps": steps, "capture": capture}
        if actions:
            body["actions"] = actions
        if release:
            body["release"] = release
        if mouse_delta:
            body["mouse_delta"] = mouse_delta
        if look_at_position:
            body["look_at_position"] = look_at_position
        return bridge.post("/god/step_and_observe", body)

    @mcp.tool
    def release_all_actions() -> dict:
        """Release all currently pressed input actions.

        Call this to reset input state — e.g., before starting a new play sequence
        or when switching from one behavior to another."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}
        # Release common actions
        common_actions = [
            "move_forward", "move_back", "move_left", "move_right",
            "jump", "shoot", "reload", "interact", "crouch", "sprint",
        ]
        return bridge.post("/god/play", {"release": common_actions})
