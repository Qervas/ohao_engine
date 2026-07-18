#!/usr/bin/env python3
"""Assemble M3a multi-preset gallery summary + HTML wall from run dirs."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


def _load_metrics(d: Path) -> dict[str, Any] | None:
    for name in (
        "dense_orm_metrics.json",
        "dense_metal_metrics.json",
        "dense_map_metrics.json",
    ):
        p = d / name
        if p.is_file():
            m = json.loads(p.read_text())
            m["_metrics_file"] = name
            return m
    return None


def _pick_still(d: Path, stems: list[str]) -> str | None:
    """Prefer HD *_show.png when present."""
    for stem in stems:
        for cand in (d / f"{stem}_show.png", d / f"{stem}.png"):
            if cand.is_file() and cand.stat().st_size > 64:
                return cand.name
    return None


def _mode_from_metrics(m: dict[str, Any]) -> str:
    mode = str(m.get("mode", ""))
    if "orm" in mode:
        return "orm"
    if "metal" in mode:
        return "metal"
    if "map" in mode or "albedo" in mode:
        return "map"
    return mode or "unknown"


def _pass_from_metrics(m: dict[str, Any]) -> bool:
    """Replicate published MAPTEST spirit from metrics fields."""
    mode = _mode_from_metrics(m)
    if mode == "orm":
        mse_i = float(m.get("rough_mse_init", 1e9))
        mse_r = float(m.get("rough_mse_recovered", 1e9))
        d_psnr = float(m.get("train_psnr", 0)) - float(m.get("init_psnr", 0))
        d_rel = float(m.get("relight_recovered_psnr", 0)) - float(m.get("relight_init_psnr", 0))
        return mse_r < mse_i * 0.85 and d_psnr >= 2.0 and d_rel >= 1.5
    if mode == "metal":
        mse_i = float(m.get("metal_mse_init", 1e9))
        mse_r = float(m.get("metal_mse_recovered", 1e9))
        d_psnr = float(m.get("train_psnr", 0)) - float(m.get("init_psnr", 0))
        d_rel = float(m.get("relight_recovered_psnr", 0)) - float(m.get("relight_init_psnr", 0))
        return mse_r < mse_i * 0.85 and d_psnr >= 2.0 and d_rel >= 1.5
    if mode == "map":
        mse_i = float(m.get("map_mse_init", m.get("dense_map_mse_init", 1e9)))
        mse_r = float(m.get("map_mse_recovered", m.get("dense_map_mse_recovered", 1e9)))
        d_psnr = float(m.get("train_psnr", 0)) - float(m.get("init_psnr", 0))
        return mse_r < mse_i and d_psnr >= 2.0
    return False


def _parse_dir_name(name: str) -> tuple[str, str]:
    # e.g. lantern_orm_hd720, helmet_metal_256x144
    m = re.match(r"^([a-z0-9]+)_(orm|metal|map)_(.+)$", name, re.I)
    if m:
        return m.group(1).lower(), m.group(2).lower()
    return name, "unknown"


def collect_runs(root: Path) -> list[dict[str, Any]]:
    runs: list[dict[str, Any]] = []
    tsv = root / "gallery_runs.tsv"
    tsv_pass: dict[str, int] = {}
    if tsv.is_file():
        for line in tsv.read_text().splitlines()[1:]:
            parts = line.split("\t")
            if len(parts) >= 4:
                tsv_pass[parts[2]] = int(parts[3])

    for d in sorted(root.iterdir()):
        if not d.is_dir():
            continue
        metrics = _load_metrics(d)
        if not metrics:
            continue
        preset, mode = _parse_dir_name(d.name)
        if mode == "unknown":
            mode = _mode_from_metrics(metrics)
        if "preset" not in metrics:
            metrics["preset"] = preset

        init_stem = {
            "orm": ["orm_init"],
            "metal": ["metal_init"],
            "map": ["dense_init", "map_init"],
        }.get(mode, ["init"])
        rec_stem = {
            "orm": ["orm_recovered"],
            "metal": ["metal_recovered"],
            "map": ["dense_recovered", "map_recovered"],
        }.get(mode, ["recovered"])
        tgt_stem = {
            "orm": ["orm_forward_truth", "orm_target_0"],
            "metal": ["metal_forward_truth", "metal_target_0"],
            "map": ["dense_forward_truth", "dense_target_0"],
        }.get(mode, ["target"])

        entry = {
            "preset": preset,
            "mode": mode,
            "dir": d.name,
            "path": str(d),
            "pass": _pass_from_metrics(metrics),
            "script_pass": tsv_pass.get(str(d), tsv_pass.get(d.name)),
            "metrics": {
                k: metrics[k]
                for k in metrics
                if not k.startswith("_") and k
                in {
                    "mode",
                    "init_psnr",
                    "train_psnr",
                    "psnr_improve_db",
                    "rough_mse_init",
                    "rough_mse_recovered",
                    "metal_mse_init",
                    "metal_mse_recovered",
                    "map_mse_init",
                    "map_mse_recovered",
                    "relight_init_psnr",
                    "relight_recovered_psnr",
                    "relight_improve_db",
                    "fit_wh",
                    "show_wh",
                    "dense_map_res",
                    "dense_grid",
                    "wrong_init_source",
                }
            },
            "stills": {
                "init": _pick_still(d, init_stem),
                "recovered": _pick_still(d, rec_stem),
                "target": _pick_still(d, tgt_stem),
            },
        }
        runs.append(entry)
    return runs


def write_summary_json(runs: list[dict[str, Any]], path: Path) -> None:
    presets = sorted({r["preset"] for r in runs})
    passed = [r for r in runs if r["pass"]]
    payload = {
        "milestone": "M3a",
        "n_runs": len(runs),
        "n_pass": len(passed),
        "presets": presets,
        "n_presets_pass": len({r["preset"] for r in passed}),
        "runs": runs,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n")


def write_summary_md(runs: list[dict[str, Any]], path: Path) -> None:
    lines = [
        "# Inverse gallery (M3a)",
        "",
        "Multi-preset dense Diff-IR plate. Each row is one MAPTEST-gated run.",
        "",
        "| Preset | Mode | FIT | SHOW | Init→Train PSNR | Map MSE | Relight Δ | Pass |",
        "|--------|------|-----|------|-----------------|---------|-----------|------|",
    ]
    for r in runs:
        m = r["metrics"]
        fit = m.get("fit_wh", "?")
        show = m.get("show_wh", "?")
        psnr = f"{m.get('init_psnr', 0):.1f}→{m.get('train_psnr', 0):.1f}"
        if "rough_mse_init" in m:
            mmap = f"{m['rough_mse_init']:.3f}→{m.get('rough_mse_recovered', 0):.3f}"
        elif "metal_mse_init" in m:
            mmap = f"{m['metal_mse_init']:.3f}→{m.get('metal_mse_recovered', 0):.3f}"
        else:
            mmap = "—"
        rel = m.get("relight_improve_db")
        rel_s = f"{rel:.1f} dB" if isinstance(rel, (int, float)) else "—"
        mark = "✅" if r["pass"] else "❌"
        lines.append(
            f"| {r['preset']} | {r['mode']} | {fit} | {show} | {psnr} | {mmap} | {rel_s} | {mark} |"
        )
    n_pass = sum(1 for r in runs if r["pass"])
    n_presets = len({r["preset"] for r in runs if r["pass"]})
    lines += [
        "",
        f"**Runs pass:** {n_pass}/{len(runs)}  ·  **Presets with ≥1 pass:** {n_presets}",
        "",
        "Open `gallery_wall.html` for the wrong / recovered / target strip.",
        "",
    ]
    path.write_text("\n".join(lines))


def write_wall_html(runs: list[dict[str, Any]], path: Path) -> None:
    cards = []
    for r in runs:
        d = r["dir"]
        stills = r["stills"]
        cells = []
        for label, key in (("wrong-init", "init"), ("recovered", "recovered"), ("target", "target")):
            name = stills.get(key)
            if name:
                cells.append(
                    f'<figure><img src="{d}/{name}" alt="{label}" loading="lazy"/>'
                    f"<figcaption>{label}</figcaption></figure>"
                )
            else:
                cells.append(f"<figure><figcaption>{label} (missing)</figcaption></figure>")
        mark = "PASS" if r["pass"] else "FAIL"
        m = r["metrics"]
        meta = (
            f"{r['preset']} · {r['mode']} · "
            f"PSNR {m.get('init_psnr', 0):.1f}→{m.get('train_psnr', 0):.1f} · {mark}"
        )
        cards.append(f'<section class="card"><h2>{meta}</h2><div class="row">{"".join(cells)}</div></section>')

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>OHAO Inverse Gallery (M3a)</title>
<style>
  body {{ font-family: ui-sans-serif, system-ui, sans-serif; margin: 24px; background: #0e1116; color: #e8eaed; }}
  h1 {{ font-weight: 600; letter-spacing: -0.02em; }}
  h2 {{ font-size: 1rem; font-weight: 500; color: #b0b8c4; margin: 0 0 12px; }}
  .card {{ margin: 0 0 28px; padding: 16px; border: 1px solid #2a3140; border-radius: 12px; background: #151a22; }}
  .row {{ display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }}
  figure {{ margin: 0; }}
  img {{ width: 100%; height: auto; border-radius: 8px; background: #000; display: block; }}
  figcaption {{ font-size: 0.8rem; color: #8b93a1; margin-top: 6px; text-align: center; }}
  a {{ color: #8ab4ff; }}
</style>
</head>
<body>
<h1>OHAO Inverse Lab — multi-preset gallery (M3a)</h1>
<p>Wrong-init / recovered / target (SHOW stills when present). Generated by
<code>scripts/run_inverse_gallery.sh</code>.</p>
{"".join(cards)}
</body>
</html>
"""
    path.write_text(html)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", type=Path, help="Gallery root (e.g. renders/inverse_gallery)")
    ap.add_argument("--summary-json", type=Path, default=None)
    ap.add_argument("--summary-md", type=Path, default=None)
    ap.add_argument("--wall-html", type=Path, default=None)
    args = ap.parse_args()
    root = args.root
    if not root.is_dir():
        print(f"FAIL missing gallery root {root}")
        return 1
    runs = collect_runs(root)
    if not runs:
        print(f"FAIL no metric runs under {root}")
        return 1
    write_summary_json(runs, args.summary_json or root / "gallery_summary.json")
    write_summary_md(runs, args.summary_md or root / "GALLERY.md")
    write_wall_html(runs, args.wall_html or root / "gallery_wall.html")
    print(f"gallery wall: {len(runs)} runs → {args.wall_html or root / 'gallery_wall.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
