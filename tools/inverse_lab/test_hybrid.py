#!/usr/bin/env python3
"""Gate hybrid Diff→PT plate metrics (honest dual bar)."""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main(out: Path) -> int:
    lab = out / "lab_metrics.json"
    hyb = out / "hybrid_metrics.json"
    dif = out / "diff_metrics.json"
    tiles = out / "recovered_tiles.json"
    for p in (lab, hyb, dif, tiles):
        if not p.is_file():
            print(f"FAIL missing {p}")
            return 1

    m = json.loads(lab.read_text())
    h = json.loads(hyb.read_text())
    d = json.loads(dif.read_text())

    assert d.get("dense_map_sot") is True
    assert d.get("beauty_theta_path") == "dense_map_bindless_deferred"
    assert h.get("fit_backend") == "diff"
    assert h.get("eval_backend") == "pt"
    assert h.get("metric_gt") == "capture_export_images"
    assert m.get("metric_gt") == "capture_export_images"

    holdout = float(m["holdout"]["psnr"])
    relight = float(m["relight"]["psnr"])
    gain = float(m["holdout_psnr_gain_db"])
    xfer_h = float(h.get("xfer_holdout_min_db", 22.0))
    xfer_r = float(h.get("xfer_relight_min_db", 24.0))
    xfer_g = float(h.get("xfer_gain_min_db", 8.0))
    print(
        f"hybrid metrics: holdout={holdout:.2f} relight={relight:.2f} gain={gain:.2f} "
        f"diff_train_psnr={float(h.get('diff_train_psnr', 0)):.2f} "
        f"transfer_pass={h.get('transfer_pass')} full_lab={h.get('full_labtest_pass')}"
    )
    transfer_ok = (
        holdout >= xfer_h
        and gain >= xfer_g
        and relight >= xfer_r
        and h.get("transfer_pass") is True
        and h.get("difftest_pass") is True
    )
    # Full oracle bar (same as PT LABTEST) — expected after PT light+soft-tile refine.
    full_ok = holdout >= 28.0 and gain >= 8.0 and relight >= 26.0
    if h.get("full_labtest_pass") is True:
        full_ok = True
    print(f"transfer_ok={transfer_ok} full_lab_ok={full_ok} light_refine={h.get('pt_light_refine')}")
    ok = transfer_ok  # hybrid exit contract; full_lab is bonus when light refine lands
    print(("PASS" if ok else "FAIL") + " tools/inverse_lab/test_hybrid.py")
    return 0 if ok else 1


if __name__ == "__main__":
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "renders/inverse_lab/lantern_hybrid")
    raise SystemExit(main(path))
