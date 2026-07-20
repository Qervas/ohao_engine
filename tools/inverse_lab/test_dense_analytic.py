#!/usr/bin/env python3
"""H4/M5a–b gate: GRADCHECK + analytic Adam optim + speed vs FD estimate."""
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
    optim = m.get("optim_analytic") is True
    rel = float(m["grad_median_rel_err"])
    speedup = float(m.get("speedup_vs_fd") or 0)
    a_ms = float(m.get("analytic_optim_ms") or 0)
    fd_est = float(m.get("fd_est_3pass_ms") or 0)

    print(
        f"analytic_grad={analytic} optim_analytic={optim} "
        f"grad_median_rel_err={rel:.4f} speedup={speedup:.1f}× "
        f"analytic_ms={a_ms:.0f} fd_est_ms={fd_est:.0f}"
    )

    if not analytic:
        print("FAIL GRADCHECK did not pass")
        return 1
    if rel > 0.20:
        print(f"FAIL median rel err {rel} > 0.20")
        return 1

    # MAPTEST quality
    mse_i = float(m["map_mse_init"])
    mse_r = float(m["map_mse_recovered"])
    d_psnr = float(m["train_psnr"]) - float(m["init_psnr"])
    ok_map = mse_r < mse_i * 0.85
    ok_psnr = d_psnr >= 2.0
    print(f"map_mse {mse_i:.4f}→{mse_r:.4f} PSNR Δ={d_psnr:.2f} dB")
    if not (ok_map and ok_psnr):
        print("FAIL MAPTEST quality")
        return 1

    # M5b: analytic+sparse path (not full 3-pass FD) + ≥3× speedup
    if not optim:
        print("FAIL optim_analytic false (full FD fallback)")
        return 1
    if speedup < 3.0:
        print(f"FAIL speedup {speedup:.1f}× < 3× vs 3-pass FD estimate")
        return 1
    if speedup >= 10.0:
        print(f"SPEED 10×+ achieved ({speedup:.1f}×)")
    else:
        print(f"NOTE speedup {speedup:.1f}× (target 10×; ≥3× with MAPTEST accepted for M5b hybrid)")

    print("PASS tools/inverse_lab/test_dense_analytic.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1] if len(sys.argv) > 1 else "renders/diff_dense_analytic")))
