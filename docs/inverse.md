# Inverse rendering (Phase A → B5)

Recover scene parameters \(\theta\) so offline path tracing matches a target image.
**Physical only** — no ML in the fit loop. `--export-dataset` prepares data for future ML.

## Current θ (studio default)

| Block | Dims | Content |
|-------|------|---------|
| Primary (ground) | 5 | albedo RGB + roughness + metallic |
| Pedestal | 3 | albedo RGB |
| Key / fill / rim | 3 | light intensities (normalized) |
| Env | 1 | HDRI intensity scale |
| **Total** | **12** | staged FD |

**Cornell**: wall PBR[5] + key I[1].

### Optimization schedule (B5)

1. **env** — HDRI scale (global brightness first)  
2. **lights** — key + fill + rim  
3. **albedo** — primary RGB  
4. **brdf** — roughness + metallic  
5. **pedestal** — second-surface albedo  

Brightness (env/lights) before materials prevents albedo blowing to white under a dark init HDRI.

Multi-view FIT: view 0 weight 1.0, other views 0.5.

## Dual budget

| Mode | Role | Default (`high`) | Denoise |
|------|------|------------------|---------|
| **FIT** | FD / loss | 640×360 @ 128 spp | always none |
| **SHOW** | Stills | 1920×1080 @ 1024 spp | OIDN |

## Run

```bash
./build/inverse_fit --selftest --scene studio --quality draft
./build/inverse_fit --selftest --scene studio --quality high

# Ablations
./build/inverse_fit --selftest --no-pedestal --no-rim --no-env
./build/inverse_fit --selftest --no-light

# ML data factory
./build/inverse_fit --export-dataset 64 --quality draft --out-dir renders/inverse
```

## How far to ML?

| Phase | Status |
|-------|--------|
| A dual budget + OIDN SHOW | ✅ |
| B1–B2 scalar full PBR | ✅ |
| B3 multi-surface + key | ✅ |
| B4 multi-light + dataset export | ✅ |
| **B5 rim + env + harden FIT** | ✅ **you are here** |
| C1 neural residual on θ | **next for ML** |
| C2 photo targets | later |
| C3 texture / SVBRDF fields | research |

**ML can start now** (synthetic pairs via `--export-dataset`).  
**Recommended C1:** image → Δθ prior, refine with 5–15 physical FD iters (hybrid).

## Limits

- Autodiff / adjoint PT  
- Textured PBR maps  
- Real photographs  
- Camera as free θ  
- ML priors (data factory ready)  
