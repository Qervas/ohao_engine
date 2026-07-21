# OHAO Inverse Lab — research results
Measured on this machine from gated lab runs. **Metric domains are labeled**; do not mix capture-gated PT dB with Deferred dense-map dB as one leaderboard.

## Protocol honesty
- PT LABTEST uses capture-exported holdout/relight PNGs (not live-oracle theater).
- Diff dense MAPTEST uses wrong-init cool/high-rough/low-metal vs GT maps; beauty via bindless Deferred.
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
| Preset | Init→Train PSNR | ΔPSNR | Rough map MSE | Relight Δ | Pass |
|--------|-----------------|-------|---------------|-----------|------|
| spheres | 38.3→58.1 | **+19.8** | 0.362→0.179 | +19.9 | ✅ |
| outdoor | 32.4→56.9 | **+24.5** | 0.362→0.194 | +24.5 | ✅ |
| helmet | 35.8→57.6 | **+21.8** | 0.362→0.081 | +21.8 | ✅ |

## T3 — Dense albedo / metal MAPTEST (Deferred studio)
| Task | Init→Train PSNR | ΔPSNR | Map MSE | Relight Δ |
|------|-----------------|-------|---------|----------|
| Albedo 64² free-grid | 17.6→25.0 | +7.3 | 0.104→0.084 | — |
| Albedo 128² | 17.6→24.9 | +7.2 | 0.104→0.080 | — |
| Roughness ORM.g (lab) | — | +21.0 | 0.378→0.169 | +22.1 |
| Metallic ORM.b | 16.6→43.4 | **+26.8** | 0.405→0.006 | +26.9 |

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

Machine-readable: [`RESULTS.json`](RESULTS.json)
