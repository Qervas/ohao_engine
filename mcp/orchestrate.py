"""Workflow orchestration — multi-step task coordination for AI agents.

Provides predefined workflows (new render pass, shader experiment, perf audit)
and tracks progress across steps. Agents follow structured checklists with
state that persists across sessions.
"""

import json
import time
from pathlib import Path

WORKFLOW_DIR = Path(__file__).parent / "workflows"
STATE_DIR = Path(__file__).parent / "memory_store" / "active_workflows"

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _ensure_dirs():
    STATE_DIR.mkdir(parents=True, exist_ok=True)


def _load_definition(name: str) -> dict | None:
    """Load a workflow definition JSON."""
    path = WORKFLOW_DIR / f"{name}.json"
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def _generate_id(name: str) -> str:
    return f"{name}_{int(time.time())}"


def _load_state(workflow_id: str) -> dict | None:
    _ensure_dirs()
    path = STATE_DIR / f"{workflow_id}.json"
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def _save_state(workflow_id: str, state: dict):
    _ensure_dirs()
    path = STATE_DIR / f"{workflow_id}.json"
    path.write_text(json.dumps(state, indent=2), encoding="utf-8")


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register orchestration tools on the MCP server."""

    @mcp.tool
    def workflow_list() -> dict:
        """List all available workflow templates with descriptions."""
        workflows = []
        if WORKFLOW_DIR.exists():
            for f in sorted(WORKFLOW_DIR.glob("*.json")):
                try:
                    data = json.loads(f.read_text(encoding="utf-8"))
                    workflows.append({
                        "name": f.stem,
                        "description": data.get("description", ""),
                        "step_count": len(data.get("steps", [])),
                    })
                except Exception:
                    pass

        # Also list active workflows
        active = []
        _ensure_dirs()
        for f in STATE_DIR.glob("*.json"):
            try:
                state = json.loads(f.read_text(encoding="utf-8"))
                active.append({
                    "id": f.stem,
                    "workflow": state.get("workflow", ""),
                    "current_step": state.get("current_step", 0),
                    "total_steps": state.get("total_steps", 0),
                    "status": state.get("status", "unknown"),
                })
            except Exception:
                pass

        return {
            "available": workflows,
            "active": active,
        }

    @mcp.tool
    def workflow_describe(name: str) -> dict:
        """Get detailed description of a workflow: all steps, required tools,
        and expected inputs/outputs.

        name: workflow name (e.g., 'new_render_pass', 'shader_experiment')"""
        definition = _load_definition(name)
        if not definition:
            available = [f.stem for f in WORKFLOW_DIR.glob("*.json")] if WORKFLOW_DIR.exists() else []
            return {"error": f"Workflow '{name}' not found. Available: {available}"}
        return definition

    @mcp.tool
    def workflow_start(name: str, params: dict | None = None) -> dict:
        """Start a new workflow instance. Returns the workflow ID and first step.

        name: workflow template name
        params: initial parameters for the workflow (e.g., {pass_name: 'fog'})"""
        definition = _load_definition(name)
        if not definition:
            return {"error": f"Workflow '{name}' not found"}

        workflow_id = _generate_id(name)
        state = {
            "workflow": name,
            "params": params or {},
            "current_step": 1,
            "total_steps": len(definition["steps"]),
            "status": "in_progress",
            "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "step_results": {},
        }
        _save_state(workflow_id, state)

        first_step = definition["steps"][0] if definition["steps"] else None
        return {
            "workflow_id": workflow_id,
            "workflow": name,
            "status": "in_progress",
            "current_step": 1,
            "total_steps": state["total_steps"],
            "next_step": first_step,
        }

    @mcp.tool
    def workflow_advance(workflow_id: str, step_result: str | None = None) -> dict:
        """Mark the current step as complete and advance to the next step.

        workflow_id: the active workflow ID
        step_result: summary of what was done in this step"""
        state = _load_state(workflow_id)
        if not state:
            return {"error": f"Workflow '{workflow_id}' not found"}

        definition = _load_definition(state["workflow"])
        if not definition:
            return {"error": f"Workflow definition '{state['workflow']}' not found"}

        current = state["current_step"]
        state["step_results"][str(current)] = {
            "result": step_result or "completed",
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

        if current >= state["total_steps"]:
            state["status"] = "completed"
            state["completed"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            _save_state(workflow_id, state)
            return {
                "workflow_id": workflow_id,
                "status": "completed",
                "all_steps_done": True,
            }

        state["current_step"] = current + 1
        _save_state(workflow_id, state)

        next_step = definition["steps"][current]  # 0-indexed, current is already incremented
        return {
            "workflow_id": workflow_id,
            "status": "in_progress",
            "completed_step": current,
            "current_step": current + 1,
            "total_steps": state["total_steps"],
            "next_step": next_step,
        }

    @mcp.tool
    def workflow_status(workflow_id: str) -> dict:
        """Get current status of an active workflow.

        workflow_id: the workflow ID returned by workflow_start"""
        state = _load_state(workflow_id)
        if not state:
            return {"error": f"Workflow '{workflow_id}' not found"}

        definition = _load_definition(state["workflow"])
        steps = definition.get("steps", []) if definition else []

        current_step = None
        if state["current_step"] <= len(steps):
            current_step = steps[state["current_step"] - 1]

        return {
            "workflow_id": workflow_id,
            "workflow": state["workflow"],
            "status": state["status"],
            "current_step": state["current_step"],
            "total_steps": state["total_steps"],
            "params": state.get("params", {}),
            "step_results": state.get("step_results", {}),
            "next_step": current_step,
            "started": state.get("started", ""),
        }
