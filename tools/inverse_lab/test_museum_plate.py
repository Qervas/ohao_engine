#!/usr/bin/env python3
"""Publish-face museum plate gate: MAPTEST + 1080p SHOW stills + amphora protocol."""
from __future__ import annotations

import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None  # type: ignore


def main(out: Path) -> int:
    metrics = out / "dense_map_metrics.json"
    if not metrics.is_file():
        print(f"FAIL missing {metrics}")
        return 1
    m = json.loads(metrics.read_text())

    if m.get("dense_map_sot") is not True:
        print("FAIL dense_map_sot not true")
        return 1
    if m.get("museum_studio") is not True and m.get("metric_domain") != "ohao_museum_studio_protocol":
        print("FAIL not a museum studio plate")
        return 1

    mse_i = float(m["map_mse_init"])
    mse_r = float(m["map_mse_recovered"])
    d_psnr = float(m["train_psnr"]) - float(m["init_psnr"])
    ok_map = mse_r < mse_i * 0.85
    ok_psnr = d_psnr >= 2.0
    print(f"map_mse {mse_i:.6f}→{mse_r:.6f}  ΔPSNR={d_psnr:.2f} dB")
    if not (ok_map and ok_psnr):
        print("FAIL MAPTEST")
        return 1

    show = m.get("show_wh") or [0, 0]
    if not (isinstance(show, list) and len(show) >= 2 and show[0] >= 1280 and show[1] >= 720):
        print(f"FAIL show_wh {show} < 1280×720")
        return 1
    print(f"show_wh={show[0]}x{show[1]}")

    for stem in ("dense_init_show", "dense_recovered_show", "dense_forward_truth_show"):
        p = out / f"{stem}.png"
        if not p.is_file() or p.stat().st_size < 1000:
            print(f"FAIL missing/small SHOW still {p}")
            return 1
        if Image is not None:
            im = Image.open(p)
            if im.size[0] < 1280 or im.size[1] < 720:
                print(f"FAIL {p.name} resolution {im.size} < 1280×720")
                return 1
            print(f"  {p.name}: {im.size[0]}x{im.size[1]}")

    print("PASS tools/inverse_lab/test_museum_plate.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1] if len(sys.argv) > 1 else "renders/diff_museum_plate")))
