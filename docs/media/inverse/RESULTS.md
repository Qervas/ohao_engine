# OHAO Inverse Lab — research results
Measured on this machine from gated lab runs. **Metric domains are labeled**; do not mix capture-gated PT dB with Deferred dense-map dB as one leaderboard.

## Protocol honesty
- PT LABTEST uses capture-exported holdout/relight PNGs (not live-oracle theater).
- Diff dense MAPTEST uses wrong-init cool/high-rough/low-metal vs GT maps; beauty via bindless Deferred.
- **Dense "relight" is a 2.5× scale of the same key light** — not novel illumination. No environment
  swap, no light moved or added, and the dense paths do not fit lights at all. It is a weak
  perturbation (see the caveat under T3) and must not be read like the PT capture-exported relight.
- Quality plate: FIT 960×540, SHOW 1920×1080 @20f, map≥128, multi-view, hard presets.
- lab_fast 256×144 is diagnostic only — do not quote as product proof.
- Analytic speedup is vs estimated full 3-pass coordinate FD (same loss eval cost model).

## T1 — PT capture-gated LABTEST (lantern frontier)
| Metric | Value |
|--------|-------|
| Metric domain | `capture_export_images` |
| Holdout PSNR / SSIM | **32.5 dB** / 0.983 |
| Relight PSNR / SSIM | **34.4 dB** / 0.989 |
| Wrong-init holdout PSNR | 11.9 dB |
| Holdout gain vs wrong init | **+20.5 dB** |
| Train RMSE before → after | 0.2986 → 0.0195 |

## T2 — Diff-IR quality plate (dense roughness, 1080p SHOW)
| Preset | Init→Train PSNR | ΔPSNR | Rough map MSE | ~~Relight Δ~~ | Pass |
|--------|-----------------|-------|---------------|-----------|------|
| spheres | 38.3→58.1 | **+19.8** | 0.362→0.179 | ~~+19.9~~ withdrawn | ✅ |
| outdoor | 32.4→56.9 | **+24.5** | 0.362→0.194 | ~~+24.5~~ withdrawn | ✅ |
| helmet | 35.8→57.6 | **+21.8** | 0.362→0.081 | ~~+21.8~~ withdrawn | ✅ |

> **T2 relight column withdrawn 2026-07-25.** These runs predate the relight fix below, so their
> "relight" figures are duplicate *training-light* measurements (that is why each one lands within
> 0.15 dB of its own ΔPSNR). The quality plate has not been re-run since the fix; the ΔPSNR / map-MSE
> columns are unaffected by it, the relight column is not trustworthy and is not replaced by a guess.

## T3 — Dense albedo / metal MAPTEST (Deferred studio)
| Task | Init→Train PSNR | ΔPSNR | Map MSE | Key×2.5 Δ |
|------|-----------------|-------|---------|----------|
| Albedo 64² free-grid | 17.6→25.0 | +7.3 | 0.104→0.084 | — |
| Albedo 128² | 17.6→24.9 | +7.2 | 0.104→0.080 | — |
| Roughness ORM.g (lab) | 38.1→43.7 | +5.5 | 0.378→0.250 | +3.4 |
| Metallic ORM.b | 19.8→47.9 | **+28.1** | 0.405→0.0016 | +28.0 |

**T3 correction, 2026-07-25 — two separate defects, both now fixed in the numbers above.**

1. *The relight metric measured the training light.* `dense_orm_fit.hpp` / `dense_metal_fit.hpp`
   boosted `keyLight` by 2.5× and then issued a forward with `force=true`, whose first statement
   `inv.applyTruth()` re-drove the key intensity from the θ source-of-truth back to the training
   value. Both the "relit" target and the "relit" candidate were therefore rendered under the
   training light. Verified before the fix: `orm_relight_truth.png` was **byte-identical** to
   `orm_forward_truth.png` (md5 match, mean-abs-diff 0.000000), and `relight_init_psnr` /
   `relight_recovered_psnr` were *exactly* `init_psnr` / `train_psnr`. Fixed by scaling
   `InverseScene::truthKeyI` (the source `applyTheta` reads) inside an RAII `RelightScope` that also
   asserts the intensity actually held; see `ohao/inverse/dense_common.hpp`.
2. *The train / map columns were stale.* The previously published values (+21.0 / +26.8 dB) came from
   `renders/diff_dense_{orm,metal}/*_metrics.json` files written **before** commit `3557cfb`, which
   changed both fits — provable from their schema (no `fit_wh`/`preset`/`quality_plate` keys;
   `metal_prior_w: 0.002` where the code now has `0.0`). `make_readme_figures.py` re-read those stale
   files without re-running the fits. Every T3 number above is a fresh run on current `HEAD`.

**Honest caveat on the size of this relight.** A 2.5× key boost is a *weak* perturbation here: the
loss-cropped floor band is already near the ACES tonemap ceiling, so the relit truth frame is only
~0.4 % brighter in mean pixel value (0.8176 → 0.8210 on the crop) and sits 36.9 dB from the training
frame. It is a real second lighting condition, but it is nowhere near a novel-illumination test.

Reproduce (current `HEAD`, FIT 640×360 / SHOW 1920×1080):
```bash
./build/inverse_fit --backend diff --dense-orm   --dense-map-res 64 --dense-grid 4 --preset lantern \
  --out-dir renders/diff_dense_orm
./build/inverse_fit --backend diff --dense-metal --dense-map-res 64 --dense-grid 2 --preset lantern \
  --out-dir renders/diff_dense_metal
```

## T4 — Analytic albedo optim (H4/M5)
| Metric | Value |
|--------|-------|
| GRADCHECK median rel err vs FD | 0.198 (pass < 0.20) |
| Analytic optim wall-clock | 2166 ms |
| Est. full 3-pass coord FD | 42769 ms |
| Speedup | **19.7×** |
| MAPTEST ΔPSNR / map MSE | +2.8 dB / 0.104→0.082 |
| Claim boundary | Agreement with FD is GRADCHECK (not reverse-mode autodiff claim). |

## T5 — Photo proxy PHOTOTEST
| Metric | Value |
|--------|-------|
| Domain | `photo_proxy_images` |
| Holdout PSNR | 28.7 dB |
| Wrong-init holdout | 13.8 dB |
| **Holdout gain** | **+14.9 dB** |
| Relight PSNR | 27.4 dB |
| Note | Gain-based PHOTOTEST; synthetic LABTEST bar not applied under domain shift. |

## Figures
- `docs/media/inverse/readme_quality_matrix.jpg`
- `docs/media/inverse/readme_quality_relight.jpg`
- `docs/media/inverse/readme_pt_frontier.jpg`
- `docs/media/inverse/readme_dense_albedo.jpg`
- `docs/media/inverse/readme_dense_metal.jpg`
- `docs/media/inverse/readme_photo_proxy.jpg`
- `docs/media/inverse/readme_analytic.jpg`
- `docs/media/inverse/readme_museum.jpg`

Machine-readable: [`RESULTS.json`](RESULTS.json)
