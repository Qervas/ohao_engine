#!/usr/bin/env python3
"""MAPTEST + relight gate for H2/M2a dense roughness/ORM inverse."""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main(out: Path) -> int:
    metrics = out / "dense_orm_metrics.json"
    if not metrics.is_file():
        print(f"FAIL missing {metrics}")
        return 1

    m = json.loads(metrics.read_text())
    for key in (
        "init_psnr",
        "train_psnr",
        "rough_mse_init",
        "rough_mse_recovered",
        "beauty_theta_path",
        "dense_orm_sot",
        "wrong_init_source",
        "dense_map_res",
        "relight_init_psnr",
        "relight_recovered_psnr",
    ):
        if key not in m:
            print(f"FAIL missing field {key}")
            return 1

    if m.get("dense_orm_sot") is not True:
        print("FAIL dense_orm_sot is not true")
        return 1
    if m.get("beauty_theta_path") != "dense_orm_bindless_deferred":
        print(f"FAIL unexpected beauty path {m.get('beauty_theta_path')}")
        return 1
    if str(m.get("wrong_init_source", "")).lower().startswith("gt"):
        print("FAIL wrong_init must not be GT")
        return 1
    if int(m.get("dense_map_res", 0)) < 32:
        print("FAIL dense_map_res too small")
        return 1
    if m.get("albedo_free") is True:
        print("FAIL M2a expects fixed albedo (albedo_free false)")
        return 1

    init_psnr = float(m["init_psnr"])
    train_psnr = float(m["train_psnr"])
    mse_i = float(m["rough_mse_init"])
    mse_r = float(m["rough_mse_recovered"])
    d_psnr = train_psnr - init_psnr
    rel_i = float(m["relight_init_psnr"])
    rel_r = float(m["relight_recovered_psnr"])
    d_rel = rel_r - rel_i

    print(
        f"orm metrics: rough_mse {mse_i:.6f} → {mse_r:.6f}; "
        f"PSNR {init_psnr:.3f} → {train_psnr:.3f} (Δ={d_psnr:.3f} dB); "
        f"relight {rel_i:.3f} → {rel_r:.3f} (Δ={d_rel:.3f} dB)"
    )

    ok_map = mse_r < mse_i * 0.85
    ok_psnr = d_psnr >= 2.0
    ok_relight = d_rel >= 1.5
    ok = ok_map and ok_psnr and ok_relight

    stills = [
        out / "orm_init.png",
        out / "orm_recovered.png",
        out / "orm_forward_truth.png",
        out / "orm_relight_truth.png",
        out / "orm_relight_recovered.png",
        out / "materials" / "ground_rough_init.png",
        out / "materials" / "ground_rough_recovered.png",
        out / "materials" / "ground_rough_gt.png",
        out / "materials" / "ground_orm_recovered.png",
    ]
    for p in stills:
        if not p.is_file() or p.stat().st_size < 64:
            print(f"FAIL still missing/small: {p}")
            return 1

    print(("PASS" if ok else "FAIL") + " tools/inverse_lab/test_dense_orm.py")
    if not ok:
        print(f"  map_drop={ok_map} psnr_gain>={2.0}dB={ok_psnr} relight_gain>={1.5}dB={ok_relight}")
    return 0 if ok else 1


if __name__ == "__main__":
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "renders/diff_dense_orm")
    raise SystemExit(main(path))
