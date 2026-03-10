"""Persistent knowledge store — memory that grows across sessions.

Stores structured knowledge entries as JSON on disk. AI agents use this to
remember architecture decisions, shader experiments, performance baselines,
and debugging insights.
"""

import json
import time
from pathlib import Path

STORE_DIR = Path(__file__).parent / "memory_store"
KNOWLEDGE_FILE = STORE_DIR / "knowledge.json"
EXPERIMENTS_FILE = STORE_DIR / "experiments.json"
PERF_FILE = STORE_DIR / "perf_baselines.json"

# ─────────────────────────────────────────────────────────────────────────────
# Storage helpers
# ─────────────────────────────────────────────────────────────────────────────

def _ensure_store():
    """Create store directory and files if they don't exist."""
    STORE_DIR.mkdir(parents=True, exist_ok=True)
    for f, default in [
        (KNOWLEDGE_FILE, {"entries": []}),
        (EXPERIMENTS_FILE, {"experiments": []}),
        (PERF_FILE, {"baselines": []}),
    ]:
        if not f.exists():
            f.write_text(json.dumps(default, indent=2), encoding="utf-8")


def _load(path: Path) -> dict:
    """Load a JSON store file."""
    _ensure_store()
    return json.loads(path.read_text(encoding="utf-8"))


def _save(path: Path, data: dict):
    """Save a JSON store file."""
    _ensure_store()
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def _timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _matches(entry: dict, query: str | None, tags: list[str] | None) -> bool:
    """Check if entry matches search criteria."""
    if tags:
        entry_tags = set(entry.get("tags", []))
        if not entry_tags.intersection(tags):
            return False
    if query:
        q = query.lower()
        searchable = f"{entry.get('key', '')} {entry.get('value', '')}".lower()
        if q not in searchable:
            return False
    return True


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register memory tools on the MCP server."""

    @mcp.tool
    def memory_store(key: str, value: str, tags: list[str] | None = None) -> dict:
        """Store a knowledge entry. If the key exists, updates it.

        key: unique identifier (e.g., 'bloom-karis-technique')
        value: the knowledge to store
        tags: categories for filtering (e.g., ['shader', 'bloom', 'technique'])"""
        data = _load(KNOWLEDGE_FILE)
        now = _timestamp()

        # Update existing or create new
        for entry in data["entries"]:
            if entry["key"] == key:
                entry["value"] = value
                entry["tags"] = tags or entry.get("tags", [])
                entry["updated"] = now
                _save(KNOWLEDGE_FILE, data)
                return {"stored": key, "action": "updated"}

        data["entries"].append({
            "key": key,
            "value": value,
            "tags": tags or [],
            "created": now,
            "updated": now,
        })
        _save(KNOWLEDGE_FILE, data)
        return {"stored": key, "action": "created", "total_entries": len(data["entries"])}

    @mcp.tool
    def memory_recall(query: str | None = None, tags: list[str] | None = None) -> dict:
        """Search knowledge entries by keyword and/or tags.

        query: text to search in key and value fields
        tags: filter by any of these tags
        Returns matching entries sorted by most recently updated."""
        data = _load(KNOWLEDGE_FILE)
        matches = [e for e in data["entries"] if _matches(e, query, tags)]
        matches.sort(key=lambda e: e.get("updated", ""), reverse=True)
        return {"count": len(matches), "entries": matches}

    @mcp.tool
    def memory_list(tag: str | None = None, limit: int = 50) -> dict:
        """List knowledge entries, optionally filtered by a single tag.
        Returns keys and tags for quick browsing."""
        data = _load(KNOWLEDGE_FILE)
        entries = data["entries"]
        if tag:
            entries = [e for e in entries if tag in e.get("tags", [])]
        entries = entries[:limit]

        # Collect all tags for discovery
        all_tags: set[str] = set()
        for e in data["entries"]:
            all_tags.update(e.get("tags", []))

        return {
            "count": len(entries),
            "entries": [{"key": e["key"], "tags": e.get("tags", []),
                         "updated": e.get("updated", "")} for e in entries],
            "all_tags": sorted(all_tags),
        }

    @mcp.tool
    def memory_delete(key: str) -> dict:
        """Delete a knowledge entry by key."""
        data = _load(KNOWLEDGE_FILE)
        original = len(data["entries"])
        data["entries"] = [e for e in data["entries"] if e["key"] != key]
        if len(data["entries"]) == original:
            return {"error": f"Key '{key}' not found"}
        _save(KNOWLEDGE_FILE, data)
        return {"deleted": key, "remaining": len(data["entries"])}

    @mcp.tool
    def memory_log_experiment(
        description: str,
        outcome: str,
        details: str | None = None,
        tags: list[str] | None = None,
    ) -> dict:
        """Log a shader/code experiment with its outcome.

        description: what was tried
        outcome: 'success', 'failure', 'partial', or 'reverted'
        details: additional notes about what happened
        tags: categories (e.g., ['shader', 'ssao', 'optimization'])"""
        data = _load(EXPERIMENTS_FILE)
        entry = {
            "id": len(data["experiments"]) + 1,
            "description": description,
            "outcome": outcome,
            "details": details or "",
            "tags": tags or [],
            "timestamp": _timestamp(),
        }
        data["experiments"].append(entry)
        _save(EXPERIMENTS_FILE, data)
        return {"logged": entry["id"], "total_experiments": len(data["experiments"])}

    @mcp.tool
    def memory_experiments(
        outcome: str | None = None,
        tags: list[str] | None = None,
        limit: int = 20,
    ) -> dict:
        """List past experiments, optionally filtered by outcome or tags.

        outcome: 'success', 'failure', 'partial', 'reverted'
        tags: filter by any matching tag"""
        data = _load(EXPERIMENTS_FILE)
        results = data["experiments"]
        if outcome:
            results = [e for e in results if e["outcome"] == outcome]
        if tags:
            tag_set = set(tags)
            results = [e for e in results if tag_set.intersection(e.get("tags", []))]
        results = results[-limit:]  # Most recent
        return {"count": len(results), "experiments": results}

    @mcp.tool
    def memory_perf_baseline(label: str | None = None) -> dict:
        """Capture or retrieve a performance baseline.

        If label is given, captures current engine perf and stores it.
        If label is None, returns the most recent baseline.
        Requires engine running for capture."""
        data = _load(PERF_FILE)

        if label:
            if not bridge.is_connected():
                return {"error": "Engine not running — cannot capture baseline"}
            try:
                perf = bridge.get("/introspect/perf")
            except Exception:
                perf = {"note": "perf endpoint not yet available"}

            entry = {
                "label": label,
                "timestamp": _timestamp(),
                "data": perf,
            }
            data["baselines"].append(entry)
            _save(PERF_FILE, data)
            return {"captured": label, "total_baselines": len(data["baselines"])}

        # Return most recent
        if not data["baselines"]:
            return {"error": "No baselines stored yet"}
        return data["baselines"][-1]
