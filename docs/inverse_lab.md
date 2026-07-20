# Inverse Lab — frontier multi-view / novel-view / relight protocol

**Status:** L1–L2 **pass** on synthetic lantern studio capture (capture-gated PSNR/SSIM bar).  
**Long-run plan:** [`docs/inverse_lab_roadmap.md`](inverse_lab_roadmap.md) — horizons H0–H5, elongated phases, milestones M0–M6.

## Problem definition

| | Demo selftest | Lab track |
|--|---------------|-----------|
| **In** | Fixed scene + images | Multi-view **capture bundle** (train / holdout / relight) |
| **Optimize** | Train views only | Train views only (holdout never in loss) |
| **Out** | θ + stills | θ + **spatial maps** + train/holdout/relight **PSNR/SSIM** |
| **Pass** | RMSE gate | **LABTEST** PSNR bar (below) |

## Frontier eval bar (synthetic lantern)

| Metric | Threshold | Measured (draft, 640×360 @ 128 spp, capture GT) |
|--------|-----------|--------------------------------------------------|
| Holdout PSNR | ≥ **28 dB** | **32.46 dB** |
| Relight PSNR | ≥ **26 dB** | **34.36 dB** |
| Holdout PSNR gain vs wrong-init | ≥ **8 dB** | **20.52 dB** |
| Train PSNR (info) | — | 34.20 dB |
| SSIM | reported | holdout 0.983, relight 0.989 |
| Oracle ceiling (diag) | — | 32.22 dB (truth θ vs capture holdout) |

- **Train targets:** capture PNGs (export, no denoise).  
- **Metric GT (LABTEST):** exported holdout/relight PNGs only. Documented as `metric_gt=capture_export_images`. Live re-renders of truth θ are **diagnostics** (`psnr_live_diag`, `oracle_truth_vs_capture_psnr`) — they do **not** pass the gate.  
- **LPIPS:** optional / not bundled (`lpips: null`).

## Capture format (`ohao_inverse_lab_capture` v1)

```
capture/
  capture.json
  cameras.jsonl
  images/train_*.png, holdout_*.png
  relight/...
  materials/ground_albedo*.png, ground_orm*.png
  theta_gt.json
```

Spatial materials: `--map-res N` → N×N ground albedo tiles + shared rough/metal (default N=2 on `--export-capture`).

## CLI

```bash
# Export multi-view capture (spatial maps on; no denoise — domain match for FIT)
./build/inverse_fit --export-capture --preset lantern --quality draft --views 3 \
  --show-width 640 --show-height 360 --show-spp 128 --fit-spp 64 --map-res 2 \
  --out-dir renders/inverse_lab/lantern_frontier

# Fit (train-only loss; half-res FD + full-res polish; holdout + relight eval)
./build/inverse_fit --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --preset lantern --quality draft \
  --show-width 640 --show-height 360 --show-spp 128 --fit-spp 64 \
  --iters 24 --multi-start 5 --visual-polish --polish-iters 8 \
  --out-dir renders/inverse_lab/lantern_frontier_fit

python3 tools/inverse_lab/test_metrics_and_maps.py renders/inverse_lab/lantern_frontier_fit
python3 tools/inverse_lab/eval_bundle.py renders/inverse_lab/lantern_frontier_fit

# H1 free dense ground albedo (Deferred map SoT, MAPTEST) — 64² or 128²
./build/inverse_fit --backend diff --dense-map --dense-map-res 64 --dense-grid 8 \
  --preset lantern --quality draft --out-dir renders/diff_dense
./build/inverse_fit --backend diff --dense-map --dense-map-res 128 --dense-grid 8 \
  --preset lantern --quality draft --out-dir renders/diff_dense_128
python3 tools/inverse_lab/test_dense_map.py renders/diff_dense
python3 tools/inverse_lab/test_dense_map.py renders/diff_dense_128

# H2 free dense ground roughness / ORM.g (fixed albedo, MAPTEST + synthetic relight)
./build/inverse_fit --backend diff --dense-orm --dense-map-res 64 --dense-grid 4 \
  --preset lantern --out-dir renders/diff_dense_orm
python3 tools/inverse_lab/test_dense_orm.py renders/diff_dense_orm

# H2 free dense ground metallic / ORM.b (fixed albedo+rough; default G=2)
./build/inverse_fit --backend diff --dense-metal --dense-map-res 64 --dense-grid 2 \
  --preset lantern --out-dir renders/diff_dense_metal
python3 tools/inverse_lab/test_dense_metal.py renders/diff_dense_metal

# Daily-realistic HD plate: FIT optim + SHOW 720p/1080p stills (*_show.png)
./build/inverse_fit --backend diff --dense-orm --hd 720 --preset lantern \
  --out-dir renders/diff_dense_orm_hd720
./build/inverse_fit --backend diff --dense-metal --hd 720 --preset lantern \
  --out-dir renders/diff_dense_metal_hd720
./build/inverse_fit --backend diff --dense-orm --hd 1080 --preset lantern \
  --out-dir renders/diff_dense_orm_hd1080
```

## Ladder

| Level | Status |
|-------|--------|
| L0 scalar multi-param IR | ✅ |
| L1 multi-view capture + holdout/relight | ✅ |
| L2 PSNR/SSIM bar + train-only loss | ✅ |
| L3 denser UV maps / hero maps | partial — ground atlas UVs + dense map SoT; denser θ next |
| L4 Diff-IR (`--backend diff`, Deferred dense-map SoT) | ✅ bindless albedo SoT (DIFFTEST) |
| L5 Hybrid Diff-fit → PT light/tile refine → eval (`--backend hybrid`) | ✅ DIFFTEST + transfer; full LABTEST achievable |
| L6 / H1 free dense albedo (`--dense-map`) | ✅ MAPTEST 64² + 128²; in-place map upload (M1a–c) |
| L6 / H2 free dense roughness (`--dense-orm`) | ✅ MAPTEST + floor-crop + synthetic key-light relight (M2a) |
| L6 / H2 free dense metallic (`--dense-metal`) | ✅ MAPTEST + relight; G=2 checker-aligned free θ (M2b) |
| L6 / HD plates (`--hd 720\|1080`) | ✅ FIT optim + SHOW 720p/1080p stills |
| L6 / H2 multi-preset gallery (M3a) | ✅ lantern + helmet + spheres; `scripts/run_inverse_gallery.sh` |
| L6 / quality plate (publish bar) | ✅ `--quality-plate`: 1080p@20f, map128, hard presets (spheres/outdoor/helmet) |
| L6 / M3b ablation | ✅ views / map-res / hd / lab_fast table on spheres |
| L7 / H3 photo plate (M4a–b) | ✅ recipe + photo_proxy + PHOTOTEST (`docs/inverse_photo_lab.md`) |
| L8 / H4 analytic albedo (M5a–b) | ✅ GRADCHECK + linear solve + sparse FD optim (~6× vs full FD) |
| L8+ pure-analytic ≥10× / real_photo | → **roadmap** next |

### Quality bar (how we speak in public)

| Path | Resolution | Role |
|------|------------|------|
| lab_fast 256×144 | CI / iteration | Diagnostics only — **do not quote dB as product proof** |
| `--hd 720` | SHOW 1280×720 | Daily dev plate |
| **`--quality-plate`** | **SHOW 1920×1080 @20 frames**, map≥128, multi-view, hard presets | **Publish face** — stills must look clean; dB + map MSE together |

Hard presets for persuasion: **spheres** (metal chart), **outdoor** (HDRI), **helmet** (textured hero). Lantern alone is not enough.

**L4 note:** Diff-IR paints tile θ into a dense albedo map, binds it as Deferred-sampled bindless albedo (atlas UVs + `<actor>_albedo_0`), optimizes with coordinate FD from wrong init. Capture-gated holdout/relight bar (≥28/≥26/≥8) uses **`--backend pt`** (or hybrid Diff-fit + PT eval) because PT matches the capture export domain.

## Tests

- `tools/inverse_lab/test_metrics_and_maps.py` — gates on real `lab_metrics.json` + map MSE  
- `tools/inverse_lab/test_map_apply_diff.py` — export path writes differing init/GT maps  
- `tools/inverse_lab/test_dense_map.py` — H1 dense albedo MAPTEST  
- `tools/inverse_lab/test_dense_orm.py` — H2 dense ORM/rough MAPTEST + relight  
- `tools/inverse_lab/test_dense_metal.py` — H2 dense metallic MAPTEST + relight  
- `tools/inverse_lab/test_gallery.py` — M3a ≥3 presets MAPTEST + gallery wall  
- `tools/inverse_lab/test_quality_plate.py` — publish bar (≥2 hard presets, 1080p, map≥128)  
- `tools/inverse_lab/test_ablation.py` — M3b ablation table baseline green  
- `scripts/run_inverse_gallery.sh` — multi-preset matrix + HTML wall  
- `scripts/run_inverse_quality_plate.sh` — hard-scene 1080p plate  
- `scripts/run_inverse_ablation.sh` — views/map/hd/lab_fast matrix  
- `scripts/run_inverse_photo_plate.sh` — H3 multi-view photo_proxy + PHOTOTEST  
- `tools/inverse_lab/test_photo_plate.py` — honest photo gain gate  
- `tools/inverse_lab/test_dense_analytic.py` — H4 analytic GRADCHECK + MAPTEST  
- `docs/inverse_photo_lab.md` — capture recipe (real + proxy)  

```bash
# H4 analytic albedo GRADCHECK (then coord FD MAPTEST)
./build/inverse_fit --backend diff --dense-map --dense-map-res 64 --dense-grid 8 \
  --fit-width 256 --fit-height 144 --preset lantern --out-dir renders/diff_dense_analytic
python3 tools/inverse_lab/test_dense_analytic.py renders/diff_dense_analytic
```

```bash
# Fast CI-ish gallery (256×144) — not the product face
./scripts/run_inverse_gallery.sh --fast
# Daily 720p gallery wall
OUT_ROOT=renders/inverse_gallery_hd720 HD=720 ./scripts/run_inverse_gallery.sh
# Publish face (quote these stills + metrics)
./scripts/run_inverse_quality_plate.sh
# open renders/inverse_quality_plate/gallery_wall.html
```

## Non-goals (this track)

Public dataset SOTA, free geometry/camera, unmatched single-photo inverse.

See roadmap §0 / §6 for elongated non-goals and optional ambition tracks.
