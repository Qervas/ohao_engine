#!/usr/bin/env python3
"""MAPTEST gate for H1/M1a dense albedo inverse (reads real metrics JSON)."""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main(out: Path) -> int:
    metrics = out / "dense_map_metrics.json"
    if not metrics.is_file():
        print(f"FAIL missing {metrics}")
        return 1

    m = json.loads(metrics.read_text())
    for key in (
        "init_psnr",
        "train_psnr",
        "map_mse_init",
        "map_mse_recovered",
        "beauty_theta_path",
        "dense_map_sot",
        "wrong_init_source",
        "dense_map_res",
    ):
        if key not in m:
            print(f"FAIL missing field {key}")
            return 1

    if m.get("dense_map_sot") is not True:
        print("FAIL dense_map_sot is not true")
        return 1
    if m.get("beauty_theta_path") != "dense_map_bindless_deferred":
        print(f"FAIL unexpected beauty path {m.get('beauty_theta_path')}")
        return 1
    if str(m.get("wrong_init_source", "")).lower().startswith("gt"):
        print("FAIL wrong_init must not be GT")
        return 1
    if int(m.get("dense_map_res", 0)) < 32:
        print("FAIL dense_map_res too small")
        return 1
    # M1c: prefer in-place upload when field present
    upload = m.get("map_upload")
    if upload is not None and "in_place" not in str(upload):
        print(f"WARN map_upload={upload} (expected in_place path for M1c)")

    init_psnr = float(m["init_psnr"])
    train_psnr = float(m["train_psnr"])
    mse_i = float(m["map_mse_init"])
    mse_r = float(m["map_mse_recovered"])
    d_psnr = train_psnr - init_psnr

    print(
        f"dense metrics: map_mse {mse_i:.6f} → {mse_r:.6f}; "
        f"PSNR {init_psnr:.3f} → {train_psnr:.3f} (Δ={d_psnr:.3f} dB)"
    )

    ok_map = mse_r < mse_i * 0.85
    ok_psnr = d_psnr >= 2.0
    ok = ok_map and ok_psnr

    # Stills must exist and be non-trivial.
    stills = [
        out / "dense_init.png",
        out / "dense_recovered.png",
        out / "dense_forward_truth.png",
        out / "materials" / "ground_albedo_init.png",
        out / "materials" / "ground_albedo_recovered.png",
        out / "materials" / "ground_albedo_gt.png",
    ]
    for p in stills:
        if not p.is_file() or p.stat().st_size < 64:
            print(f"FAIL still missing/small: {p}")
            return 1

    print(("PASS" if ok else "FAIL") + " tools/inverse_lab/test_dense_map.py")
    if not ok:
        print(f"  map_drop={ok_map} psnr_gain>={2.0}dB={ok_psnr}")
    return 0 if ok else 1


if __name__ == "__main__":
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "renders/diff_dense")
    raise SystemExit(main(path))
