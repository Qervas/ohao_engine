#!/usr/bin/env python3
"""Build before/after comparison sheets for inverse_fit outputs.

Layout (primary sheet):
  [ Target | Init (wrong) | Recovered | |Diff|×scale ]

Also writes multi-view strip if target_*/recovered_* exist.

Usage:
  python3 tools/inverse_c1/make_compare.py renders/inverse_c1_compare/lantern_nn
  python3 tools/inverse_c1/make_compare.py --dir A --dir B --out renders/compare_all
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def to_u8(img: np.ndarray) -> np.ndarray:
    return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)


def resize_match(img: np.ndarray, hw: tuple[int, int]) -> np.ndarray:
    h, w = hw
    if img.shape[0] == h and img.shape[1] == w:
        return img
    pil = Image.fromarray(to_u8(img))
    pil = pil.resize((w, h), Image.BILINEAR)
    return np.asarray(pil, dtype=np.float32) / 255.0


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    d = a - b
    return float(np.sqrt(np.mean(d * d)))


def absdiff_vis(a: np.ndarray, b: np.ndarray, gain: float = 4.0) -> np.ndarray:
    """|diff| amplified and mapped to hot colors (green=ok, red=bad)."""
    d = np.abs(a - b).mean(axis=2)  # HxW
    d = np.clip(d * gain, 0.0, 1.0)
    # black → yellow → red
    out = np.zeros((*d.shape, 3), dtype=np.float32)
    out[..., 0] = np.clip(d * 1.4, 0, 1)  # R
    out[..., 1] = np.clip(d * 0.9, 0, 1)  # G
    out[..., 2] = np.clip(d * 0.15, 0, 1)
    return out


def try_font(size: int):
    for name in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    ):
        p = Path(name)
        if p.is_file():
            return ImageFont.truetype(str(p), size=size)
    return ImageFont.load_default()


def label_panel(img: np.ndarray, title: str, subtitle: str = "") -> Image.Image:
    """Add a title bar above the image."""
    h, w = img.shape[:2]
    bar_h = 48 if subtitle else 36
    canvas = Image.new("RGB", (w, h + bar_h), (18, 18, 22))
    canvas.paste(Image.fromarray(to_u8(img)), (0, bar_h))
    draw = ImageDraw.Draw(canvas)
    font = try_font(18)
    font_s = try_font(14)
    draw.text((10, 8), title, fill=(240, 240, 245), font=font)
    if subtitle:
        draw.text((10, 28), subtitle, fill=(160, 170, 190), font=font_s)
    return canvas


def hstack_pil(panels: list[Image.Image], gap: int = 8, bg=(12, 12, 16)) -> Image.Image:
    h = max(p.height for p in panels)
    w = sum(p.width for p in panels) + gap * (len(panels) - 1)
    out = Image.new("RGB", (w, h), bg)
    x = 0
    for p in panels:
        y = (h - p.height) // 2
        out.paste(p, (x, y))
        x += p.width + gap
    return out


def vstack_pil(panels: list[Image.Image], gap: int = 10, bg=(12, 12, 16)) -> Image.Image:
    w = max(p.width for p in panels)
    h = sum(p.height for p in panels) + gap * (len(panels) - 1)
    out = Image.new("RGB", (w, h), bg)
    y = 0
    for p in panels:
        x = (w - p.width) // 2
        out.paste(p, (x, y))
        y += p.height + gap
    return out


def find_image(d: Path, names: list[str]) -> Path | None:
    for n in names:
        p = d / n
        if p.is_file():
            return p
    return None


def build_dir_compare(d: Path, out_dir: Path | None = None) -> dict:
    d = Path(d)
    out_dir = Path(out_dir) if out_dir else d
    out_dir.mkdir(parents=True, exist_ok=True)

    target_p = find_image(d, ["target_show.png", "target_front.png", "target.png"])
    # Prefer true wrong-init for BEFORE; fall back to init_show
    wrong_p = find_image(d, ["init_wrong_show.png", "init_show.png", "init.png", "init_s.png"])
    nn_p = find_image(d, ["nn_prior_show.png", "init_opt_start_show.png"])
    rec_p = find_image(d, ["recovered_show.png", "recovered_front.png", "recovered.png"])

    if not target_p or not rec_p:
        raise FileNotFoundError(f"Need target_* and recovered_* in {d}")

    target = load_rgb(target_p)
    rec = resize_match(load_rgb(rec_p), target.shape[:2])
    wrong = resize_match(load_rgb(wrong_p), target.shape[:2]) if wrong_p else None
    nn_img = resize_match(load_rgb(nn_p), target.shape[:2]) if nn_p else None

    r_wrong = rmse(wrong, target) if wrong is not None else float("nan")
    r_nn = rmse(nn_img, target) if nn_img is not None else float("nan")
    r_rec = rmse(rec, target)
    diff = absdiff_vis(rec, target, gain=5.0)

    panels = [
        label_panel(target, "TARGET (truth)", str(target_p.name)),
    ]
    if wrong is not None:
        panels.append(
            label_panel(wrong, "BEFORE (wrong init)", f"RMSE vs target = {r_wrong:.4f}")
        )
    if nn_img is not None:
        panels.append(
            label_panel(nn_img, "NN PRIOR (seed)", f"RMSE vs target = {r_nn:.4f}")
        )
    panels.append(
        label_panel(rec, "AFTER (recovered)", f"RMSE vs target = {r_rec:.4f}")
    )
    panels.append(
        label_panel(diff, "|DIFF| ×5 (error heat)", "dark = match, red/yellow = error")
    )

    sheet = hstack_pil(panels)
    # Footer banner
    footer_h = 40
    full = Image.new("RGB", (sheet.width, sheet.height + footer_h), (12, 12, 16))
    full.paste(sheet, (0, 0))
    draw = ImageDraw.Draw(full)
    font = try_font(16)
    improve = ""
    base_r = r_wrong if wrong is not None else r_nn
    if base_r == base_r and base_r > 1e-8:
        pct = 100.0 * (1.0 - r_rec / base_r)
        improve = f"  |  RMSE improvement: {pct:+.1f}%"
    nn_s = f"  nn={r_nn:.4f}" if nn_img is not None else ""
    draw.text(
        (12, sheet.height + 10),
        f"{d.name}  |  before RMSE={base_r:.4f}{nn_s}  after RMSE={r_rec:.4f}{improve}",
        fill=(200, 210, 230),
        font=font,
    )

    primary_out = out_dir / "compare_before_after.png"
    full.save(primary_out, quality=95)
    print(f"wrote {primary_out}")

    # Multi-view strip
    views = []
    for name in ("front", "three_quarter", "opposite", "leftish", "rightish"):
        t = d / f"target_{name}.png"
        r = d / f"recovered_{name}.png"
        if t.is_file() and r.is_file():
            views.append(name)
    if views:
        rows = []
        for name in views:
            t = load_rgb(d / f"target_{name}.png")
            r = resize_match(load_rgb(d / f"recovered_{name}.png"), t.shape[:2])
            df = absdiff_vis(r, t, gain=5.0)
            row = hstack_pil(
                [
                    label_panel(t, f"{name} TARGET"),
                    label_panel(r, f"{name} RECOVERED", f"RMSE={rmse(r, t):.4f}"),
                    label_panel(df, f"{name} |DIFF|×5"),
                ]
            )
            rows.append(row)
        multi = vstack_pil(rows)
        multi_out = out_dir / "compare_multiview.png"
        multi.save(multi_out, quality=95)
        print(f"wrote {multi_out}")

    # Side-by-side only target|recovered (simple)
    simple = hstack_pil(
        [
            label_panel(target, "TARGET"),
            label_panel(rec, "RECOVERED", f"RMSE={r_rec:.4f}"),
        ]
    )
    simple_out = out_dir / "compare_target_recovered.png"
    simple.save(simple_out, quality=95)
    print(f"wrote {simple_out}")

    meta = {
        "dir": str(d),
        "rmse_before": r_wrong,
        "rmse_nn_prior": r_nn,
        "rmse_recovered": r_rec,
        "outputs": [str(primary_out), str(simple_out)],
    }
    with open(out_dir / "compare_metrics.json", "w") as f:
        json.dump(meta, f, indent=2)
    return meta


def main() -> int:
    ap = argparse.ArgumentParser(description="Inverse target vs recovered comparison sheets")
    ap.add_argument("dirs", nargs="*", type=Path, help="inverse_fit out-dirs")
    ap.add_argument("--dir", action="append", dest="dirs_flag", type=Path, default=[])
    ap.add_argument("--out", type=Path, default=None, help="optional shared output root")
    args = ap.parse_args()
    dirs = list(args.dirs) + list(args.dirs_flag or [])
    if not dirs:
        # Defaults: recent hybrid compare runs
        candidates = [
            Path("renders/inverse_c1_compare/lantern_baseline"),
            Path("renders/inverse_c1_compare/lantern_nn"),
        ]
        dirs = [c for c in candidates if c.is_dir()]
    if not dirs:
        print("No dirs given and no default compare runs found.", file=sys.stderr)
        return 2

    all_meta = []
    for d in dirs:
        try:
            out = args.out / d.name if args.out else None
            all_meta.append(build_dir_compare(d, out))
        except Exception as e:
            print(f"SKIP {d}: {e}", file=sys.stderr)

    # Combined baseline vs NN sheet if both present
    base = Path("renders/inverse_c1_compare/lantern_baseline")
    nn = Path("renders/inverse_c1_compare/lantern_nn")
    if base.is_dir() and nn.is_dir():
        try:
            t = load_rgb(find_image(base, ["target_show.png", "target_front.png"]))
            b_init = resize_match(
                load_rgb(find_image(base, ["init_show.png"])), t.shape[:2]
            )
            b_rec = resize_match(
                load_rgb(find_image(base, ["recovered_show.png", "recovered_front.png"])),
                t.shape[:2],
            )
            n_rec = resize_match(
                load_rgb(find_image(nn, ["recovered_show.png", "recovered_front.png"])),
                t.shape[:2],
            )
            sheet = hstack_pil(
                [
                    label_panel(t, "TARGET (truth)"),
                    label_panel(b_init, "BEFORE (bad init)", f"RMSE={rmse(b_init, t):.4f}"),
                    label_panel(
                        b_rec,
                        "AFTER baseline FD only",
                        f"RMSE={rmse(b_rec, t):.4f}",
                    ),
                    label_panel(
                        n_rec,
                        "AFTER NN prior + soft FD",
                        f"RMSE={rmse(n_rec, t):.4f}",
                    ),
                ]
            )
            outp = Path("renders/inverse_c1_compare/COMPARE_baseline_vs_nn.png")
            outp.parent.mkdir(parents=True, exist_ok=True)
            sheet.save(outp, quality=95)
            print(f"wrote {outp}")
            print(
                f"  baseline recovered RMSE={rmse(b_rec, t):.4f}  "
                f"NN hybrid RMSE={rmse(n_rec, t):.4f}"
            )
        except Exception as e:
            print(f"combined sheet skipped: {e}", file=sys.stderr)

    print("Done. Open the compare_*.png files to inspect.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
