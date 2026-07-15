#!/usr/bin/env python3
"""Train C1 θ-prior network on synthetic inverse_fit exports.

Example:
  python3 tools/inverse_c1/train.py \\
      --data renders/inverse_c1_data/dataset \\
      --out tools/inverse_c1/checkpoints \\
      --epochs 40 --batch 32
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader

# Allow running as script from repo root or this dir
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from dataset import InverseThetaDataset, load_config, load_records, loss_weights  # noqa: E402
from model import ThetaPriorNet  # noqa: E402


def set_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


def split_records(records: list, val_frac: float, seed: int):
    idx = list(range(len(records)))
    rng = random.Random(seed)
    rng.shuffle(idx)
    n_val = max(1, int(len(records) * val_frac)) if len(records) > 2 else 1
    n_val = min(n_val, len(records) - 1) if len(records) > 1 else 0
    val_i = set(idx[:n_val])
    train = [records[i] for i in idx if i not in val_i]
    val = [records[i] for i in idx if i in val_i]
    return train, val


def main() -> int:
    ap = argparse.ArgumentParser(description="Train OHAO inverse C1 θ prior")
    ap.add_argument("--data", type=Path, required=True, help="dataset dir with meta.jsonl")
    ap.add_argument("--out", type=Path, default=Path("tools/inverse_c1/checkpoints"))
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--val-frac", type=float, default=0.15)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    ap.add_argument("--width", type=int, default=128, help="net input width (quality: 128)")
    ap.add_argument("--height", type=int, default=72, help="net input height (quality: 72)")
    args = ap.parse_args()

    data_dir = args.data
    if not (data_dir / "meta.jsonl").is_file():
        print(f"ERROR: {data_dir}/meta.jsonl not found", file=sys.stderr)
        return 2

    set_seed(args.seed)
    cfg = load_config(data_dir)
    records = load_records(data_dir)
    if len(records) < 4:
        print(f"ERROR: need ≥4 samples, got {len(records)}", file=sys.stderr)
        return 2

    train_rec, val_rec = split_records(records, args.val_frac, args.seed)
    img_size = (args.width, args.height)
    train_ds = InverseThetaDataset(data_dir, train_rec, image_size=img_size, augment=True)
    val_ds = InverseThetaDataset(data_dir, val_rec, image_size=img_size, augment=False)

    train_loader = DataLoader(train_ds, batch_size=args.batch, shuffle=True, num_workers=0)
    val_loader = DataLoader(val_ds, batch_size=args.batch, shuffle=False, num_workers=0)

    device = torch.device(args.device)
    dims = int(cfg["dims"])
    names = list(cfg.get("names") or [f"dim{i}" for i in range(dims)])
    w = torch.from_numpy(loss_weights(names)).to(device)

    model = ThetaPriorNet(theta_dims=dims).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=max(1, args.epochs))

    args.out.mkdir(parents=True, exist_ok=True)
    best_val = float("inf")
    history: list[dict] = []

    print(f"C1 train: {len(train_rec)} train / {len(val_rec)} val  dims={dims}  device={device}")
    print(f"  names={names}")
    print(f"  weights={w.detach().cpu().tolist()}")

    for epoch in range(1, args.epochs + 1):
        model.train()
        tr_loss = 0.0
        tr_n = 0
        for batch in train_loader:
            x = batch["image"].to(device)
            y = batch["theta"].to(device)
            pred = model(x)
            # Weighted MSE + light MAE on albedo-ish dims
            err = (pred - y) ** 2
            loss = (err * w).mean()
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            opt.step()
            tr_loss += float(loss.item()) * x.size(0)
            tr_n += x.size(0)
        sched.step()

        model.eval()
        va_loss = 0.0
        va_n = 0
        va_abs = torch.zeros(dims, device=device)
        with torch.no_grad():
            for batch in val_loader:
                x = batch["image"].to(device)
                y = batch["theta"].to(device)
                pred = model(x)
                err = (pred - y) ** 2
                loss = (err * w).mean()
                va_loss += float(loss.item()) * x.size(0)
                va_n += x.size(0)
                va_abs += (pred - y).abs().sum(dim=0)

        tr = tr_loss / max(1, tr_n)
        va = va_loss / max(1, va_n)
        mae = (va_abs / max(1, va_n)).detach().cpu().numpy()
        # Report primary RGB MAE if present
        rgb_mae = float(mae[:3].mean()) if dims >= 3 else float(mae.mean())
        print(
            f"  epoch {epoch:03d}/{args.epochs}  train={tr:.5f}  val={va:.5f}  "
            f"RGB_MAE≈{rgb_mae:.4f}  lr={sched.get_last_lr()[0]:.2e}"
        )
        history.append({"epoch": epoch, "train": tr, "val": va, "rgb_mae": rgb_mae})

        if va < best_val:
            best_val = va
            ckpt = {
                "model": model.state_dict(),
                "dims": dims,
                "names": names,
                "config": cfg,
                "image_size": [args.width, args.height],
                "epoch": epoch,
                "val_loss": va,
            }
            path = args.out / "best.pt"
            torch.save(ckpt, path)
            # Side JSON meta for C++ / docs
            meta = {
                "checkpoint": str(path),
                "dims": dims,
                "names": names,
                "image_size": [args.width, args.height],
                "val_loss": va,
                "epoch": epoch,
                "preset": cfg.get("preset"),
                "format": "ohao_inverse_c1",
            }
            with open(args.out / "best_meta.json", "w", encoding="utf-8") as f:
                json.dump(meta, f, indent=2)
            print(f"    ✓ saved {path} (val={va:.5f})")

    with open(args.out / "history.json", "w", encoding="utf-8") as f:
        json.dump(history, f, indent=2)
    print(f"Done. Best val={best_val:.5f} → {args.out / 'best.pt'}")
    print("Infer: python3 tools/inverse_c1/infer.py --model .../best.pt --image target.png --out theta.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
