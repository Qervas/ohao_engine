#!/usr/bin/env python3
"""Build a simple HTML gallery of inverse before/after compare sheets.

Usage:
  python3 tools/inverse_c1/make_gallery.py --root renders/inverse_c1_gallery
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def parse_run_log(path: Path) -> dict:
    if not path.is_file():
        return {}
    t = path.read_text(errors="ignore")
    prim = re.search(
        r"primary \|Δ\| RGB=\(([^)]+)\)\s*rough=([0-9.eE+-]+)\s*metal=([0-9.eE+-]+)",
        t,
    )
    show = re.search(r"SHOW RMSE \(primary\) = ([0-9.eE+-]+)", t)
    prmse = re.search(r"param RMSE = ([0-9.eE+-]+)", t)
    st = "PASS" if "SELFTEST PASS" in t else ("FAIL" if "SELFTEST FAIL" in t else "?")
    out: dict = {"selftest": st}
    if prim:
        rgb = [float(x) for x in prim.group(1).split(",")]
        out["rgb_mean"] = sum(rgb) / 3.0
        out["rgb"] = rgb
        out["rough"] = float(prim.group(2))
        out["metal"] = float(prim.group(3))
    if show:
        out["show_rmse"] = float(show.group(1))
    if prmse:
        out["param_rmse"] = float(prmse.group(1))
    return out


def fmt(v, nd=3):
    if v is None:
        return "—"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path("renders/inverse_c1_gallery"))
    args = ap.parse_args()
    root = args.root
    root.mkdir(parents=True, exist_ok=True)

    presets = []
    for p in ("lantern", "mirror", "outdoor", "spheres", "helmet"):
        base = root / f"{p}_baseline"
        # Prefer metal-pass hybrid dirs, then classic _nn
        nn = root / f"{p}_metal"
        if not nn.is_dir():
            nn = root / f"{p}_nn"
        if not nn.is_dir() and not base.is_dir():
            continue
        b = parse_run_log(base / "run.log") if base.is_dir() else {}
        # Metal-pass runs often only leave trajectory + compares; scrape compare dir via summary
        n = {}
        for log_name in ("run.log", "fit.log"):
            if (nn / log_name).is_file():
                n = parse_run_log(nn / log_name)
                break
        # Fall back: parse recovered metrics from sibling tmp logs is not available; keep empty
        # prefer summary json if present
        sj = root / f"{p}_summary.json"
        if sj.is_file():
            try:
                s = json.loads(sj.read_text())
                b = s.get("baseline") or b
                n = s.get("nn_hybrid") or n
                # normalize keys
                if "rgb_mean_abs" in b:
                    b["rgb_mean"] = b["rgb_mean_abs"]
                if "rgb_mean_abs" in n:
                    n["rgb_mean"] = n["rgb_mean_abs"]
                if "metal_abs" in b:
                    b["metal"] = b["metal_abs"]
                if "metal_abs" in n:
                    n["metal"] = n["metal_abs"]
                if "rough_abs" in b:
                    b["rough"] = b["rough_abs"]
                if "rough_abs" in n:
                    n["rough"] = n["rough_abs"]
                if "show_rmse" in b:
                    pass
            except Exception:
                pass
        imgs = []
        for label, d in (("NN hybrid", nn), ("Baseline FD", base)):
            if not d.is_dir():
                continue
            for name in (
                "compare_before_after.png",
                "compare_target_recovered.png",
                "compare_multiview.png",
            ):
                if (d / name).is_file():
                    imgs.append((f"{label}: {name}", f"{d.name}/{name}"))
        presets.append({"name": p, "baseline": b, "nn": n, "images": imgs})

    rows_html = []
    for pr in presets:
        b, n = pr["baseline"], pr["nn"]
        rows_html.append(
            f"""
<tr>
  <td><b>{pr['name']}</b></td>
  <td>{fmt(b.get('rgb_mean'))}</td>
  <td><b>{fmt(n.get('rgb_mean'))}</b></td>
  <td>{fmt(b.get('metal'))}</td>
  <td><b>{fmt(n.get('metal'))}</b></td>
  <td>{fmt(b.get('show_rmse'))}</td>
  <td><b>{fmt(n.get('show_rmse'))}</b></td>
  <td>{b.get('selftest','—')}</td>
  <td>{n.get('selftest','—')}</td>
</tr>"""
        )

    sections = []
    for pr in presets:
        cards = []
        for title, rel in pr["images"]:
            cards.append(
                f"""
<figure class="card">
  <figcaption>{title}</figcaption>
  <a href="{rel}" target="_blank"><img src="{rel}" alt="{title}" loading="lazy"/></a>
</figure>"""
            )
        sections.append(
            f"""
<section>
  <h2>{pr['name']}</h2>
  <div class="grid">{''.join(cards) if cards else '<p>No compare images yet.</p>'}</div>
</section>"""
        )

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>OHAO Inverse C1 Gallery</title>
<style>
  :root {{ color-scheme: dark; font-family: ui-sans-serif, system-ui, sans-serif; }}
  body {{ margin: 0; background: #0e0f12; color: #e8eaef; }}
  header {{ padding: 28px 24px 12px; border-bottom: 1px solid #242833; }}
  h1 {{ margin: 0 0 8px; font-size: 1.5rem; }}
  p {{ color: #9aa3b5; margin: 0.3rem 0; }}
  main {{ padding: 20px 24px 48px; max-width: 1400px; margin: 0 auto; }}
  table {{ border-collapse: collapse; width: 100%; margin: 16px 0 28px; font-size: 0.95rem; }}
  th, td {{ border: 1px solid #2a2f3a; padding: 8px 10px; text-align: center; }}
  th {{ background: #161a22; color: #c5cbe0; }}
  td:first-child, th:first-child {{ text-align: left; }}
  section {{ margin-bottom: 36px; }}
  h2 {{ font-size: 1.2rem; margin: 0 0 12px; text-transform: capitalize; }}
  .grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(420px, 1fr)); gap: 16px; }}
  .card {{ margin: 0; background: #151821; border: 1px solid #262b38; border-radius: 10px; overflow: hidden; }}
  .card figcaption {{ padding: 8px 12px; font-size: 0.85rem; color: #aab3c5; border-bottom: 1px solid #262b38; }}
  .card img {{ display: block; width: 100%; height: auto; background: #000; }}
  code {{ background: #1c2130; padding: 1px 6px; border-radius: 4px; }}
</style>
</head>
<body>
<header>
  <h1>OHAO Inverse C1 — Before / After Gallery</h1>
  <p>Baseline FD vs NN hybrid. Lower RGB / SHOW RMSE is better.</p>
  <p>Regenerate: <code>./tools/inverse_c1/pipeline.sh --skip-train</code></p>
</header>
<main>
  <h2>Summary</h2>
  <table>
    <thead>
      <tr>
        <th>Preset</th>
        <th>RGB base</th><th>RGB NN</th>
        <th>metal base</th><th>metal NN</th>
        <th>SHOW base</th><th>SHOW NN</th>
        <th>Selftest base</th><th>Selftest NN</th>
      </tr>
    </thead>
    <tbody>
      {''.join(rows_html) if rows_html else '<tr><td colspan="9">No runs found</td></tr>'}
    </tbody>
  </table>
  {''.join(sections)}
</main>
</body>
</html>
"""
    out = root / "index.html"
    out.write_text(html, encoding="utf-8")
    print(f"wrote {out}")
    print(f"  presets: {[p['name'] for p in presets]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
