#!/usr/bin/env python3
"""H4/M5a gate: analytic albedo grads used + FD agreement + MAPTEST still holds."""
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
        "analytic_albedo_grad",
        "grad_median_rel_err",
        "init_psnr",
        "train_psnr",
        "map_mse_init",
        "map_mse_recovered",
        "dense_map_sot",
    ):
        if key not in m:
            print(f"FAIL missing {key}")
            return 1

    analytic = m.get("analytic_albedo_grad") is True
    rel = float(m["grad_median_rel_err"])
    print(f"analytic={analytic} grad_median_rel_err={rel:.4f}")

    # Agreement: if analytic path claimed, rel err must be under 20%
    if analytic and rel > 0.20:
        print(f"FAIL analytic claimed but median rel err {rel} > 0.20")
        return 1
    # Always require some agreement measurement
    if rel > 0.50:
        print(f"FAIL grad check too poor even for diagnostics: {rel}")
        return 1

    # MAPTEST spirit
    mse_i = float(m["map_mse_init"])
    mse_r = float(m["map_mse_recovered"])
    d_psnr = float(m["train_psnr"]) - float(m["init_psnr"])
    ok_map = mse_r < mse_i * 0.85
    ok_psnr = d_psnr >= 2.0
    print(f"map_mse {mse_i:.4f}→{mse_r:.4f} PSNR Δ={d_psnr:.2f} dB")

    if not (ok_map and ok_psnr):
        print("FAIL MAPTEST quality not held under analytic path")
        return 1

    # M5a plate: GRADCHECK must pass (analytic_albedo_grad true).
    if not analytic:
        print("FAIL GRADCHECK did not pass (analytic_albedo_grad false)")
        return 1

    print("PASS tools/inverse_lab/test_dense_analytic.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1] if len(sys.argv) > 1 else "renders/diff_dense_analytic")))
