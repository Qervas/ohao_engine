#!/usr/bin/env python3
"""Summarize a lab fit directory (holdout / relight metrics if present)."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def parse_run_log(path: Path) -> dict:
    if not path.is_file():
        return {}
    t = path.read_text(errors="ignore")
    out: dict = {}
    m = re.search(r"SHOW RMSE \(primary\) = ([0-9.eE+-]+)(?:\s+PSNR = ([0-9.eE+-]+))?(?:\s+SSIM = ([0-9.eE+-]+))?", t)
    if m:
        out["show_rmse_primary"] = float(m.group(1))
        if m.group(2):
            out["train_psnr"] = float(m.group(2))
        if m.group(3):
            out["train_ssim"] = float(m.group(3))
    m = re.search(r"holdout SHOW RMSE = ([0-9.eE+-]+)(?:\s+PSNR = ([0-9.eE+-]+))?(?:\s+SSIM = ([0-9.eE+-]+))?", t)
    if m:
        out["holdout_show_rmse"] = float(m.group(1))
        if m.group(2):
            out["holdout_psnr"] = float(m.group(2))
        if m.group(3):
            out["holdout_ssim"] = float(m.group(3))
    m = re.search(r"relight SHOW RMSE = ([0-9.eE+-]+)(?:\s+PSNR = ([0-9.eE+-]+))?(?:\s+SSIM = ([0-9.eE+-]+))?", t)
    if m:
        out["relight_show_rmse"] = float(m.group(1))
        if m.group(2):
            out["relight_psnr"] = float(m.group(2))
        if m.group(3):
            out["relight_ssim"] = float(m.group(3))
    m = re.search(r"holdout PSNR gain vs wrong-init = ([0-9.eE+-]+)", t)
    if m:
        out["holdout_psnr_gain_db"] = float(m.group(1))
    m = re.search(r"param RMSE = ([0-9.eE+-]+)", t)
    if m:
        out["param_rmse"] = float(m.group(1))
    if "LABTEST PASS" in t:
        out["labtest"] = "PASS"
    elif "LABTEST FAIL" in t:
        out["labtest"] = "FAIL"
    elif "SELFTEST PASS" in t:
        out["labtest"] = "SELFTEST_PASS"
    elif "SELFTEST FAIL" in t:
        out["labtest"] = "SELFTEST_FAIL"
    # Prefer machine-readable JSON if present
    jm = path.parent / "lab_metrics.json"
    if jm.is_file():
        try:
            j = json.loads(jm.read_text())
            out["lab_metrics"] = j
            if "holdout" in j and "psnr" in j["holdout"]:
                out["holdout_psnr"] = j["holdout"]["psnr"]
            if "relight" in j and "psnr" in j["relight"]:
                out["relight_psnr"] = j["relight"]["psnr"]
            if "holdout_psnr_gain_db" in j:
                out["holdout_psnr_gain_db"] = j["holdout_psnr_gain_db"]
        except Exception:
            pass
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("fit_dir", type=Path, help="inverse_fit --out-dir for a lab run")
    args = ap.parse_args()
    d = args.fit_dir
    metrics = {}
    for name in ("run.log", "lab.log", "fit.log"):
        p = d / name
        if p.is_file():
            metrics = parse_run_log(p)
            break
    # Also try scraping trajectory
    tj = d / "trajectory.json"
    if tj.is_file():
        try:
            t = json.loads(tj.read_text())
            metrics["schedule"] = t.get("schedule")
            metrics["best_loss"] = t.get("best_loss")
            metrics["lab_bundle"] = t.get("lab_bundle")
        except Exception:
            pass
    cap = d / "capture_used.json"
    if not cap.is_file():
        # parent capture pointer
        for c in d.glob("**/capture.json"):
            metrics["capture"] = str(c)
            break
    # Never overwrite C++ lab_metrics.json (capture-gated PSNR/SSIM gates live there).
    out = d / "eval_summary.json"
    out.write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
