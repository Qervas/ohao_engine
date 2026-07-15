# Inverse rendering (Phase A → B3)

Recover scene parameters \(\theta\) so offline path tracing matches a target image.

**Studio θ (default)**:
- ground scalar PBR: albedo RGB + roughness + metallic  
- pedestal albedo RGB (second surface)  
- key light intensity  

**Cornell θ**: left-wall PBR + key intensity.

**Schedule**: staged FD — **materials first**, then **key light** (joint albedo↔light is ill-conditioned under HDRI).

## Dual budget

| Mode | Role | Default (`--quality high`) | Denoise |
|------|------|----------------------------|---------|
| **FIT** | FD gradients / loss only | 640×360 @ 128 spp | **Always none** (raw MC — white noise is a feature for inverse) |
| **SHOW** | Human-facing stills | **1920×1080 @ 1024 spp** | **OIDN by default** (grain-free) |

You never have to stare at FIT frames. Showcase files are SHOW quality and grain-free.

```bash
--show-denoise=oidn   # default: clean stills
--show-denoise=none   # raw SHOW (if you want noise in docs)
```

## Scenes

| `--scene` | Content | Optimizes |
|-----------|---------|-----------|
| **`studio` (default)** | Product shot: Lantern + pedestal + cyclorama + HDRI + lights | Ground PBR[5] + pedestal albedo[3] + key I[1] |
| `cornell` | Classic box | Left wall PBR[5] + key I[1] |

Flags: `--no-pedestal`, `--no-light`. Override hero with `--model PATH`. Multi-view SHOW; FIT uses dual-res draft budget.

## Run

```bash
# Default: studio helmet, multi-view, OIDN SHOW
./build/inverse_fit --selftest --scene studio --quality draft
./build/inverse_fit --selftest --scene studio --quality high

# Legacy box
./build/inverse_fit --selftest --scene cornell --quality draft

./build/inverse_fit --selftest --quality ultra    # 1080p @ 2048 spp stills
./build/inverse_fit --selftest --quality cinema   # 4K stills
```

Outputs under `renders/inverse/`:

- `target_show.png` / `target_<view>.png` — ground-truth stills  
- `init_show.png` — wrong initial parameters  
- `recovered_show.png` / `recovered_<view>.png` — after optimization  
- `recovered_relight.png` / `truth_relight.png` — same materials, different lighting  
- `trajectory.json` — loss / θ per iter  

## Pipeline

1. Build **studio** (Lantern product shot + HDRI) or cornell.  
2. Multi-view SHOW + FIT targets under truth PBR.  
3. Finite-difference + Adam on **multi-view masked MSE** (floor / wall band).  
4. SHOW recovered multi-view (OIDN).  
5. Relight: boost key light, re-render recovered vs truth.  

Selftest (composite): SHOW RMSE + key intensity error + soft param RMSE.

**No ML** in this path — pure physical image match. ML priors are a later phase.

**FIT never denoises** (white noise is a feature for inverse/FD). **SHOW uses OIDN** for grain-free stills.

## Seed

`--seed N` sets the path tracer sample base after each accumulation reset so FD probes are stable.

## Library

```
ohao/inverse/
  image_loss.hpp    # masked MSE
  param_space.hpp   # θ + FD gradient
  optimizer.hpp     # Adam
  quality.hpp       # draft/high/ultra/cinema
  inverse_module.hpp
```

## Implementation notes

- Env map loads **once**; later light-buffer rebuilds re-stamp `envMapTexIdx` (skip-reload).  
- Material θ updates use `updateRTMaterialParams()` (no BLAS rebuild) so multi-iter FD does not OOM.  
- Optimizer keeps **best-θ** by FIT loss (Adam can overshoot after the min).  

## Limits (not yet)

- Autodiff / adjoint path tracer  
- Multi-surface PBR / lighting / HDRI fit  
- **Textured** PBR maps (only uniform scalars)  
- Real photographs as targets  
- ML priors (deferred — physical IR first)  
