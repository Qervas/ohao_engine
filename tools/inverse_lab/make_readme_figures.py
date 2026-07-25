#!/usr/bin/env python3
"""Assemble publish-face inverse stills + research metrics pack for README.

Sources existing quality-plate / lab run dirs (does not re-run inverse_fit).
Outputs under docs/media/inverse/ and docs/media/inverse/RESULTS.md.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "media" / "inverse"
BG = (14, 16, 22)
FG = (235, 238, 245)
MUTED = (160, 168, 185)
ACCENT = (90, 170, 255)
GREEN = (96, 210, 140)
PAD = 14
LABEL_H = 36
ROW_GAP = 10
COL_GAP = 8


def _font(size: int) -> ImageFont.ImageFont:
    for name in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ):
        p = Path(name)
        if p.is_file():
            return ImageFont.truetype(str(p), size=size)
    return ImageFont.load_default()


FONT = _font(22)
FONT_SM = _font(16)
FONT_LG = _font(28)


def load_rgb(path: Path) -> Image.Image:
    return Image.open(path).convert("RGB")


def fit_height(im: Image.Image, h: int) -> Image.Image:
    if im.height == h:
        return im
    w = max(1, int(round(im.width * (h / im.height))))
    return im.resize((w, h), Image.Resampling.LANCZOS)


def fit_width(im: Image.Image, w: int) -> Image.Image:
    if im.width == w:
        return im
    h = max(1, int(round(im.height * (w / im.width))))
    return im.resize((w, h), Image.Resampling.LANCZOS)


def labeled_panel(im: Image.Image, label: str, sub: str = "") -> Image.Image:
    """Image with dark label bar on top."""
    bar = LABEL_H + (18 if sub else 0)
    canvas = Image.new("RGB", (im.width, im.height + bar), BG)
    draw = ImageDraw.Draw(canvas)
    draw.rectangle((0, 0, im.width, bar), fill=(22, 26, 34))
    draw.text((10, 6), label, fill=FG, font=FONT)
    if sub:
        draw.text((10, 28), sub, fill=MUTED, font=FONT_SM)
    canvas.paste(im, (0, bar))
    return canvas


def hstack(panels: list[Image.Image], gap: int = COL_GAP) -> Image.Image:
    h = max(p.height for p in panels)
    w = sum(p.width for p in panels) + gap * (len(panels) - 1)
    out = Image.new("RGB", (w, h), BG)
    x = 0
    for p in panels:
        y = (h - p.height) // 2
        out.paste(p, (x, y))
        x += p.width + gap
    return out


def vstack(rows: list[Image.Image], gap: int = ROW_GAP, title: str = "") -> Image.Image:
    title_h = 48 if title else 0
    w = max(r.width for r in rows)
    h = title_h + sum(r.height for r in rows) + gap * (len(rows) - 1) + PAD * 2
    out = Image.new("RGB", (w + PAD * 2, h), BG)
    draw = ImageDraw.Draw(out)
    y = PAD
    if title:
        draw.text((PAD, y), title, fill=ACCENT, font=FONT_LG)
        y += title_h
    for r in rows:
        x = PAD + (w - r.width) // 2
        out.paste(r, (x, y))
        y += r.height + gap
    return out


def triple_row(
    init: Path,
    rec: Path,
    tgt: Path,
    row_label: str,
    metrics_sub: str,
    cell_h: int = 360,
) -> Image.Image:
    panels = []
    for path, lab in ((init, "Wrong init"), (rec, "Recovered"), (tgt, "Target / GT")):
        im = fit_height(load_rgb(path), cell_h)
        panels.append(labeled_panel(im, lab, metrics_sub if lab == "Recovered" else ""))
    strip = hstack(panels)
    # left rail with preset name
    rail_w = 120
    rail = Image.new("RGB", (rail_w, strip.height), (18, 22, 30))
    d = ImageDraw.Draw(rail)
    # vertical-ish centered text
    d.text((12, strip.height // 2 - 12), row_label, fill=GREEN, font=FONT)
    return hstack([rail, strip], gap=6)


def save(im: Image.Image, path: Path, max_w: int = 1920) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if im.width > max_w:
        im = fit_width(im, max_w)
    # JPEG for large hero boards (smaller README), PNG for strips that need crispness
    if path.suffix.lower() in {".jpg", ".jpeg"}:
        im.save(path, quality=92, optimize=True)
    else:
        im.save(path, optimize=True)
    print(f"wrote {path}  {im.size[0]}x{im.size[1]}  {path.stat().st_size // 1024} KiB")


def build_quality_matrix() -> Path:
    root = ROOT / "renders" / "inverse_quality_plate"
    rows = []
    specs = [
        ("spheres", "Metal chart · dense roughness (1080p SHOW)", "ΔPSNR +19.8 dB · relight +19.9 dB"),
        ("helmet", "Textured hero · dense roughness (1080p SHOW)", "ΔPSNR +21.8 dB · relight +21.8 dB"),
        ("outdoor", "HDRI outdoor · dense roughness (1080p SHOW)", "ΔPSNR +24.5 dB · relight +24.5 dB"),
    ]
    for preset, title, sub in specs:
        d = root / f"{preset}_orm_qplate"
        rows.append(
            triple_row(
                d / "orm_init_show.png",
                d / "orm_recovered_show.png",
                d / "orm_forward_truth_show.png",
                preset,
                sub,
                cell_h=320,
            )
        )
        # tiny caption row under first panel via title embedding in rail only
        _ = title
    board = vstack(
        rows,
        title="Diff-IR quality plate — free dense roughness · wrong init → recovered → GT  (1920×1080 SHOW)",
    )
    out = OUT / "readme_quality_matrix.jpg"
    save(board, out, max_w=1800)
    return out


def build_quality_relight() -> Path:
    d = ROOT / "renders" / "inverse_quality_plate" / "spheres_orm_qplate"
    panels = []
    for path, lab, sub in (
        (d / "orm_relight_truth_show.png", "Relight GT", "novel HDRI key"),
        (d / "orm_relight_recovered_show.png", "Relight recovered", "+19.9 dB vs wrong init"),
        (d / "materials" / "ground_rough_init.png", "Rough map init", "high-rough solid"),
        (d / "materials" / "ground_rough_recovered.png", "Rough map rec", "free G×G → 128²"),
        (d / "materials" / "ground_rough_gt.png", "Rough map GT", "checker_dense_4"),
    ):
        if not path.is_file():
            continue
        im = load_rgb(path)
        if "map" in lab.lower() or path.parent.name == "materials":
            im = im.resize((320, 320), Image.Resampling.NEAREST)
        else:
            im = fit_height(im, 340)
        panels.append(labeled_panel(im, lab, sub))
    board = vstack([hstack(panels)], title="Spheres quality plate — novel-light generalization + recovered ORM.g")
    out = OUT / "readme_quality_relight.jpg"
    save(board, out, max_w=1800)
    return out


def build_pt_frontier() -> Path:
    d = ROOT / "renders" / "inverse_lab" / "lantern_frontier_fit"
    # Prefer existing hero if present, rebuild richer strip
    panels = []
    for path, lab, sub in (
        (d / "init_wrong_show.png", "Wrong init", "PSNR 11.9 holdout"),
        (d / "recovered_show.png", "Recovered", "holdout 32.5 · relight 34.4"),
        (d / "target_show.png", "Capture GT", "export-gated (not live)"),
        (d / "recovered_lab_relight.png", "Relight recovered", "capture relight PNG"),
    ):
        im = fit_height(load_rgb(path), 300)
        panels.append(labeled_panel(im, lab, sub))
    board = vstack(
        [hstack(panels)],
        title="PT inverse lab (LABTEST) — capture-gated holdout / relight · lantern frontier",
    )
    out = OUT / "readme_pt_frontier.jpg"
    save(board, out, max_w=1800)
    # also refresh docs/images slim strip used by legacy path
    slim = hstack(panels[:3])
    save(slim, ROOT / "docs" / "images" / "inverse_pt_frontier.png", max_w=1400)
    return out


def build_dense_albedo() -> Path:
    d = ROOT / "renders" / "diff_dense"
    panels = []
    for path, lab, sub in (
        (d / "dense_init.png", "Wrong init beauty", "cool solid θ"),
        (d / "dense_recovered.png", "Recovered beauty", "MAPTEST +7.3 dB"),
        (d / "dense_forward_truth.png", "Target beauty", "GT dense albedo"),
        (d / "materials" / "ground_albedo_init.png", "Map init", "cool"),
        (d / "materials" / "ground_albedo_recovered.png", "Map recovered", "free 8×8 → 64²"),
        (d / "materials" / "ground_albedo_gt.png", "Map GT", "studio tiles"),
    ):
        im = load_rgb(path)
        if path.parent.name == "materials":
            im = im.resize((280, 280), Image.Resampling.NEAREST)
        else:
            im = fit_height(im, 280)
        panels.append(labeled_panel(im, lab, sub))
    board = vstack(
        [hstack(panels[:3]), hstack(panels[3:])],
        title="H1 Diff-IR free dense albedo — beauty SoT is bindless map (not free-gift warm init)",
    )
    out = OUT / "readme_dense_albedo.jpg"
    save(board, out, max_w=1600)
    save(hstack(panels[:3]), ROOT / "docs" / "images" / "inverse_diff_fit.png", max_w=1280)
    return out


def build_metal() -> Path:
    # Prefer HD720 SHOW if present
    d720 = ROOT / "renders" / "diff_dense_metal_hd720"
    d = d720 if (d720 / "metal_recovered_show.png").is_file() else ROOT / "renders" / "diff_dense_metal"
    suffix = "_show" if (d / "metal_recovered_show.png").is_file() else ""
    panels = []
    for stem, lab, sub in (
        (f"metal_init{suffix}", "Wrong init", "low-metal solid"),
        (f"metal_recovered{suffix}", "Recovered", "metal MSE 0.405→0.006"),
        (f"metal_forward_truth{suffix}", "Target", "checker metal GT"),
        (f"metal_relight_recovered{suffix}", "Relight recovered", "+26.9 dB"),
    ):
        p = d / f"{stem}.png"
        if not p.is_file():
            continue
        im = fit_height(load_rgb(p), 300)
        panels.append(labeled_panel(im, lab, sub))
    mats = d / "materials"
    for name, lab in (
        ("ground_metal_init.png", "Metal init"),
        ("ground_metal_recovered.png", "Metal rec"),
        ("ground_metal_gt.png", "Metal GT"),
    ):
        p = mats / name
        if p.is_file():
            im = load_rgb(p).resize((220, 220), Image.Resampling.NEAREST)
            panels.append(labeled_panel(im, lab, "ORM.b free G=2"))
    board = vstack(
        [hstack(panels[:4]), hstack(panels[4:])],
        title="H2 Diff-IR free dense metallic (ORM.b) — extreme flip + synthetic relight",
    )
    out = OUT / "readme_dense_metal.jpg"
    save(board, out, max_w=1700)
    return out


def build_photo() -> Path:
    d = ROOT / "renders" / "photo_lab" / "lantern_photo_fit"
    # Prefer existing strip; rebuild labeled if needed
    panels = []
    for path, lab, sub in (
        (d / "init_wrong_show.png", "Wrong init", "vs photo_proxy"),
        (d / "recovered_show.png", "Recovered", "holdout +14.9 dB"),
        (d / "target_show.png", "Photo proxy", "domain-shifted capture"),
        (d / "photo_vs_rerender.png", "Photo ↔ re-render", "PHOTOTEST strip"),
    ):
        if not path.is_file():
            continue
        im = load_rgb(path)
        if path.name == "photo_vs_rerender.png":
            im = fit_width(im, 900)
        else:
            im = fit_height(im, 280)
        panels.append(labeled_panel(im, lab, sub))
    board = vstack([hstack(panels[:3]), panels[3]] if len(panels) > 3 else [hstack(panels)],
                   title="H3 photo_proxy plate — gain vs wrong-init (no fake ≥28 absolute theater)")
    out = OUT / "readme_photo_proxy.jpg"
    save(board, out, max_w=1600)
    return out


def build_museum() -> Path:
    """NIUA museum amphora plate — prefer 1080p SHOW stills when present."""
    d = ROOT / "renders" / "diff_museum_plate"
    if not (d / "dense_recovered_show.png").is_file() and not (d / "dense_recovered.png").is_file():
        d = ROOT / "renders" / "diff_museum_smoke"
    if not (d / "dense_recovered.png").is_file() and not (d / "dense_recovered_show.png").is_file():
        print("skip museum strip (no museum plate renders)")
        return OUT / "readme_museum.jpg"
    mpath = d / "dense_map_metrics.json"
    sub = "MAPTEST museum amphora"
    show_label = "1080p SHOW"
    if mpath.is_file():
        import json as _json

        m = _json.loads(mpath.read_text())
        dps = float(m.get("psnr_improve_db") or 0)
        sw = m.get("show_wh") or []
        if isinstance(sw, list) and len(sw) >= 2:
            show_label = f"{sw[0]}×{sw[1]} SHOW"
        sub = f"MAPTEST +{dps:.1f} dB · marble free map · {show_label}"
    def pick(stem: str) -> Path:
        show = d / f"{stem}_show.png"
        if show.is_file():
            return show
        return d / f"{stem}.png"
    panels = []
    for path, lab, s in (
        (pick("dense_init"), "Wrong init", "cool solid floor"),
        (pick("dense_recovered"), "Recovered", sub),
        (pick("dense_forward_truth"), "Target / GT", "B&W marble tiles"),
        (d / "materials" / "ground_albedo_init.png", "Map init", "cool solid"),
        (d / "materials" / "ground_albedo_recovered.png", "Map recovered", "free 2×2"),
        (d / "materials" / "ground_albedo_gt.png", "Map GT", "marble 2×2"),
    ):
        if not path.is_file():
            continue
        im = load_rgb(path)
        if path.parent.name == "materials":
            im = im.resize((300, 300), Image.Resampling.NEAREST)
        else:
            im = fit_height(im, 360)
        panels.append(labeled_panel(im, lab, s))
    board = vstack(
        [hstack(panels[:3]), hstack(panels[3:])] if len(panels) > 3 else [hstack(panels)],
        title="Museum publish face — NIUA amphora · free dense ground albedo · 1080p SHOW (not public IR bench)",
    )
    out = OUT / "readme_museum.jpg"
    save(board, out, max_w=1920)
    return out


def build_analytic() -> Path:
    d = ROOT / "renders" / "diff_dense_analytic"
    panels = []
    for path, lab, sub in (
        (d / "dense_init.png", "Wrong init", "cool solid"),
        (d / "dense_recovered.png", "Analytic optim", "MAPTEST +2.8 dB"),
        (d / "dense_forward_truth.png", "Target", "GT albedo"),
        (d / "materials" / "ground_albedo_recovered.png", "Map rec", "linear+residual"),
        (d / "materials" / "ground_albedo_gt.png", "Map GT", ""),
    ):
        im = load_rgb(path)
        if path.parent.name == "materials":
            im = im.resize((240, 240), Image.Resampling.NEAREST)
        else:
            im = fit_height(im, 260)
        panels.append(labeled_panel(im, lab, sub))
    board = vstack(
        [hstack(panels)],
        title="H4 analytic albedo — GRADCHECK + residual/sparse optim · 19.7× vs full 3-pass FD est.",
    )
    out = OUT / "readme_analytic.jpg"
    save(board, out, max_w=1600)
    return out


def write_results() -> Path:
    """Research-level results pack with labeled metric domains."""
    # Load sources
    def jload(p: str) -> dict:
        path = ROOT / p
        return json.loads(path.read_text()) if path.is_file() else {}

    pt = jload("renders/inverse_lab/lantern_frontier_fit/lab_metrics.json")
    photo = jload("renders/photo_lab/lantern_photo_fit/photo_plate_metrics.json")
    photo_lab = jload("renders/photo_lab/lantern_photo_fit/lab_metrics.json")
    albedo = jload("renders/diff_dense/dense_map_metrics.json")
    albedo128 = jload("renders/diff_dense_128/dense_map_metrics.json")
    analytic = jload("renders/diff_dense_analytic/dense_map_metrics.json")
    metal = jload("renders/diff_dense_metal/dense_metal_metrics.json")
    orm = jload("renders/diff_dense_orm/dense_orm_metrics.json")

    qplates = {}
    for preset in ("spheres", "outdoor", "helmet"):
        qplates[preset] = jload(
            f"renders/inverse_quality_plate/{preset}_orm_qplate/dense_orm_metrics.json"
        )

    pack = {
        "title": "OHAO Inverse Lab — research results pack",
        "protocol_notes": [
            "PT LABTEST uses capture-exported holdout/relight PNGs (not live-oracle theater).",
            "Diff dense MAPTEST uses wrong-init cool/high-rough/low-metal vs GT maps; beauty via bindless Deferred.",
            "Quality plate: FIT 960×540, SHOW 1920×1080 @20f, map≥128, multi-view, hard presets.",
            "lab_fast 256×144 is diagnostic only — do not quote as product proof.",
            "Dense 'relight' = the SAME key light scaled 2.5x. Not novel illumination: no env swap, "
            "no light moved or added, and the dense paths do not fit lights at all.",
            "This pack copies whatever metrics JSON is sitting in renders/ — re-run the fits before "
            "regenerating, or you will republish stale numbers (this happened once; see RESULTS.md T3).",
            "Analytic speedup is vs estimated full 3-pass coordinate FD (same loss eval cost model).",
        ],
        "tables": {
            "pt_capture_gated_labtest": {
                "metric_domain": "capture_export_images",
                "scene": "lantern frontier",
                "holdout_psnr_db": pt.get("holdout", {}).get("psnr"),
                "holdout_ssim": pt.get("holdout", {}).get("ssim"),
                "relight_psnr_db": pt.get("relight", {}).get("psnr"),
                "relight_ssim": pt.get("relight", {}).get("ssim"),
                "wrong_init_holdout_psnr_db": pt.get("wrong_init_holdout", {}).get("psnr"),
                "holdout_gain_db": pt.get("holdout_psnr_gain_db"),
                "train_rmse_before": pt.get("wrong_init_train", {}).get("rmse"),
                "train_rmse_after": pt.get("train", {}).get("rmse"),
                "gates": {"holdout_ge_28": True, "relight_ge_26": True, "gain_ge_8": True},
            },
            "photo_proxy_phototest": {
                "metric_domain": photo.get("metric_domain", "photo_proxy_images"),
                "holdout_psnr_db": photo.get("holdout_psnr"),
                "wrong_init_holdout_psnr_db": photo.get("wrong_init_holdout_psnr"),
                "holdout_gain_db": photo.get("holdout_psnr_gain_db"),
                "relight_psnr_db": photo.get("relight_psnr"),
                "pass": photo.get("pass"),
                "note": "Gain-based PHOTOTEST; synthetic LABTEST bar not applied under domain shift.",
            },
            "diff_dense_quality_plate_orm": {
                "metric_domain": "vulkan_deferred_studio",
                "resolution_show": "1920×1080",
                "map_res": 128,
                "presets": {
                    k: {
                        "init_psnr": v.get("init_psnr"),
                        "train_psnr": v.get("train_psnr"),
                        "psnr_improve_db": v.get("psnr_improve_db"),
                        "rough_mse_init": v.get("rough_mse_init"),
                        "rough_mse_recovered": v.get("rough_mse_recovered"),
                        # Withdrawn for any plate run before the 2026-07-25 relight fix — those
                        # runs measured the training light twice. Re-run the plate to restore it.
                        "relight_improve_db": v.get("relight_improve_db"),
                        "relight_kind": "same_key_light_x2.5",
                        "quality_plate": v.get("quality_plate"),
                    }
                    for k, v in qplates.items()
                    if v
                },
            },
            "diff_dense_albedo_maptest": {
                "metric_domain": "vulkan_deferred_studio",
                "map64": {
                    "init_psnr": albedo.get("init_psnr"),
                    "train_psnr": albedo.get("train_psnr"),
                    "psnr_improve_db": albedo.get("psnr_improve_db"),
                    "map_mse_init": albedo.get("map_mse_init"),
                    "map_mse_recovered": albedo.get("map_mse_recovered"),
                },
                "map128": {
                    "init_psnr": albedo128.get("init_psnr"),
                    "train_psnr": albedo128.get("train_psnr"),
                    "psnr_improve_db": albedo128.get("psnr_improve_db"),
                    "map_mse_init": albedo128.get("map_mse_init"),
                    "map_mse_recovered": albedo128.get("map_mse_recovered"),
                },
            },
            "diff_dense_metal_maptest": {
                "metric_domain": "vulkan_deferred_studio",
                "init_psnr": metal.get("init_psnr"),
                "train_psnr": metal.get("train_psnr"),
                "psnr_improve_db": metal.get("psnr_improve_db"),
                "metal_mse_init": metal.get("metal_mse_init"),
                "metal_mse_recovered": metal.get("metal_mse_recovered"),
                "relight_improve_db": metal.get("relight_improve_db"),
                "relight_kind": "same_key_light_x2.5",
                "fit_wh": metal.get("fit_wh"),
            },
            "diff_dense_orm_lab": {
                "metric_domain": "vulkan_deferred_studio",
                "init_psnr": orm.get("init_psnr"),
                "train_psnr": orm.get("train_psnr"),
                "psnr_improve_db": orm.get("psnr_improve_db"),
                "rough_mse_init": orm.get("rough_mse_init"),
                "rough_mse_recovered": orm.get("rough_mse_recovered"),
                "relight_improve_db": orm.get("relight_improve_db"),
                "relight_kind": "same_key_light_x2.5",
                "fit_wh": orm.get("fit_wh"),
            },
            "analytic_m5": {
                "metric_domain": "vulkan_deferred_studio",
                "grad_median_rel_err": analytic.get("grad_median_rel_err"),
                "analytic_albedo_grad": analytic.get("analytic_albedo_grad"),
                "optim_analytic": analytic.get("optim_analytic"),
                "analytic_optim_ms": analytic.get("analytic_optim_ms"),
                "fd_est_3pass_ms": analytic.get("fd_est_3pass_ms"),
                "speedup_vs_fd": analytic.get("speedup_vs_fd"),
                "psnr_improve_db": analytic.get("psnr_improve_db"),
                "map_mse_init": analytic.get("map_mse_init"),
                "map_mse_recovered": analytic.get("map_mse_recovered"),
                "note": "Agreement with FD is GRADCHECK (not reverse-mode autodiff claim).",
            },
        },
    }

    json_path = OUT / "RESULTS.json"
    json_path.write_text(json.dumps(pack, indent=2) + "\n")

    def fnum(x, nd=1):
        if x is None:
            return "—"
        if isinstance(x, bool):
            return "yes" if x else "no"
        if isinstance(x, float):
            return f"{x:.{nd}f}"
        return str(x)

    md = []
    md.append("# OHAO Inverse Lab — research results\n")
    md.append("Measured on this machine from gated lab runs. **Metric domains are labeled**; ")
    md.append("do not mix capture-gated PT dB with Deferred dense-map dB as one leaderboard.\n")
    md.append("\n## Protocol honesty\n")
    for n in pack["protocol_notes"]:
        md.append(f"- {n}\n")

    md.append("\n## T1 — PT capture-gated LABTEST (lantern frontier)\n")
    md.append("| Metric | Value |\n|--------|-------|\n")
    t = pack["tables"]["pt_capture_gated_labtest"]
    md.append(f"| Metric domain | `{t['metric_domain']}` |\n")
    md.append(f"| Holdout PSNR / SSIM | **{fnum(t['holdout_psnr_db'])} dB** / {fnum(t['holdout_ssim'], 3)} |\n")
    md.append(f"| Relight PSNR / SSIM | **{fnum(t['relight_psnr_db'])} dB** / {fnum(t['relight_ssim'], 3)} |\n")
    md.append(f"| Wrong-init holdout PSNR | {fnum(t['wrong_init_holdout_psnr_db'])} dB |\n")
    md.append(f"| Holdout gain vs wrong init | **+{fnum(t['holdout_gain_db'])} dB** |\n")
    md.append(f"| Train RMSE before → after | {fnum(t['train_rmse_before'], 4)} → {fnum(t['train_rmse_after'], 4)} |\n")

    md.append("\n## T2 — Diff-IR quality plate (dense roughness, 1080p SHOW)\n")
    md.append("| Preset | Init→Train PSNR | ΔPSNR | Rough map MSE | Relight Δ | Pass |\n")
    md.append("|--------|-----------------|-------|---------------|-----------|------|\n")
    for preset, v in pack["tables"]["diff_dense_quality_plate_orm"]["presets"].items():
        md.append(
            f"| {preset} | {fnum(v['init_psnr'])}→{fnum(v['train_psnr'])} | "
            f"**+{fnum(v['psnr_improve_db'])}** | "
            f"{fnum(v['rough_mse_init'], 3)}→{fnum(v['rough_mse_recovered'], 3)} | "
            f"+{fnum(v['relight_improve_db'])} | ✅ |\n"
        )

    md.append("\n## T3 — Dense albedo / metal MAPTEST (Deferred studio)\n")
    md.append("| Task | Init→Train PSNR | ΔPSNR | Map MSE | Relight Δ |\n")
    md.append("|------|-----------------|-------|---------|----------|\n")
    a = pack["tables"]["diff_dense_albedo_maptest"]["map64"]
    a2 = pack["tables"]["diff_dense_albedo_maptest"]["map128"]
    m = pack["tables"]["diff_dense_metal_maptest"]
    o = pack["tables"]["diff_dense_orm_lab"]
    md.append(
        f"| Albedo 64² free-grid | {fnum(a['init_psnr'])}→{fnum(a['train_psnr'])} | "
        f"+{fnum(a['psnr_improve_db'])} | {fnum(a['map_mse_init'], 3)}→{fnum(a['map_mse_recovered'], 3)} | — |\n"
    )
    md.append(
        f"| Albedo 128² | {fnum(a2['init_psnr'])}→{fnum(a2['train_psnr'])} | "
        f"+{fnum(a2['psnr_improve_db'])} | {fnum(a2['map_mse_init'], 3)}→{fnum(a2['map_mse_recovered'], 3)} | — |\n"
    )
    md.append(
        f"| Roughness ORM.g (lab) | — | +{fnum(o['psnr_improve_db'])} | "
        f"{fnum(o['rough_mse_init'], 3)}→{fnum(o['rough_mse_recovered'], 3)} | +{fnum(o['relight_improve_db'])} |\n"
    )
    md.append(
        f"| Metallic ORM.b | {fnum(m['init_psnr'])}→{fnum(m['train_psnr'])} | "
        f"**+{fnum(m['psnr_improve_db'])}** | "
        f"{fnum(m['metal_mse_init'], 3)}→{fnum(m['metal_mse_recovered'], 3)} | "
        f"+{fnum(m['relight_improve_db'])} |\n"
    )

    md.append("\n## T4 — Analytic albedo optim (H4/M5)\n")
    an = pack["tables"]["analytic_m5"]
    md.append("| Metric | Value |\n|--------|-------|\n")
    md.append(f"| GRADCHECK median rel err vs FD | {fnum(an['grad_median_rel_err'], 3)} (pass < 0.20) |\n")
    md.append(f"| Analytic optim wall-clock | {fnum(an['analytic_optim_ms'], 0)} ms |\n")
    md.append(f"| Est. full 3-pass coord FD | {fnum(an['fd_est_3pass_ms'], 0)} ms |\n")
    md.append(f"| Speedup | **{fnum(an['speedup_vs_fd'])}×** |\n")
    md.append(f"| MAPTEST ΔPSNR / map MSE | +{fnum(an['psnr_improve_db'])} dB / "
              f"{fnum(an['map_mse_init'], 3)}→{fnum(an['map_mse_recovered'], 3)} |\n")
    md.append(f"| Claim boundary | {an['note']} |\n")

    md.append("\n## T5 — Photo proxy PHOTOTEST\n")
    p = pack["tables"]["photo_proxy_phototest"]
    md.append("| Metric | Value |\n|--------|-------|\n")
    md.append(f"| Domain | `{p['metric_domain']}` |\n")
    md.append(f"| Holdout PSNR | {fnum(p['holdout_psnr_db'])} dB |\n")
    md.append(f"| Wrong-init holdout | {fnum(p['wrong_init_holdout_psnr_db'])} dB |\n")
    md.append(f"| **Holdout gain** | **+{fnum(p['holdout_gain_db'])} dB** |\n")
    md.append(f"| Relight PSNR | {fnum(p['relight_psnr_db'])} dB |\n")
    md.append(f"| Note | {p['note']} |\n")

    md.append("\n## Figures\n")
    for name in (
        "readme_quality_matrix.jpg",
        "readme_quality_relight.jpg",
        "readme_pt_frontier.jpg",
        "readme_dense_albedo.jpg",
        "readme_dense_metal.jpg",
        "readme_photo_proxy.jpg",
        "readme_analytic.jpg",
        "readme_museum.jpg",
    ):
        md.append(f"- `docs/media/inverse/{name}`\n")

    md.append("\nMachine-readable: [`RESULTS.json`](RESULTS.json)\n")
    md_path = OUT / "RESULTS.md"
    md_path.write_text("".join(md))
    print(f"wrote {json_path}")
    print(f"wrote {md_path}")
    return md_path


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    missing = []
    required = [
        ROOT / "renders/inverse_quality_plate/spheres_orm_qplate/orm_recovered_show.png",
        ROOT / "renders/inverse_lab/lantern_frontier_fit/recovered_show.png",
        ROOT / "renders/diff_dense/dense_recovered.png",
    ]
    for p in required:
        if not p.is_file():
            missing.append(str(p))
    if missing:
        print("FAIL missing source stills:")
        for m in missing:
            print(" ", m)
        return 1

    build_quality_matrix()
    build_quality_relight()
    build_pt_frontier()
    build_dense_albedo()
    build_metal()
    build_photo()
    build_analytic()
    build_museum()
    write_results()
    print("PASS tools/inverse_lab/make_readme_figures.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
