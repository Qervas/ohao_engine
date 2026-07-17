# Inverse rendering (Phase A → B6)

Recover scene parameters \(\theta\) so offline path tracing matches a target image.
**Physical only** — no ML in the fit loop. `--export-dataset` prepares data for future ML.

## Current θ (studio default)

| Block | Dims | Content |
|-------|------|---------|
| Primary (ground) | 5 | albedo RGB + roughness + metallic |
| Pedestal | 3 | albedo RGB |
| Key / fill / rim | 3 | light intensities (normalized) |
| Env | 1 | HDRI intensity scale |
| **Total** | **12** | staged FD + multi-start |

**`--map-ground`**: primary becomes **14D** — 4× tile albedo RGB + shared rough/metal (2×2 floor). Total ≈ 21D with pedestal/lights.

**Cornell**: wall PBR[5] + key I[1].

**`--fit-exposure`** (with `--target-image`): optional extra exposure dim.

### Optimization schedule (B6)

1. **multi-start** — probe several inits (mid-gray, high-metal, low-metal, random); pick best loss  
2. **env** — HDRI scale (global brightness first)  
3. **exposure** — only if `--fit-exposure`  
4. **lights** — key + fill + rim (tighter bounds + light regularizer)  
5. **brdf_pre** — rough/metal *before* albedo when target is specular (mirror/spheres)  
6. **albedo** — primary RGB (or 4 tiles)  
7. **brdf** / **brdf2** — high-spp + specular-weighted loss  
8. **pedestal** — second-surface albedo  
9. **lights2** — re-fit lights after materials  
10. **refine** — joint polish (albedo + brdf + env + key)

Brightness (env/lights) before materials prevents albedo blowing to white under a dark init HDRI.  
BRDF-before-albedo on specular targets prevents the white-diffuse basin from stealing metal.

Multi-view FIT: view 0 weight 1.0, other views 0.5.

### Gap-close features (B6)

| Feature | Flag / behavior |
|---------|-----------------|
| Multi-start init | `--multi-start N` (default 5), `--no-multi-start` |
| Specular-weighted loss | highlight-biased hybrid MSE+MAE; `--specular-weight W` |
| High-spp BRDF stages | `--brdf-spp-mul M` (default 2; mirror uses 2.5) |
| Light regularizer | key-dominant hierarchy + soft mid-prior; `--light-reg W` |
| Tighter light bounds | reduces multi-light intensity trade-offs under HDRI |
| External photo target | `--target-image PATH` (+ `--exposure E` / `--fit-exposure`) |
| 2×2 ground albedo map | `--map-ground` |

## Dual budget

| Mode | Role | Default (`high`) | Denoise |
|------|------|------------------|---------|
| **FIT** | FD / loss | 640×360 @ 128 spp | always none |
| **SHOW** | Stills | 1920×1080 @ 1024 spp | OIDN |

## Run

```bash
./build/inverse_fit --selftest --preset lantern --quality draft
./build/inverse_fit --selftest --preset mirror --quality draft    # metal floor (multi-start + brdf_pre)
./build/inverse_fit --selftest --preset spheres --quality draft
./build/inverse_fit --selftest --map-ground --quality draft       # 2×2 ground tiles

# External LDR target (photo path, image-match gate only)
./build/inverse_fit --target-image photo.png --exposure 1.1 --quality high
./build/inverse_fit --target-image photo.png --fit-exposure --quality high

# Ablations
./build/inverse_fit --selftest --no-pedestal --no-rim --no-env --no-multi-start

# ML data factory
./build/inverse_fit --export-dataset 64 --quality draft --out-dir renders/inverse
```

### Presets

| `--preset` | Hero | Notes |
|------------|------|--------|
| `lantern` | Lantern | Baseline product studio |
| `helmet` | DamagedHelmet | Textured metal hero |
| `bottle` | WaterBottle | Glass/plastic — **tricky** |
| `spheres` | MetalRoughSpheres | Metal/rough chart — **tricky** |
| `toycar` | ToyCar | Dense mesh — **tricky** |
| `boombox` | BoomBox | Mixed materials |
| `outdoor` | Lantern + outdoor HDRI | Strong directional — **tricky** |
| `mirror` | Lantern + mirror floor | High metal floor — **tricky** |
| `chess` | ABeautifulGame | Large set — **tricky** |
| `cornell` | Cornell box | Fast regression |

## How far to ML?

| Phase | Status |
|-------|--------|
| A dual budget + OIDN SHOW | ✅ |
| B1–B2 scalar full PBR | ✅ |
| B3 multi-surface + key | ✅ |
| B4 multi-light + dataset export | ✅ |
| B5 rim + env + harden FIT | ✅ |
| B6 gap-close (multi-start, specular, photo, map) | ✅ |
| **C1 neural θ prior + FD refine** | ✅ **you are here** |
| C2 real-photo domain gap | later (path ready via `--target-image`) |
| C3 full-res texture / SVBRDF | research (`--map-ground` is the 2×2 pragmatic step) |

## C1 hybrid (unified θ + generalization ladder)

Physical FD alone often matches the **image** while **color θ is wrong** (albedo ↔ light ambiguity).  
C1 trains a CNN on **synthetic pairs with a single 12D studio θ layout**, then seeds FD.

### Not scene-locked — ladder of diversity

| Level | Diversity | Typical use |
|-------|-----------|-------------|
| L0 | θ only, one preset | smoke |
| L1 | multi-preset (same θ dims) | multi-hero |
| **L2** | + random cam/lights/hero | **default general prior** |
| L2e | + exposure jitter | photo-ish lite |

All levels share the **same θ vector** → one network, not one model per scene.

```bash
# Recommended: L2 export + train
QUALITY=draft LEVELS="L2" ./tools/inverse_c1/export_ladder.sh 128 renders/inverse_c1_ladder
python3 tools/inverse_c1/train.py \
  --data renders/inverse_c1_ladder/L2/dataset \
  --out tools/inverse_c1/checkpoints/L2 --epochs 80

# Hybrid selftest
./build/inverse_fit --selftest --preset lantern --quality draft \
  --nn-model tools/inverse_c1/checkpoints/L2/best.pt \
  --out-dir renders/inverse_c1_hybrid

# Full ladder (export+train+eval)
QUALITY=draft EPOCHS=60 ./tools/inverse_c1/run_ladder.sh 96
```

When NN init is already good (SHOW RMSE &lt; 0.08), FD runs a **soft refine** schedule only — full staged FD was walking off good albedo.

Details + measured tables: [`tools/inverse_c1/README.md`](../tools/inverse_c1/README.md).

## Lab track (multi-view / maps / PSNR)

Toward lab-grade inverse rendering (train-only multi-view, spatial maps, holdout + relight **PSNR/SSIM**):

- Architecture + measured bar: [`docs/inverse_lab.md`](inverse_lab.md)
- Tooling: `tools/inverse_lab/`
- CLI: `--export-capture` / `--lab-bundle` / `--map-res N`

**LABTEST** (synthetic lantern, capture-gated): holdout PSNR ≥ 28 dB, relight ≥ 26 dB, holdout gain ≥ 8 dB — all vs **exported** capture PNGs (`metric_gt=capture_export_images`).

```bash
./build/inverse_fit --export-capture --preset lantern --quality draft --views 3 \
  --show-width 1280 --show-height 720 --show-spp 256 --fit-spp 64 --map-res 2 \
  --out-dir renders/inverse_lab/lantern_frontier
./build/inverse_fit --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --preset lantern --quality draft \
  --show-width 1280 --show-height 720 --show-spp 256 --fit-spp 64 \
  --iters 36 --multi-start 5 --visual-polish \
  --out-dir renders/inverse_lab/lantern_frontier_fit
```

## Code layout

`examples/inverse_fit.cpp` is a **thin CLI** (~30 LOC). Pipeline lives under `ohao/inverse/`:

| Module | Role |
|--------|------|
| `fit_config.hpp` | `FitConfig`, presets, CLI (`parseArgs`) |
| `scene_builder.hpp` | `InverseScene` (studio / Cornell), θ apply, domain-rand |
| `io.hpp` | PNG load/save, θ JSON prior, highlight score |
| `render_session.hpp` | cached FIT/SHOW renders (mat/light-only updates) |
| `export_dataset.hpp` | C1 ML data factory (`runExportDataset`) |
| `staged_fit.hpp` | `StagedFitter`: loss, multi-start, Adam stages, schedule |
| `visual_polish.hpp` | high-spp pure-image polish + SHOW guard |
| `fit_engine.hpp` | orchestration: setup → fit → report (`runInverseFit`) |
| `image_loss.hpp` / `param_space.hpp` / `optimizer.hpp` / `quality.hpp` | core math + dual budgets |

Umbrella: `#include "inverse/inverse_module.hpp"`. Entry: `runInverseFit(cfg)`.

## Limits

- Autodiff / adjoint PT  
- Full-res textured PBR maps (2×2 tiles only)  
- Real-photo domain gap (tonemap/cam response) — path exists, not calibrated  
- Camera as free θ  
- ML priors (data factory ready)  
