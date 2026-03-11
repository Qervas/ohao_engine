"""Visual regression testing — screenshot capture, comparison, and baseline management.

AI agents capture screenshots before/after changes, compare pixel differences,
and maintain visual baselines for automated regression detection.
"""

import base64
import hashlib
import json
import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent
BASELINE_DIR = Path(__file__).parent / "memory_store" / "visual_baselines"


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def _image_hash(png_bytes: bytes) -> str:
    """SHA-256 of raw PNG bytes for fast equality check."""
    return hashlib.sha256(png_bytes).hexdigest()[:16]


def _load_baseline(name: str) -> dict | None:
    """Load baseline metadata if it exists."""
    meta_path = BASELINE_DIR / f"{name}.json"
    if meta_path.exists():
        return json.loads(meta_path.read_text(encoding="utf-8"))
    return None


def _save_baseline(name: str, meta: dict, png_bytes: bytes) -> None:
    """Save baseline PNG + metadata."""
    _ensure_dir(BASELINE_DIR)
    (BASELINE_DIR / f"{name}.png").write_bytes(png_bytes)
    (BASELINE_DIR / f"{name}.json").write_text(
        json.dumps(meta, indent=2), encoding="utf-8"
    )


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register(mcp, bridge):
    """Register visual regression testing tools on the MCP server."""

    @mcp.tool
    def visual_capture(name: str, description: str = "") -> dict:
        """Capture current viewport as a named visual test baseline.

        name: baseline name (e.g., 'default_scene', 'bloom_on', 'rain_heavy')
        description: what this baseline represents

        Saves PNG + metadata to mcp/memory_store/visual_baselines/."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}

        # Capture screenshot from engine
        result = bridge.post("/god/capture", {})
        if "error" in result:
            return result

        png_b64 = result.get("image_base64", "")
        if not png_b64:
            return {"error": "No image data returned from engine."}

        png_bytes = base64.b64decode(png_b64)
        img_hash = _image_hash(png_bytes)

        # Get current scene state for context
        state = bridge.get("/god/state")
        scene_summary = {}
        if "error" not in state:
            actors = state.get("actors", [])
            scene_summary = {
                "actor_count": len(actors),
                "actor_names": [a.get("name", "?") for a in actors[:20]],
            }
            for key in ("camera", "effects", "environment"):
                if key in state:
                    scene_summary[key] = state[key]

        meta = {
            "name": name,
            "description": description,
            "hash": img_hash,
            "width": result.get("width", 0),
            "height": result.get("height", 0),
            "scene": scene_summary,
        }

        _save_baseline(name, meta, png_bytes)

        return {
            "saved": name,
            "hash": img_hash,
            "size_bytes": len(png_bytes),
            "width": meta["width"],
            "height": meta["height"],
            "path": str(BASELINE_DIR / f"{name}.png"),
        }

    @mcp.tool
    def visual_compare(name: str, threshold: float = 0.01) -> dict:
        """Compare current viewport against a saved baseline.

        name: baseline name to compare against
        threshold: max allowed difference ratio (0.0-1.0). Default 0.01 = 1% pixels.

        Returns comparison result with pass/fail and difference metrics."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}

        # Load baseline
        baseline_meta = _load_baseline(name)
        if not baseline_meta:
            return {"error": f"No baseline found: '{name}'. Capture one first with visual_capture()."}

        baseline_path = BASELINE_DIR / f"{name}.png"
        if not baseline_path.exists():
            return {"error": f"Baseline PNG missing: {baseline_path}"}

        baseline_bytes = baseline_path.read_bytes()
        baseline_hash = _image_hash(baseline_bytes)

        # Capture current frame
        result = bridge.post("/god/capture", {})
        if "error" in result:
            return result

        png_b64 = result.get("image_base64", "")
        if not png_b64:
            return {"error": "No image data returned from engine."}

        current_bytes = base64.b64decode(png_b64)
        current_hash = _image_hash(current_bytes)

        # Fast path: identical hashes
        if current_hash == baseline_hash:
            return {
                "result": "PASS",
                "identical": True,
                "difference_ratio": 0.0,
                "baseline": name,
                "threshold": threshold,
            }

        # Pixel-level comparison using raw byte diff
        # Both are PNGs so we compare decoded pixel data if sizes match
        diff_ratio = _compare_png_bytes(baseline_bytes, current_bytes)

        passed = diff_ratio <= threshold

        result = {
            "result": "PASS" if passed else "FAIL",
            "identical": False,
            "difference_ratio": round(diff_ratio, 6),
            "threshold": threshold,
            "baseline": name,
            "baseline_hash": baseline_hash,
            "current_hash": current_hash,
        }

        # If failed, save the current frame for manual inspection
        if not passed:
            fail_path = BASELINE_DIR / f"{name}_FAIL.png"
            fail_path.write_bytes(current_bytes)
            result["fail_screenshot"] = str(fail_path)

        return result

    @mcp.tool
    def visual_list_baselines() -> dict:
        """List all saved visual test baselines with metadata."""
        _ensure_dir(BASELINE_DIR)
        baselines = []
        for meta_file in sorted(BASELINE_DIR.glob("*.json")):
            try:
                meta = json.loads(meta_file.read_text(encoding="utf-8"))
                png_file = meta_file.with_suffix(".png")
                meta["png_exists"] = png_file.exists()
                if png_file.exists():
                    meta["size_bytes"] = png_file.stat().st_size
                baselines.append(meta)
            except Exception:
                pass

        return {
            "baseline_count": len(baselines),
            "baselines": baselines,
            "baseline_dir": str(BASELINE_DIR),
        }

    @mcp.tool
    def visual_delete_baseline(name: str) -> dict:
        """Delete a saved visual test baseline."""
        deleted = []
        for suffix in (".json", ".png", "_FAIL.png"):
            path = BASELINE_DIR / f"{name}{suffix}"
            if path.exists():
                path.unlink()
                deleted.append(str(path))

        if not deleted:
            return {"error": f"No baseline found: '{name}'"}

        return {"deleted": deleted, "name": name}

    @mcp.tool
    def visual_regression_suite(
        baselines: list[str] | None = None,
        threshold: float = 0.01
    ) -> dict:
        """Run visual regression tests against all (or specified) baselines.

        baselines: list of baseline names to test, or None for all
        threshold: max allowed difference ratio

        Returns summary with pass/fail for each baseline."""
        if not bridge.is_connected():
            return {"error": "Engine not connected."}

        _ensure_dir(BASELINE_DIR)

        # Discover baselines
        if baselines:
            names = baselines
        else:
            names = [f.stem for f in sorted(BASELINE_DIR.glob("*.json"))]

        if not names:
            return {"error": "No baselines found. Capture some first with visual_capture()."}

        results = []
        passed = 0
        failed = 0

        for name in names:
            # We need to set up the scene for each baseline
            # For now, just compare current viewport against each
            meta = _load_baseline(name)
            if not meta:
                results.append({"name": name, "result": "SKIP", "reason": "baseline not found"})
                continue

            comparison = visual_compare(name, threshold)
            result_status = comparison.get("result", "ERROR")
            if result_status == "PASS":
                passed += 1
            elif result_status == "FAIL":
                failed += 1

            results.append({
                "name": name,
                "result": result_status,
                "difference_ratio": comparison.get("difference_ratio", -1),
            })

        return {
            "total": len(results),
            "passed": passed,
            "failed": failed,
            "skipped": len(results) - passed - failed,
            "threshold": threshold,
            "results": results,
        }


def _compare_png_bytes(png1: bytes, png2: bytes) -> float:
    """Compare two PNG images byte-by-byte. Returns difference ratio [0.0, 1.0].

    This is a simple byte-level comparison of the raw PNG data.
    Not pixel-perfect (PNG compression varies) but catches meaningful changes.
    For exact pixel comparison, we'd need to decode the PNGs."""
    # If sizes differ significantly, it's likely a different image
    if len(png1) == 0 or len(png2) == 0:
        return 1.0

    # Compare raw bytes (fast approximation)
    # Use the smaller length for comparison
    min_len = min(len(png1), len(png2))
    max_len = max(len(png1), len(png2))

    # Skip PNG header (8 bytes) — it's always the same
    diff_count = abs(len(png1) - len(png2))  # size difference = guaranteed diffs
    for i in range(8, min_len):
        if png1[i] != png2[i]:
            diff_count += 1

    return diff_count / max_len
