"""Agent communication — shared message board for multi-agent coordination.

Agents post messages (feature requests, bug reports, completions) and read
messages addressed to their role. Persistent JSON storage.
"""

import json
import time
from pathlib import Path

MSG_FILE = Path(__file__).parent.parent / "memory_store" / "agent_messages.json"
ROLES_FILE = Path(__file__).parent / "roles.json"


def _load_messages() -> list[dict]:
    if MSG_FILE.exists():
        try:
            return json.loads(MSG_FILE.read_text(encoding="utf-8"))
        except Exception:
            return []
    return []


def _save_messages(messages: list[dict]) -> None:
    MSG_FILE.parent.mkdir(parents=True, exist_ok=True)
    MSG_FILE.write_text(json.dumps(messages, indent=2), encoding="utf-8")


def _load_roles() -> dict:
    return json.loads(ROLES_FILE.read_text(encoding="utf-8"))


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register multi-agent communication tools on the MCP server."""

    @mcp.tool
    def agent_whoami(role: str) -> dict:
        """Get role definition — what you can/cannot modify, what tools to use.

        role: game_dev | engine_dev | qa | architect

        Call this first when assuming a role. It tells you your boundaries."""
        roles = _load_roles()
        if role not in roles["roles"]:
            return {"error": f"Unknown role: {role}. Available: {list(roles['roles'].keys())}"}
        return roles["roles"][role]

    @mcp.tool
    def agent_post(
        from_role: str,
        to_role: str,
        msg_type: str,
        title: str,
        body: str,
        priority: str = "normal",
        context: dict | None = None,
    ) -> dict:
        """Post a message to another agent role.

        from_role: your role (game_dev, engine_dev, qa, architect)
        to_role: recipient role
        msg_type: feature_request | bug_report | implementation_complete |
                  review_approved | review_rejected | question
        title: short summary
        body: detailed description
        priority: low | normal | high | critical
        context: optional dict with files, screenshots, metrics, etc.

        Examples:
          Game Dev → Engine Dev: feature_request "Need soft shadow support"
          QA → Game Dev: bug_report "Enemies don't take damage from shotgun"
          Engine Dev → Architect: implementation_complete "Added soft shadows in worktree"
          Architect → Engine Dev: review_approved "Merge soft shadows to dev"
        """
        messages = _load_messages()

        msg_id = f"msg_{int(time.time())}_{len(messages)}"
        message = {
            "id": msg_id,
            "from": from_role,
            "to": to_role,
            "type": msg_type,
            "title": title,
            "body": body,
            "priority": priority,
            "context": context or {},
            "status": "open",
            "created_at": time.time(),
            "responses": [],
        }
        messages.append(message)
        _save_messages(messages)

        return {"message_id": msg_id, "status": "posted", "to": to_role}

    @mcp.tool
    def agent_inbox(
        role: str,
        status: str | None = None,
        msg_type: str | None = None,
    ) -> dict:
        """Check messages addressed to your role.

        role: your role
        status: filter by open | in_progress | resolved | all (default: open)
        msg_type: filter by message type

        Returns unread/open messages for your role."""
        messages = _load_messages()
        status = status or "open"

        filtered = [m for m in messages if m["to"] == role]
        if status != "all":
            filtered = [m for m in filtered if m["status"] == status]
        if msg_type:
            filtered = [m for m in filtered if m["type"] == msg_type]

        # Sort by priority then time
        priority_order = {"critical": 0, "high": 1, "normal": 2, "low": 3}
        filtered.sort(key=lambda m: (priority_order.get(m["priority"], 2), -m["created_at"]))

        summaries = []
        for m in filtered:
            summaries.append({
                "id": m["id"],
                "from": m["from"],
                "type": m["type"],
                "title": m["title"],
                "priority": m["priority"],
                "status": m["status"],
                "age_hours": round((time.time() - m["created_at"]) / 3600, 1),
            })

        return {"role": role, "count": len(summaries), "messages": summaries}

    @mcp.tool
    def agent_respond(
        message_id: str,
        from_role: str,
        response: str,
        new_status: str | None = None,
    ) -> dict:
        """Respond to a message and optionally update its status.

        message_id: the message to respond to
        from_role: your role
        response: your response text
        new_status: open | in_progress | resolved (optional)

        Use this to:
          - Acknowledge a feature request (status: in_progress)
          - Provide a fix for a bug report (status: resolved)
          - Answer a question (status: resolved)
        """
        messages = _load_messages()

        for msg in messages:
            if msg["id"] == message_id:
                msg["responses"].append({
                    "from": from_role,
                    "response": response,
                    "timestamp": time.time(),
                })
                if new_status:
                    msg["status"] = new_status
                _save_messages(messages)
                return {
                    "message_id": message_id,
                    "status": msg["status"],
                    "response_count": len(msg["responses"]),
                }

        return {"error": f"Message not found: {message_id}"}

    @mcp.tool
    def agent_read(message_id: str) -> dict:
        """Read a specific message with all responses.

        Returns the full message including body, context, and all responses."""
        messages = _load_messages()
        for msg in messages:
            if msg["id"] == message_id:
                return msg
        return {"error": f"Message not found: {message_id}"}

    @mcp.tool
    def agent_feature_request(
        title: str,
        what: str,
        why: str,
        suggested_api: str | None = None,
        priority: str = "normal",
        example_usage: str | None = None,
    ) -> dict:
        """Shortcut: Game Dev requests a new engine feature from Engine Dev.

        title: feature name (e.g., 'soft shadows', 'cloth simulation')
        what: what the feature should do
        why: why gameplay needs it (the motivation)
        suggested_api: what the GDScript API should look like (optional)
        priority: low | normal | high | critical
        example_usage: GDScript example of how you'd use this feature (optional)

        This creates a structured feature request that Engine Dev can implement.
        Architect reviews and prioritizes."""
        messages = _load_messages()

        msg_id = f"feat_{int(time.time())}_{title.replace(' ', '_')[:20]}"
        message = {
            "id": msg_id,
            "from": "game_dev",
            "to": "engine_dev",
            "type": "feature_request",
            "title": title,
            "body": f"**What**: {what}\n\n**Why**: {why}",
            "priority": priority,
            "context": {
                "suggested_api": suggested_api,
                "example_usage": example_usage,
            },
            "status": "open",
            "created_at": time.time(),
            "responses": [],
        }
        messages.append(message)

        # Also notify architect for prioritization
        notify = {
            "id": f"notify_{msg_id}",
            "from": "system",
            "to": "architect",
            "type": "feature_request",
            "title": f"[Review] {title}",
            "body": f"Game Dev requested: {title}\n\n{what}\n\nPriority: {priority}",
            "priority": priority,
            "context": {"original_request": msg_id},
            "status": "open",
            "created_at": time.time(),
            "responses": [],
        }
        messages.append(notify)
        _save_messages(messages)

        return {
            "request_id": msg_id,
            "status": "posted",
            "notified": ["engine_dev", "architect"],
            "next": "Engine Dev will review. Architect will prioritize.",
        }

    @mcp.tool
    def agent_board_summary() -> dict:
        """Get a summary of the entire message board — all roles, all statuses.

        Useful for understanding current state of work across all agents."""
        messages = _load_messages()

        by_status = {}
        by_role = {}
        by_type = {}

        for msg in messages:
            s = msg["status"]
            by_status[s] = by_status.get(s, 0) + 1

            r = msg["to"]
            by_role[r] = by_role.get(r, 0) + 1

            t = msg["type"]
            by_type[t] = by_type.get(t, 0) + 1

        open_critical = [m for m in messages if m["status"] == "open" and m["priority"] == "critical"]
        open_high = [m for m in messages if m["status"] == "open" and m["priority"] == "high"]

        return {
            "total_messages": len(messages),
            "by_status": by_status,
            "by_recipient": by_role,
            "by_type": by_type,
            "urgent": [{"id": m["id"], "title": m["title"], "to": m["to"]} for m in open_critical + open_high],
        }
