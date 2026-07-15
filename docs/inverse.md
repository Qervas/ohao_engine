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
| **B6 gap-close (multi-start, specular, photo, map)** | ✅ **you are here** |
| C1 neural residual on θ | **next for ML** |
| C2 real-photo domain gap | later (path ready via `--target-image`) |
| C3 full-res texture / SVBRDF | research (`--map-ground` is the 2×2 pragmatic step) |

**ML can start now** (synthetic pairs via `--export-dataset`).  
**Recommended C1:** image → Δθ prior, refine with 5–15 physical FD iters (hybrid).

## Limits

- Autodiff / adjoint PT  
- Full-res textured PBR maps (2×2 tiles only)  
- Real-photo domain gap (tonemap/cam response) — path exists, not calibrated  
- Camera as free θ  
- ML priors (data factory ready)  
