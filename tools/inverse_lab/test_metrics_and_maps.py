#!/usr/bin/env python3
"""Gating tests for inverse lab frontier metrics + map artifacts.

Drives real shipped artifacts (lab_metrics.json, map PNGs) — does not re-implement
PSNR thresholds with hard-coded pass values independent of the run.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image
import numpy as np


def main() -> int:
    fit = Path(sys.argv[1] if len(sys.argv) > 1 else "renders/inverse_lab/lantern_frontier_fit")
    metrics_path = fit / "lab_metrics.json"
    assert metrics_path.is_file(), f"missing {metrics_path}"
    m = json.loads(metrics_path.read_text())

    # Required keys
    for k in ("train", "holdout", "relight", "wrong_init_holdout", "holdout_psnr_gain_db"):
        assert k in m, f"missing key {k}"
    for split in ("train", "holdout", "relight"):
        for mk in ("psnr", "ssim", "rmse"):
            assert mk in m[split], f"missing {split}.{mk}"

    holdout_psnr = float(m["holdout"]["psnr"])
    relight_psnr = float(m["relight"]["psnr"])
    gain = float(m["holdout_psnr_gain_db"])
    wrong = float(m["wrong_init_holdout"]["psnr"])
    metric_gt = m.get("metric_gt", "")

    print(
        f"metric_gt={metric_gt} holdout PSNR={holdout_psnr:.3f}  relight={relight_psnr:.3f}  "
        f"gain={gain:.3f}  wrong={wrong:.3f}"
    )
    assert metric_gt == "capture_export_images", (
        f"LABTEST must gate on capture GT, got metric_gt={metric_gt!r}"
    )
    # Live diag may be higher — must not be used as the gated field.
    live = m["holdout"].get("psnr_live_diag")
    if live is not None:
        print(f"holdout live_diag PSNR={float(live):.3f} (not a gate)")

    # Frontier bar (same as C++ LABTEST) — capture-gated numbers only
    assert holdout_psnr >= 28.0, f"capture holdout PSNR {holdout_psnr} < 28"
    assert relight_psnr >= 26.0, f"capture relight PSNR {relight_psnr} < 26"
    assert gain >= 8.0, f"holdout gain {gain} < 8 dB"
    assert holdout_psnr > wrong + 7.9, "gain inconsistent with wrong_init"

    # Maps: init vs recovered must differ (spatial materials participate)
    mat = fit / "materials"
    init_p = mat / "ground_albedo_init.png"
    rec_p = mat / "ground_albedo_recovered.png"
    # also accept tag naming from writeGroundMapsFromTheta
    if not init_p.is_file():
        init_p = mat / "ground_albedo_init.png"
    if not rec_p.is_file():
        # try alternate
        cands = list(mat.glob("ground_albedo_*.png"))
        print("map candidates", cands)
        assert any("init" in p.name for p in cands), "no init albedo map"
        assert any("recovered" in p.name for p in cands), "no recovered albedo map"
        init_p = next(p for p in cands if "init" in p.name)
        rec_p = next(p for p in cands if "recovered" in p.name)

    a = np.asarray(Image.open(init_p).convert("RGB"), dtype=np.float32)
    b = np.asarray(Image.open(rec_p).convert("RGB"), dtype=np.float32)
    mse = float(np.mean((a - b) ** 2))
    print(f"map init vs recovered MSE={mse:.4f}  shape={a.shape}")
    assert mse > 10.0, f"maps too similar (MSE={mse}); recovery may not have updated maps"
    assert a.shape[0] >= 2 and a.shape[1] >= 2, "maps must be spatial (not 1x1)"

    print("PASS tools/inverse_lab/test_metrics_and_maps.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
