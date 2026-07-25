#!/usr/bin/env python3
"""Gate for publish-quality dense plates (persuasion bar).

Requires ≥2 hard presets with quality_plate=true, 1080p SHOW, map≥128, MAPTEST pass.
dB numbers without this bar are lab diagnostics, not the product face.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


HARD = {"spheres", "outdoor", "helmet", "mirror", "boombox", "toycar"}


def load_metrics(d: Path) -> dict | None:
    for name in ("dense_orm_metrics.json", "dense_metal_metrics.json"):
        p = d / name
        if p.is_file():
            return json.loads(p.read_text())
    return None


def is_pass(m: dict) -> bool:
    if m.get("mode") == "dense_metal" or m.get("dense_metal_sot"):
        mse_i, mse_r = float(m["metal_mse_init"]), float(m["metal_mse_recovered"])
    else:
        mse_i, mse_r = float(m["rough_mse_init"]), float(m["rough_mse_recovered"])
    d_psnr = float(m["train_psnr"]) - float(m["init_psnr"])
    d_rel = float(m["relight_recovered_psnr"]) - float(m["relight_init_psnr"])
    return mse_r < mse_i * 0.85 and d_psnr >= 2.0 and d_rel >= 1.5


def main(root: Path) -> int:
    if not root.is_dir():
        print(f"FAIL missing {root}")
        return 1

    runs = []
    for d in sorted(root.iterdir()):
        if not d.is_dir():
            continue
        m = load_metrics(d)
        if not m:
            continue
        runs.append((d, m))

    if not runs:
        print("FAIL no quality plate runs")
        return 1

    hard_pass = []
    for d, m in runs:
        # Required keys: a missing key must FAIL, never silently skip a check.
        # show_frames used to be absent from dense_metal metrics, which turned the
        # show_frames>=12 gate below into a guaranteed no-op for every metal run.
        missing = [k for k in ("show_wh", "dense_map_res", "show_frames") if m.get(k) is None]
        if missing:
            print(f"FAIL {d.name}: metrics missing required key(s): {', '.join(missing)}")
            return 1

        qp = m.get("quality_plate") is True
        show = m.get("show_wh")
        map_res = int(m["dense_map_res"])
        frames = int(m["show_frames"])
        preset = str(m.get("preset", d.name.split("_")[0])).lower()
        ok = is_pass(m)
        print(
            f"  {preset}: pass={ok} quality_plate={qp} show={show} map={map_res} "
            f"show_frames={frames} PSNR {m.get('init_psnr')}→{m.get('train_psnr')}"
        )
        if not qp:
            print(f"FAIL {d.name}: quality_plate must be true")
            return 1
        if not (isinstance(show, list) and len(show) == 2 and show[0] >= 1920 and show[1] >= 1080):
            print(f"FAIL {d.name}: SHOW must be ≥1920×1080 for quality plate")
            return 1
        if map_res < 128:
            print(f"FAIL {d.name}: dense_map_res must be ≥128")
            return 1
        if frames < 12:
            print(f"FAIL {d.name}: show_frames too low for clean plate ({frames})")
            return 1
        # Prefer HD show stills present
        stems = list(d.glob("*_show.png"))
        if len(stems) < 3:
            print(f"FAIL {d.name}: need ≥3 *_show.png stills, got {len(stems)}")
            return 1
        if ok and preset in HARD:
            hard_pass.append(preset)

    hard_pass = sorted(set(hard_pass))
    print(f"hard presets PASS: {hard_pass}")
    if len(hard_pass) < 2:
        print("FAIL need ≥2 hard presets (spheres/outdoor/helmet/…) MAPTEST pass under quality plate")
        return 1

    print("PASS tools/inverse_lab/test_quality_plate.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1] if len(sys.argv) > 1 else "renders/inverse_quality_plate")))
