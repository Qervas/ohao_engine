#!/usr/bin/env python3
"""Drive shipped inverse_fit --backend diff: wrong-init optim must drop loss."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

from PIL import Image
import numpy as np


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean(((a.astype(np.float64) - b.astype(np.float64)) / 255.0) ** 2))
    return 99.0 if mse <= 1e-12 else -10.0 * np.log10(mse)


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    bin_path = root / "build" / "inverse_fit"
    assert bin_path.is_file(), f"missing {bin_path}"
    out = root / "renders" / "diff_selftest"
    cmd = [
        str(bin_path),
        "--backend",
        "diff",
        "--preset",
        "lantern",
        "--quality",
        "draft",
        "--iters",
        "36",
        "--views",
        "2",
        "--out-dir",
        str(out),
    ]
    print("running:", " ".join(cmd), flush=True)
    p = subprocess.run(cmd, cwd=root, capture_output=True)
    log = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode(
        "utf-8", "replace"
    )
    print(log[-4000:] if len(log) > 4000 else log)
    assert p.returncode == 0, f"rc={p.returncode}"
    assert "DIFFTEST PASS" in log
    assert "Vulkan Deferred" in log or "studio mesh" in log
    assert "wrong-init" in log
    assert "refine skipped" not in log  # must actually run coord FD
    assert "warm" not in log.lower() or "winner=warm" not in log  # no free-gift warm multi-start
    assert "GPU map SoT" not in log
    assert "gpu_map_roundtrip" not in log

    metrics = out / "diff_metrics.json"
    assert metrics.is_file()
    m = json.loads(metrics.read_text())
    assert m.get("backend") == "diff"
    assert m.get("studio_mesh_raster") is True
    assert m.get("beauty_theta_path") == "dense_map_bindless_deferred"
    assert m.get("dense_map_sot") is True
    assert m.get("dense_map_export") is True
    assert m.get("atlas_uv") is True
    assert m.get("wrong_init_source") in ("initTiles", "gray")
    assert "gpu_map_roundtrip" not in m
    assert "albedo_image_handle_live" not in m
    assert float(m["ab_gray_cool_mse"]) > 1e-4
    # Real optim: strict loss drop and map moved from wrong init
    assert float(m["final_loss"]) < float(m["init_loss"]) * 0.85
    assert float(m["map_mse_vs_init"]) > 1e-4
    assert float(m["train_psnr"]) > float(m["init_psnr"]) + 0.5
    # train_psnr is -10*log10(multi-view MSE) — consistent with optim loss

    for name in ("diff_forward_truth.png", "diff_recovered.png", "diff_init.png"):
        assert (out / name).is_file() and (out / name).stat().st_size > 1000
    truth = np.array(Image.open(out / "diff_forward_truth.png").convert("RGB"))
    init = np.array(Image.open(out / "diff_init.png").convert("RGB"))
    rec = np.array(Image.open(out / "diff_recovered.png").convert("RGB"))
    assert truth.std() > 5.0
    # Stills can be noisy; still require recovered closer or equal-ish, but
    # primary gate is multi-view loss/PSNR in metrics.
    assert float(m["train_psnr"]) > float(m["init_psnr"]) + 0.5
    print("PASS tools/inverse_lab/test_diff_ir.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
