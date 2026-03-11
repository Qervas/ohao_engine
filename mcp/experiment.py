"""Experiment system — AI invents, tests, evaluates, and evolves.

Tracks experiment lifecycle: propose → prototype → observe → evaluate → keep/discard.
Persistent log at mcp/memory_store/experiments.json so the AI never repeats failures
or forgets successes.
"""

import json
import time
from pathlib import Path

EXPERIMENT_LOG = Path(__file__).parent / "memory_store" / "experiments.json"


def _load_experiments() -> list[dict]:
    if EXPERIMENT_LOG.exists():
        try:
            return json.loads(EXPERIMENT_LOG.read_text(encoding="utf-8"))
        except Exception:
            return []
    return []


def _save_experiments(experiments: list[dict]) -> None:
    EXPERIMENT_LOG.parent.mkdir(parents=True, exist_ok=True)
    EXPERIMENT_LOG.write_text(
        json.dumps(experiments, indent=2), encoding="utf-8"
    )


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register experiment tracking tools on the MCP server."""

    @mcp.tool
    def experiment_propose(
        name: str,
        hypothesis: str,
        type: str = "mechanic",
        building_blocks: list[str] | None = None,
    ) -> dict:
        """Propose a new experiment. Returns an experiment ID.

        name: short name (e.g., 'gravity_flip', 'audio_bloom')
        hypothesis: what you expect to happen (e.g., 'reversing gravity every 10s creates fun platforming')
        type: shader | mechanic | visual | physics | audio | combination
        building_blocks: which engine primitives you'll use (e.g., ['force_volume', 'timer', 'particles'])

        This logs the experiment so you can track results later."""
        experiments = _load_experiments()

        exp_id = f"exp_{int(time.time())}_{name}"
        experiment = {
            "id": exp_id,
            "name": name,
            "hypothesis": hypothesis,
            "type": type,
            "building_blocks": building_blocks or [],
            "status": "proposed",
            "proposed_at": time.time(),
            "iterations": [],
            "result": None,
            "files_created": [],
        }
        experiments.append(experiment)
        _save_experiments(experiments)

        return {
            "experiment_id": exp_id,
            "name": name,
            "status": "proposed",
            "next_step": "Build a minimal prototype, then call experiment_iterate() with your observations.",
        }

    @mcp.tool
    def experiment_iterate(
        experiment_id: str,
        action: str,
        observation: str,
        metrics: dict | None = None,
        files_changed: list[str] | None = None,
    ) -> dict:
        """Log an iteration of an experiment.

        experiment_id: from experiment_propose()
        action: what you did this iteration (e.g., 'wrote gravity_flip.gd, set force volume')
        observation: what happened (e.g., 'objects fly up but too fast, fps dropped to 45')
        metrics: optional measurements (e.g., {'fps': 45, 'gpu_ms': 3.2, 'feel': 'too fast'})
        files_changed: files created or modified

        Call this after each prototype → observe cycle."""
        experiments = _load_experiments()

        for exp in experiments:
            if exp["id"] == experiment_id:
                exp["status"] = "iterating"
                iteration = {
                    "iteration": len(exp["iterations"]) + 1,
                    "action": action,
                    "observation": observation,
                    "metrics": metrics or {},
                    "timestamp": time.time(),
                }
                exp["iterations"].append(iteration)
                if files_changed:
                    exp["files_created"].extend(files_changed)
                    exp["files_created"] = list(set(exp["files_created"]))
                _save_experiments(experiments)

                return {
                    "experiment_id": experiment_id,
                    "iteration": iteration["iteration"],
                    "status": "iterating",
                    "total_iterations": len(exp["iterations"]),
                }

        return {"error": f"Experiment not found: {experiment_id}"}

    @mcp.tool
    def experiment_conclude(
        experiment_id: str,
        result: str,
        summary: str,
        keep: bool = False,
        pattern_name: str | None = None,
    ) -> dict:
        """Conclude an experiment — success, partial, or failure.

        experiment_id: from experiment_propose()
        result: 'success' | 'partial' | 'failure'
        summary: what was learned (kept regardless of result — failures teach too)
        keep: if True, the code stays; if False, files should be cleaned up
        pattern_name: if keep=True, suggest a /save-pattern name for this invention

        Every conclusion adds to the knowledge base. Failed experiments prevent
        repeating the same mistake. Successful ones become new skills."""
        experiments = _load_experiments()

        for exp in experiments:
            if exp["id"] == experiment_id:
                exp["status"] = "concluded"
                exp["result"] = {
                    "outcome": result,
                    "summary": summary,
                    "keep": keep,
                    "pattern_name": pattern_name,
                    "concluded_at": time.time(),
                    "total_iterations": len(exp["iterations"]),
                }
                _save_experiments(experiments)

                response = {
                    "experiment_id": experiment_id,
                    "outcome": result,
                    "summary": summary,
                    "iterations": len(exp["iterations"]),
                }

                if keep and pattern_name:
                    response["next_step"] = f"Run /save-pattern {pattern_name} to make this a reusable skill."
                elif not keep:
                    response["next_step"] = "Clean up experiment files."
                    response["files_to_remove"] = exp.get("files_created", [])

                return response

        return {"error": f"Experiment not found: {experiment_id}"}

    @mcp.tool
    def experiment_list(
        status: str | None = None,
        type: str | None = None,
    ) -> dict:
        """List all experiments, optionally filtered by status or type.

        status: proposed | iterating | concluded
        type: shader | mechanic | visual | physics | audio | combination

        Use this to check what's been tried before inventing something new.
        Avoids repeating failed experiments and surfaces successful patterns."""
        experiments = _load_experiments()

        filtered = experiments
        if status:
            filtered = [e for e in filtered if e.get("status") == status]
        if type:
            filtered = [e for e in filtered if e.get("type") == type]

        # Compact summary
        summaries = []
        for exp in filtered:
            summary = {
                "id": exp["id"],
                "name": exp["name"],
                "hypothesis": exp["hypothesis"],
                "type": exp["type"],
                "status": exp["status"],
                "iterations": len(exp.get("iterations", [])),
            }
            if exp.get("result"):
                summary["outcome"] = exp["result"]["outcome"]
                summary["learning"] = exp["result"]["summary"]
            summaries.append(summary)

        return {
            "total": len(summaries),
            "experiments": summaries,
        }

    @mcp.tool
    def experiment_insights(type: str | None = None) -> dict:
        """Get insights from past experiments — what worked, what didn't, patterns found.

        type: filter by experiment type, or None for all

        Returns aggregated learnings to inform new inventions."""
        experiments = _load_experiments()

        if type:
            experiments = [e for e in experiments if e.get("type") == type]

        concluded = [e for e in experiments if e.get("result")]
        successes = [e for e in concluded if e["result"]["outcome"] == "success"]
        failures = [e for e in concluded if e["result"]["outcome"] == "failure"]
        partials = [e for e in concluded if e["result"]["outcome"] == "partial"]

        # Extract common building blocks from successes
        success_blocks = {}
        for exp in successes:
            for block in exp.get("building_blocks", []):
                success_blocks[block] = success_blocks.get(block, 0) + 1

        failure_blocks = {}
        for exp in failures:
            for block in exp.get("building_blocks", []):
                failure_blocks[block] = failure_blocks.get(block, 0) + 1

        return {
            "total_experiments": len(experiments),
            "successes": len(successes),
            "failures": len(failures),
            "partials": len(partials),
            "in_progress": len([e for e in experiments if e["status"] == "iterating"]),
            "success_learnings": [e["result"]["summary"] for e in successes],
            "failure_learnings": [e["result"]["summary"] for e in failures],
            "reliable_blocks": dict(sorted(success_blocks.items(), key=lambda x: -x[1])),
            "risky_blocks": dict(sorted(failure_blocks.items(), key=lambda x: -x[1])),
        }
