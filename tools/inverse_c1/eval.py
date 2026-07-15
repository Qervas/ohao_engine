#!/usr/bin/env python3
"""Evaluate C1 θ prior quality (per-dim MAE, RGB MAE, param RMSE).

Example:
  python3 tools/inverse_c1/eval.py \\
      --model tools/inverse_c1/checkpoints/L2/best.pt \\
      --data renders/inverse_c1_ladder/L2/dataset
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import DataLoader

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from dataset import InverseThetaDataset, load_config, load_records  # noqa: E402
from model import ThetaPriorNet  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description="Eval OHAO inverse C1 prior")
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    ap.add_argument("--out", type=Path, default=None, help="optional metrics JSON path")
    ap.add_argument("--max-samples", type=int, default=0, help="0 = all")
    args = ap.parse_args()

    if not args.model.is_file():
        print(f"ERROR: missing model {args.model}", file=sys.stderr)
        return 2
    if not (args.data / "meta.jsonl").is_file():
        print(f"ERROR: missing dataset {args.data}", file=sys.stderr)
        return 2

    device = torch.device(args.device)
    ckpt = torch.load(args.model, map_location=device, weights_only=False)
    dims = int(ckpt.get("dims", 12))
    names = list(ckpt.get("names") or [f"dim{i}" for i in range(dims)])
    wh = ckpt.get("image_size") or [96, 54]
    w, h = int(wh[0]), int(wh[1])

    model = ThetaPriorNet(theta_dims=dims).to(device)
    model.load_state_dict(ckpt["model"])
    model.eval()

    records = load_records(args.data)
    if args.max_samples > 0:
        records = records[: args.max_samples]
    # Held-out eval (same split seed as train) so metrics are not train-set optimistic
    import random as _random

    idx = list(range(len(records)))
    _random.Random(42).shuffle(idx)
    n_val = max(1, int(len(records) * 0.15)) if len(records) > 2 else len(records)
    n_val = min(n_val, len(records) - 1) if len(records) > 1 else len(records)
    val_i = set(idx[:n_val])
    records = [records[i] for i in idx if i in val_i] if n_val < len(records) else records
    ds = InverseThetaDataset(args.data, records, image_size=(w, h), augment=False)
    loader = DataLoader(ds, batch_size=args.batch, shuffle=False, num_workers=0)

    abs_sum = np.zeros(dims, dtype=np.float64)
    sq_sum = 0.0
    n = 0
    with torch.no_grad():
        for batch in loader:
            x = batch["image"].to(device)
            y = batch["theta"].cpu().numpy()
            pred = model(x).cpu().numpy()
            # clamp rough
            for i, name in enumerate(names):
                if i >= pred.shape[1]:
                    break
                lo, hi = 0.0, 1.0
                if "rough" in name.lower():
                    lo = 0.04
                if "key" in name.lower() or "fill" in name.lower() or "rim" in name.lower():
                    lo, hi = 0.06, 0.95
                if "env" in name.lower():
                    lo, hi = 0.30, 1.50
                pred[:, i] = np.clip(pred[:, i], lo, hi)
            d = pred - y
            abs_sum += np.abs(d).sum(axis=0)
            sq_sum += float((d * d).sum())
            n += y.shape[0]

    mae = abs_sum / max(1, n)
    rmse = float(np.sqrt(sq_sum / max(1, n * dims)))
    rgb_mae = float(mae[:3].mean()) if dims >= 3 else float(mae.mean())
    metal_i = next((i for i, nm in enumerate(names) if "metal" in nm.lower()), None)
    rough_i = next((i for i, nm in enumerate(names) if "rough" in nm.lower()), None)

    metrics = {
        "samples": n,
        "dims": dims,
        "param_rmse": rmse,
        "rgb_mae": rgb_mae,
        "rough_mae": float(mae[rough_i]) if rough_i is not None else None,
        "metal_mae": float(mae[metal_i]) if metal_i is not None else None,
        "per_dim_mae": {names[i]: float(mae[i]) for i in range(min(dims, len(names)))},
        "model": str(args.model),
        "data": str(args.data),
        "level": load_config(args.data).get("level"),
    }

    print(f"=== C1 eval ({n} samples) ===")
    print(f"  level={metrics.get('level')}  dims={dims}")
    print(f"  RGB MAE     = {rgb_mae:.4f}   (lower is better; <0.12 strong, <0.18 ok)")
    print(f"  param RMSE  = {rmse:.4f}")
    if rough_i is not None:
        print(f"  rough MAE   = {mae[rough_i]:.4f}")
    if metal_i is not None:
        print(f"  metal MAE   = {mae[metal_i]:.4f}")
    print("  per-dim MAE:")
    for nm, v in metrics["per_dim_mae"].items():
        print(f"    {nm:18s} {v:.4f}")

    # Quality gate summary
    gates = {
        "rgb_ok": rgb_mae < 0.18,
        "rmse_ok": rmse < 0.28,
    }
    metrics["gates"] = gates
    print(
        f"  GATES: RGB {'PASS' if gates['rgb_ok'] else 'WEAK'}  "
        f"RMSE {'PASS' if gates['rmse_ok'] else 'WEAK'}"
    )

    out = args.out or (args.model.parent / "eval_metrics.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(metrics, f, indent=2)
    print(f"  wrote {out}")
    return 0 if gates["rgb_ok"] and gates["rmse_ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
