#!/usr/bin/env python3
"""M3a gallery gate: ≥3 presets with at least one MAPTEST-passing dense run."""
from __future__ import annotations

import json
import sys
from pathlib import Path

# Keep import path free of package install; local sibling helper.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_gallery_wall import collect_runs  # noqa: E402


def main(root: Path, min_presets: int = 3) -> int:
    if not root.is_dir():
        print(f"FAIL missing gallery root {root}")
        return 1

    summary_path = root / "gallery_summary.json"
    if summary_path.is_file():
        summary = json.loads(summary_path.read_text())
        runs = summary.get("runs", [])
    else:
        runs = collect_runs(root)
        summary = {
            "n_runs": len(runs),
            "n_pass": sum(1 for r in runs if r.get("pass")),
            "n_presets_pass": len({r["preset"] for r in runs if r.get("pass")}),
            "presets": sorted({r["preset"] for r in runs}),
            "runs": runs,
        }

    if not runs:
        print("FAIL no gallery runs")
        return 1

    presets_pass = sorted({r["preset"] for r in runs if r.get("pass")})
    n_pass = sum(1 for r in runs if r.get("pass"))

    print(
        f"gallery: runs={len(runs)} pass={n_pass}; "
        f"presets_pass={presets_pass} (need ≥{min_presets})"
    )
    for r in runs:
        m = r.get("metrics", {})
        print(
            f"  {r.get('preset')}/{r.get('mode')}: "
            f"pass={r.get('pass')} PSNR "
            f"{m.get('init_psnr', '?')}→{m.get('train_psnr', '?')}"
        )

    wall = root / "gallery_wall.html"
    if not wall.is_file() or wall.stat().st_size < 200:
        print(f"FAIL missing/small gallery wall {wall}")
        return 1

    # Each passing run must have a triple still strip available.
    for r in runs:
        if not r.get("pass"):
            continue
        stills = r.get("stills") or {}
        for key in ("init", "recovered", "target"):
            name = stills.get(key)
            if not name:
                print(f"FAIL pass run {r.get('dir')} missing still {key}")
                return 1
            p = root / r["dir"] / name
            if not p.is_file() or p.stat().st_size < 64:
                print(f"FAIL still missing/small: {p}")
                return 1

    ok = len(presets_pass) >= min_presets and n_pass >= min_presets
    print(("PASS" if ok else "FAIL") + " tools/inverse_lab/test_gallery.py")
    if not ok:
        print(f"  need ≥{min_presets} presets with MAPTEST pass; got {presets_pass}")
    return 0 if ok else 1


if __name__ == "__main__":
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "renders/inverse_gallery")
    raise SystemExit(main(path))
