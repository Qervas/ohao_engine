"""Event subscription system — real-time engine observation for AI agents.

AI agents subscribe to engine events (collisions, lightning, actor changes, etc.)
and poll for accumulated events. The GDScript side accumulates events in a ring
buffer; this module provides the MCP tool interface.
"""


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register event subscription tools on the MCP server."""

    @mcp.tool
    def events_subscribe(event_types: list[str] | None = None) -> dict:
        """Subscribe to engine events. Returns a subscription ID.

        event_types: list of event types to subscribe to, or None for all.
        Available types: collision, collision_end, lightning, actor_selected,
        actor_added, actor_removed, performance_alert, physics_step,
        shader_reloaded

        Call events_poll(subscription_id) to retrieve accumulated events."""
        if not bridge.is_connected():
            return {"error": "Engine not connected. Start Godot with OhaoViewport first."}
        body = {"event_types": event_types or ["all"]}
        return bridge.post("/events/subscribe", body)

    @mcp.tool
    def events_poll(subscription_id: str | None = None) -> dict:
        """Poll for new events since last poll. Returns list of events.

        subscription_id: from events_subscribe(). If None, returns all recent events.
        Each event: {type, timestamp, data}"""
        if not bridge.is_connected():
            return {"error": "Engine not connected. Start Godot with OhaoViewport first."}
        path = "/events/poll"
        if subscription_id:
            path += f"?sub={subscription_id}"
        return bridge.get(path)

    @mcp.tool
    def events_unsubscribe(subscription_id: str) -> dict:
        """Unsubscribe from events."""
        if not bridge.is_connected():
            return {"error": "Engine not connected. Start Godot with OhaoViewport first."}
        return bridge.post("/events/unsubscribe", {"subscription_id": subscription_id})

    @mcp.tool
    def events_history(count: int = 50, event_type: str | None = None) -> dict:
        """Get recent event history (last N events, optionally filtered by type).

        count: max events to return (1-200)
        event_type: filter to specific type, or None for all"""
        if not bridge.is_connected():
            return {"error": "Engine not connected. Start Godot with OhaoViewport first."}
        path = f"/events/history?count={count}"
        if event_type:
            path += f"&type={event_type}"
        return bridge.get(path)
