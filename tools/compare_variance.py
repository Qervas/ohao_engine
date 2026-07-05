#!/usr/bin/env python3
"""Compare two rendered images for noise/variance estimation.

Usage: python compare_variance.py reference.png candidate.png

Reports:
  - Global RMSE between images (lower = more similar)
  - Local variance (per-pixel high-frequency energy, a noise proxy)
  - Noise reduction percentage (candidate vs reference)
  - Side-by-side absolute-difference image written to candidate.diff.png
"""
import sys
import numpy as np
from PIL import Image

def load(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0

def local_variance(img, k=5):
    """Local variance via 5x5 box filter (noise proxy)."""
    try:
        from scipy.ndimage import uniform_filter
        mean = uniform_filter(img, size=(k, k, 1))
        sq_mean = uniform_filter(img * img, size=(k, k, 1))
        return np.maximum(sq_mean - mean * mean, 0.0)
    except ImportError:
        # Fallback: manual 5x5 box filter (slow but no scipy)
        pad = k // 2
        padded = np.pad(img, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
        mean = np.zeros_like(img)
        sq_mean = np.zeros_like(img)
        for dy in range(-pad, pad + 1):
            for dx in range(-pad, pad + 1):
                shifted = padded[pad + dy:pad + dy + img.shape[0],
                                 pad + dx:pad + dx + img.shape[1]]
                mean += shifted
                sq_mean += shifted * shifted
        mean /= k * k
        sq_mean /= k * k
        return np.maximum(sq_mean - mean * mean, 0.0)

def main():
    if len(sys.argv) != 3:
        print("Usage: compare_variance.py reference.png candidate.png")
        sys.exit(1)

    ref = load(sys.argv[1])
    cand = load(sys.argv[2])
    if ref.shape != cand.shape:
        print(f"Shape mismatch: {ref.shape} vs {cand.shape}")
        sys.exit(1)

    rmse = float(np.sqrt(np.mean((ref - cand) ** 2)))
    var_ref = float(local_variance(ref).mean())
    var_cand = float(local_variance(cand).mean())
    reduction = (1.0 - var_cand / max(var_ref, 1e-9)) * 100.0

    print(f"Global RMSE:           {rmse:.6f}")
    print(f"Local variance (ref):  {var_ref:.6f}")
    print(f"Local variance (cand): {var_cand:.6f}")
    print(f"Noise reduction:       {reduction:+.1f}%")

    diff = np.abs(ref - cand)
    diff_max = max(diff.max(), 1e-9)
    diff8 = (diff / diff_max * 255).astype(np.uint8)
    diff_path = sys.argv[2].replace(".png", ".diff.png")
    Image.fromarray(diff8).save(diff_path)
    print(f"Diff written:          {diff_path}")

if __name__ == "__main__":
    main()
