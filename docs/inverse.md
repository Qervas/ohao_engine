# Inverse rendering (Phase A)

Recover scene parameters \(\theta\) so offline path tracing matches a target image.

## Dual budget

| Mode | Role | Default (`--quality high`) |
|------|------|----------------------------|
| **FIT** | FD gradients / loss only | 640×360 @ 128 spp |
| **SHOW** | Human-facing stills | **1920×1080 @ 1024 spp** |

You never have to stare at FIT frames. Showcase files are always SHOW quality.

## Run

```bash
./build/inverse_fit --selftest --quality high     # default product quality
./build/inverse_fit --selftest --quality draft    # faster while developing
./build/inverse_fit --selftest --quality ultra    # 1080p @ 2048 spp stills
./build/inverse_fit --selftest --quality cinema   # 4K stills
```

Outputs under `renders/inverse/`:

- `target_show.png` — ground-truth high-quality render  
- `init_show.png` — wrong initial parameters  
- `recovered_show.png` — after optimization  
- `trajectory.json` — loss / θ per iter  

## Pipeline

1. Build synthetic Cornell-like scene (left wall albedo = optimizable RGB).  
2. SHOW-render truth → `target_show.png`.  
3. FIT-render truth → internal target for loss.  
4. Finite-difference + Adam (or `--gd`) on masked MSE (left wall).  
5. SHOW-render recovered → `recovered_show.png`.  

**Denoisers stay off** in the FIT path (determinism). Optional pretty denoise is not mixed into gradients.

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

## Limits (not yet)

- Autodiff / adjoint path tracer  
- Multi-parameter materials / lighting fit  
- Real photographs as targets  
