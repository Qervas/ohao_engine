#!/usr/bin/env python3
"""Structural proof: apply path writes different map buffers → different PNG pixels.

Uses the real inverse_fit binary to export a capture (maps written by shipped C++)
and asserts GT vs init albedo maps differ (same code path as recovery maps).
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "inverse_fit"
OUT = ROOT / "renders" / "inverse_lab" / "map_apply_selftest"


def main() -> int:
    assert BIN.is_file(), f"missing binary {BIN}"
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(BIN),
        "--export-capture",
        "--preset",
        "lantern",
        "--quality",
        "draft",
        "--views",
        "2",
        "--show-width",
        "640",
        "--show-height",
        "360",
        "--show-spp",
        "32",
        "--fit-spp",
        "16",
        "--map-res",
        "2",
        "--out-dir",
        str(OUT),
    ]
    print("running", " ".join(cmd))
    r = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    print(r.stdout[-2000:] if r.stdout else "")
    if r.returncode != 0:
        print(r.stderr[-2000:])
        return r.returncode

    mat = OUT / "capture" / "materials"
    gt = mat / "ground_albedo.png"
    init = mat / "ground_albedo_init.png"
    assert gt.is_file() and init.is_file(), f"missing maps under {mat}"
    a = np.asarray(Image.open(gt).convert("RGB"), dtype=np.float32)
    b = np.asarray(Image.open(init).convert("RGB"), dtype=np.float32)
    mse = float(np.mean((a - b) ** 2))
    print(f"GT vs init albedo map MSE={mse:.3f} shape={a.shape}")
    assert a.shape[0] >= 2 and a.shape[1] >= 2
    assert mse > 10.0, "truth and init maps must differ (spatial materials)"
    print("PASS test_map_apply_diff.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
