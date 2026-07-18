#!/usr/bin/env python3
"""Build a photo_proxy capture bundle from a synthetic lab export.

Applies mild domain shift (exposure, noise, JPEG recompression) so fit targets
are not pure sim-to-sim theater. Labels capture_kind=photo_proxy honestly.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def _load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def _save_rgb(arr: np.ndarray, path: Path, jpeg_q: int | None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    u8 = np.clip(arr * 255.0 + 0.5, 0, 255).astype(np.uint8)
    im = Image.fromarray(u8, mode="RGB")
    if jpeg_q is not None and 1 <= jpeg_q <= 100:
        # Recompress via JPEG then re-save PNG (simulates camera pipeline).
        import io

        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=jpeg_q)
        buf.seek(0)
        im = Image.open(buf).convert("RGB")
    im.save(path)


def degrade(rgb: np.ndarray, exposure: float, noise: float, rng: np.random.Generator) -> np.ndarray:
    out = np.clip(rgb * exposure, 0.0, 1.0)
    if noise > 0:
        out = out + rng.normal(0.0, noise, size=out.shape).astype(np.float32)
    return np.clip(out, 0.0, 1.0)


def process_tree(src: Path, dst: Path, exposure: float, noise: float, jpeg_q: int, seed: int) -> int:
    if not (src / "capture.json").is_file():
        print(f"FAIL missing {src / 'capture.json'}")
        return 1
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)

    # Copy non-image structure
    for name in ("cameras.jsonl", "theta_gt.json"):
        p = src / name
        if p.is_file():
            shutil.copy2(p, dst / name)
    mat_src = src / "materials"
    if mat_src.is_dir():
        shutil.copytree(mat_src, dst / "materials")

    man = json.loads((src / "capture.json").read_text())
    man["capture_kind"] = "photo_proxy"
    man["metric_domain"] = "photo_proxy_images"
    man["source_capture"] = str(src.resolve())
    man["domain_shift"] = {
        "exposure": exposure,
        "noise_sigma": noise,
        "jpeg_quality": jpeg_q,
        "seed": seed,
        "notes": "Synthetic export + mild camera-like degradation; not a real photo.",
    }
    man["notes"] = (
        (man.get("notes") or "")
        + " PHOTO_PROXY: domain-shifted images for H3 plate; do not apply synthetic LABTEST ≥28."
    )
    (dst / "capture.json").write_text(json.dumps(man, indent=2) + "\n")

    rng = np.random.default_rng(seed)
    n_img = 0
    for sub in ("images", "relight"):
        sdir = src / sub
        if not sdir.is_dir():
            continue
        ddir = dst / sub
        ddir.mkdir(parents=True, exist_ok=True)
        for p in sorted(sdir.glob("*.png")) + sorted(sdir.glob("*.jpg")):
            rgb = _load_rgb(p)
            out = degrade(rgb, exposure, noise, rng)
            _save_rgb(out, ddir / (p.stem + ".png"), jpeg_q)
            n_img += 1

    # Human-readable notes
    (dst / "NOTES.md").write_text(
        "\n".join(
            [
                "# Photo proxy capture",
                "",
                f"- Source: `{src}`",
                f"- Domain shift: exposure={exposure}, noise={noise}, jpeg_q={jpeg_q}",
                f"- Images processed: {n_img}",
                "",
                "This is **not** a real phone capture. Use for H3 pipeline + PHOTOTEST.",
                "Real shoots: see docs/inverse_photo_lab.md (COLMAP / board).",
                "",
            ]
        )
    )
    print(f"photo_proxy → {dst}  images={n_img}  exposure={exposure} noise={noise} jpeg_q={jpeg_q}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src_capture", type=Path, help="Synthetic lab capture/ directory")
    ap.add_argument("dst_capture", type=Path, help="Output photo_proxy capture/ directory")
    ap.add_argument("--exposure", type=float, default=0.88, help="Linear exposure scale")
    ap.add_argument("--noise", type=float, default=0.015, help="Gaussian noise sigma in linear")
    ap.add_argument("--jpeg-q", type=int, default=88, help="JPEG quality for recompress (1-100)")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    return process_tree(args.src_capture, args.dst_capture, args.exposure, args.noise, args.jpeg_q, args.seed)


if __name__ == "__main__":
    raise SystemExit(main())
