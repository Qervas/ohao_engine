"""OHAO inverse C1 dataset loader.

Reads exports from:
  ./build/inverse_fit --export-dataset N --out-dir DIR
→ DIR/dataset/{config.json, meta.jsonl, 00000.png, ...}
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image
import torch
from torch.utils.data import Dataset


def load_config(data_dir: Path) -> dict[str, Any]:
    cfg_path = data_dir / "config.json"
    if cfg_path.is_file():
        with open(cfg_path, encoding="utf-8") as f:
            return json.load(f)
    # Fallback: parse header line of meta.jsonl
    meta = data_dir / "meta.jsonl"
    with open(meta, encoding="utf-8") as f:
        header = json.loads(f.readline())
    dims = int(header.get("dims", 12))
    fit = header.get("fit", [384, 216, 32])
    return {
        "format": "ohao_inverse_c1",
        "version": 1,
        "dims": dims,
        "preset": header.get("preset", "unknown"),
        "scene": header.get("scene", "studio"),
        "map_ground": header.get("map_ground", False),
        "fit": {"width": fit[0], "height": fit[1], "spp": fit[2] if len(fit) > 2 else 32},
        "names": [f"dim{i}" for i in range(dims)],
    }


def load_records(data_dir: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with open(data_dir / "meta.jsonl", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            if row.get("type") == "header":
                continue
            if "file" in row and "theta" in row:
                records.append(row)
    return records


def loss_weights(names: list[str]) -> np.ndarray:
    """Up-weight albedo / color dims so the net prioritizes correct color."""
    w = np.ones(len(names), dtype=np.float32)
    for i, n in enumerate(names):
        nl = n.lower()
        is_rgb = (
            nl.endswith(".r")
            or nl.endswith(".g")
            or nl.endswith(".b")
            or nl in ("r", "g", "b")
            or "albedo" in nl
        )
        if is_rgb and "rough" not in nl and "metal" not in nl:
            w[i] = 4.0  # primary + pedestal color
        elif "rough" in nl:
            w[i] = 1.5
        elif "metal" in nl:
            w[i] = 2.0
        elif "env" in nl or "key" in nl:
            w[i] = 1.2
    # Extra boost on first three dims (primary albedo) when present
    if len(w) >= 3:
        w[0] = max(w[0], 5.0)
        w[1] = max(w[1], 5.0)
        w[2] = max(w[2], 5.0)
    return w


class InverseThetaDataset(Dataset):
    def __init__(
        self,
        data_dir: str | Path,
        records: list[dict[str, Any]] | None = None,
        image_size: tuple[int, int] = (96, 54),  # (W, H) — landscape FIT-ish
        augment: bool = False,
    ):
        self.data_dir = Path(data_dir)
        self.cfg = load_config(self.data_dir)
        self.records = records if records is not None else load_records(self.data_dir)
        self.image_size = image_size  # W, H
        self.augment = augment
        self.dims = int(self.cfg["dims"])
        self.names = list(self.cfg.get("names") or [f"dim{i}" for i in range(self.dims)])

    def __len__(self) -> int:
        return len(self.records)

    def __getitem__(self, idx: int) -> dict[str, torch.Tensor]:
        row = self.records[idx]
        path = self.data_dir / row["file"]
        img = Image.open(path).convert("RGB")
        w, h = self.image_size
        img = img.resize((w, h), Image.BILINEAR)
        arr = np.asarray(img, dtype=np.float32) / 255.0  # H,W,3
        if self.augment:
            if np.random.rand() < 0.5:
                arr = np.flip(arr, axis=1).copy()
            # Mild brightness jitter (simulates exposure mismatch)
            gain = float(np.random.uniform(0.9, 1.1))
            arr = np.clip(arr * gain, 0.0, 1.0)
        # CHW
        x = torch.from_numpy(arr).permute(2, 0, 1).contiguous()
        th = np.asarray(row["theta"], dtype=np.float32)
        if th.shape[0] != self.dims:
            # pad / truncate defensively
            t2 = np.zeros(self.dims, dtype=np.float32)
            n = min(self.dims, th.shape[0])
            t2[:n] = th[:n]
            th = t2
        y = torch.from_numpy(th)
        return {"image": x, "theta": y, "file": row["file"]}
