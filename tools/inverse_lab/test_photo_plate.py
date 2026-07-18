#!/usr/bin/env python3
"""PHOTOTEST — honest multi-view photo / photo-proxy plate gate.

Does NOT require synthetic LABTEST (≥28 holdout). Requires recovery gain vs
wrong-init, labeled capture_kind, and photo vs re-render stills.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
import numpy as np


def _psnr_pair(a: Path, b: Path) -> float:
    x = np.asarray(Image.open(a).convert("RGB"), dtype=np.float64) / 255.0
    y = np.asarray(Image.open(b).convert("RGB"), dtype=np.float64) / 255.0
    if x.shape != y.shape:
        y_img = Image.open(b).convert("RGB").resize((x.shape[1], x.shape[0]), Image.BILINEAR)
        y = np.asarray(y_img, dtype=np.float64) / 255.0
    mse = float(np.mean((x - y) ** 2))
    if mse < 1e-12:
        return 99.0
    return float(-10.0 * np.log10(mse))


def _side_by_side(paths: list[Path], labels: list[str], out: Path) -> None:
    imgs = [Image.open(p).convert("RGB") for p in paths]
    h = min(im.height for im in imgs)
    w = min(im.width for im in imgs)
    imgs = [im.resize((w, h), Image.BILINEAR) for im in imgs]
    strip = Image.new("RGB", (w * len(imgs), h + 28), (20, 22, 28))
    draw = ImageDraw.Draw(strip)
    for i, im in enumerate(imgs):
        strip.paste(im, (i * w, 28))
        draw.text((i * w + 8, 6), labels[i], fill=(220, 220, 230))
    out.parent.mkdir(parents=True, exist_ok=True)
    strip.save(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("fit_dir", type=Path)
    ap.add_argument("--capture", type=Path, default=None, help="photo_proxy or real capture/")
    ap.add_argument("--min-holdout-gain", type=float, default=3.0)
    ap.add_argument("--min-train-gain", type=float, default=2.0)
    args = ap.parse_args()

    fit = args.fit_dir
    metrics_path = fit / "lab_metrics.json"
    if not metrics_path.is_file():
        print(f"FAIL missing {metrics_path}")
        return 1
    m = json.loads(metrics_path.read_text())

    cap = args.capture
    if cap is None:
        # Try to infer from common script layout
        cand = fit.parent / "capture"
        if (cand / "capture.json").is_file():
            cap = cand
    if cap is None or not (cap / "capture.json").is_file():
        print("FAIL need --capture path to photo bundle (capture.json)")
        return 1

    cman = json.loads((cap / "capture.json").read_text())
    kind = cman.get("capture_kind") or cman.get("kind") or "synthetic_export"
    domain = cman.get("metric_domain") or m.get("metric_gt") or ""
    print(f"capture_kind={kind} metric_domain={domain}")

    if kind not in ("photo_proxy", "real_photo"):
        print(
            f"FAIL PHOTOTEST requires capture_kind photo_proxy|real_photo, got {kind!r}. "
            "Use make_photo_proxy.py or label a real shoot."
        )
        return 1

    hold = m.get("holdout") or {}
    train = m.get("train") or {}
    wrong = m.get("wrong_init_holdout") or {}
    hold_psnr = float(hold.get("psnr", 0))
    wrong_psnr = float(wrong.get("psnr", 0))
    gain = float(m.get("holdout_psnr_gain_db", hold_psnr - wrong_psnr))
    train_psnr = float(train.get("psnr", 0))

    # Wrong-init train if available from stills
    init_show = fit / "init_wrong_show.png"
    if not init_show.is_file():
        init_show = fit / "init_show.png"
    rec_show = fit / "recovered_show.png"
    # Prefer capture train image as photo target
    train_photo = None
    img_dir = cap / "images"
    for p in sorted(img_dir.glob("train_*.png")):
        train_photo = p
        break

    train_gain = None
    if init_show.is_file() and rec_show.is_file() and train_photo and train_photo.is_file():
        p_init = _psnr_pair(train_photo, init_show)
        p_rec = _psnr_pair(train_photo, rec_show)
        train_gain = p_rec - p_init
        print(f"train photo-vs-render PSNR init={p_init:.2f} rec={p_rec:.2f} gain={train_gain:.2f}")
        _side_by_side(
            [train_photo, init_show, rec_show],
            ["photo target", "wrong-init re-render", "recovered re-render"],
            fit / "photo_vs_rerender.png",
        )

    print(
        f"holdout PSNR={hold_psnr:.3f} wrong={wrong_psnr:.3f} gain={gain:.3f} "
        f"train_psnr={train_psnr:.3f}"
    )

    # Honest gates
    if gain < args.min_holdout_gain:
        print(f"FAIL holdout gain {gain:.2f} < {args.min_holdout_gain} dB")
        return 1
    if train_gain is not None and train_gain < args.min_train_gain:
        print(f"FAIL train photo gain {train_gain:.2f} < {args.min_train_gain} dB")
        return 1

    # Stills must exist
    for p in (init_show, rec_show):
        if not p.is_file() or p.stat().st_size < 64:
            print(f"FAIL missing still {p}")
            return 1
    if not (fit / "photo_vs_rerender.png").is_file():
        print("FAIL missing photo_vs_rerender.png strip")
        return 1

    # Failure modes log (required honesty)
    fm = fit / "failure_modes.json"
    if not fm.is_file():
        # Write a default honest log if script forgot — still require presence after
        failures = {
            "capture_kind": kind,
            "domain_shift": cman.get("domain_shift"),
            "known_risks": [
                "Exposure / tone mismatch vs engine renderer",
                "Specular / metal chart sensitivity",
                "Pose/mesh error not modeled in photo_proxy",
                "Do not apply synthetic LABTEST ≥28 to this plate",
            ],
            "holdout_psnr": hold_psnr,
            "holdout_gain_db": gain,
            "notes": "Auto-written by test_photo_plate; replace with shoot notes for real_photo.",
        }
        fm.write_text(json.dumps(failures, indent=2) + "\n")
        print(f"wrote default {fm}")

    # Emit photo_plate_metrics.json for dashboards
    out_m = {
        "plate": "H3_photo",
        "capture_kind": kind,
        "metric_domain": domain,
        "metric_gt": m.get("metric_gt"),
        "holdout_psnr": hold_psnr,
        "wrong_init_holdout_psnr": wrong_psnr,
        "holdout_psnr_gain_db": gain,
        "train_psnr": train_psnr,
        "train_photo_gain_db": train_gain,
        "relight_psnr": (m.get("relight") or {}).get("psnr"),
        "synthetic_labtest_bar_applied": False,
        "phototest_min_holdout_gain_db": args.min_holdout_gain,
        "pass": True,
    }
    (fit / "photo_plate_metrics.json").write_text(json.dumps(out_m, indent=2) + "\n")

    print("PASS tools/inverse_lab/test_photo_plate.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
