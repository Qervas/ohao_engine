# Inverse Lab — frontier multi-view / novel-view / relight protocol

**Status:** L1–L2 **pass** on synthetic lantern studio capture (capture-gated PSNR/SSIM bar).

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
| L5 neural materials + public benchmarks | planned |

**L4 note:** Diff-IR paints tile θ into a dense albedo map, binds it as Deferred-sampled bindless albedo (atlas UVs + `<actor>_albedo_0`), optimizes with coordinate FD from wrong init. Capture-gated holdout/relight bar (≥28/≥26/≥8) uses **`--backend pt`** (or hybrid Diff-fit + PT eval) because PT matches the capture export domain.

## Tests

- `tools/inverse_lab/test_metrics_and_maps.py` — gates on real `lab_metrics.json` + map MSE  
- `tools/inverse_lab/test_map_apply_diff.py` — export path writes differing init/GT maps  

## Non-goals (this track)

Public dataset SOTA, free geometry/camera, unmatched single-photo inverse.
