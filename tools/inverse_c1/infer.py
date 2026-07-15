#!/usr/bin/env python3
"""Run C1 θ prior on a single FIT/SHOW image → theta.json for inverse_fit.

Example:
  python3 tools/inverse_c1/infer.py \\
      --model tools/inverse_c1/checkpoints/best.pt \\
      --image renders/inverse/target_show.png \\
      --out renders/inverse/theta_prior.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
from PIL import Image

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from model import ThetaPriorNet  # noqa: E402


def load_image(path: Path, size_wh: tuple[int, int]) -> torch.Tensor:
    img = Image.open(path).convert("RGB")
    img = img.resize(size_wh, Image.BILINEAR)
    arr = np.asarray(img, dtype=np.float32) / 255.0
    t = torch.from_numpy(arr).permute(2, 0, 1).unsqueeze(0)  # 1,3,H,W
    return t


def main() -> int:
    ap = argparse.ArgumentParser(description="Infer θ prior from image (C1)")
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    args = ap.parse_args()

    if not args.model.is_file():
        print(f"ERROR: model not found: {args.model}", file=sys.stderr)
        return 2
    if not args.image.is_file():
        print(f"ERROR: image not found: {args.image}", file=sys.stderr)
        return 2

    device = torch.device(args.device)
    ckpt = torch.load(args.model, map_location=device, weights_only=False)
    dims = int(ckpt.get("dims", 12))
    names = ckpt.get("names") or [f"dim{i}" for i in range(dims)]
    wh = ckpt.get("image_size") or [96, 54]
    w, h = int(wh[0]), int(wh[1])

    model = ThetaPriorNet(theta_dims=dims).to(device)
    model.load_state_dict(ckpt["model"])
    model.eval()

    x = load_image(args.image, (w, h)).to(device)
    with torch.no_grad():
        pred = model(x).squeeze(0).cpu().numpy().astype(np.float64)

    # Soft clamp into typical box (rough lower bound 0.04)
    for i, n in enumerate(names):
        if i >= len(pred):
            break
        lo, hi = 0.0, 1.0
        if "rough" in n.lower():
            lo = 0.04
        if "key" in n.lower() or "fill" in n.lower() or "rim" in n.lower():
            lo, hi = 0.06, 0.95
        if "env" in n.lower():
            lo, hi = 0.30, 1.50
        pred[i] = float(np.clip(pred[i], lo, hi))

    out = {
        "format": "ohao_inverse_c1_theta",
        "version": 1,
        "model": str(args.model),
        "image": str(args.image),
        "dims": dims,
        "names": names,
        "theta": pred.tolist(),
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    print(f"wrote {args.out}")
    print(f"  dims={dims}")
    print(f"  theta={pred.tolist()}")
    # Pretty per-name
    for n, v in zip(names, pred.tolist()):
        print(f"    {n:18s} {v:.4f}")
    print(f"\nFD refine:\n  ./build/inverse_fit --selftest --theta-init {args.out} --no-multi-start --quality draft")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
