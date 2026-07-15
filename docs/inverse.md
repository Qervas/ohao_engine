# Inverse rendering (Phase A → B4)

Recover scene parameters \(\theta\) so offline path tracing matches a target image.
**Physical only** — no ML in the fit loop (yet). The renderer is also a **data factory** for future ML.

## Current θ (studio default)

| Block | Dims | Content |
|-------|------|---------|
| Primary (ground) | 5 | albedo RGB + roughness + metallic |
| Pedestal | 3 | albedo RGB |
| Key light | 1 | intensity (normalized) |
| Fill light | 1 | intensity (normalized) |
| **Total** | **10** | staged FD |

**Cornell**: wall PBR[5] + key I[1].

### Optimization schedule

1. **primary** — ground/wall PBR  
2. **pedestal** — second-surface albedo  
3. **lights** — key + fill intensities  

Joint material↔light FD is ill-conditioned under HDRI (albedo trades with brightness). Staging fixes that.

## Dual budget

| Mode | Role | Default (`--quality high`) | Denoise |
|------|------|----------------------------|---------|
| **FIT** | FD gradients / loss | 640×360 @ 128 spp | **Always none** (raw MC) |
| **SHOW** | Human stills | **1920×1080 @ 1024 spp** | **OIDN** (grain-free) |

## Run

```bash
# Selftest (optimize + stills)
./build/inverse_fit --selftest --scene studio --quality draft
./build/inverse_fit --selftest --scene studio --quality high

# Ablate blocks
./build/inverse_fit --selftest --no-pedestal
./build/inverse_fit --selftest --no-light
./build/inverse_fit --selftest --no-fill

# ML data factory: N random (θ, FIT image) pairs
./build/inverse_fit --export-dataset 64 --quality draft --out-dir renders/inverse
# → renders/inverse/dataset/00000.png … + meta.jsonl
```

Outputs: `target_*`, `init_show`, `recovered_*`, `*relight*`, `trajectory.json`.

## How far to ML?

| Phase | Status | What it unlocks |
|-------|--------|-----------------|
| **A** Dual budget + OIDN SHOW | ✅ | Clean demos, usable FD |
| **B1–B2** Scalar full PBR | ✅ | Uniform metal/rough, not just color |
| **B3** Multi-surface + key | ✅ | Product-scene inverse |
| **B4** Multi-light + dataset export | ✅ (this) | Fill light; synthetic pairs for training |
| **B5** More coverage (rim, env scale, multi-view FIT harden) | next | Stronger physical baseline |
| **C1** Neural residual / prior on θ | **ML starts here** | Faster / more stable init; photo ambiguity |
| **C2** Photo targets + domain gap | later | Real images, not self-rendered |
| **C3** Map / SVBRDF / NeRF-style fields | research | Textures, geometry-from-images |

**Practical answer:** we can start **using ML any time after B4** — specifically once you have:

1. A stable physical loop that can **label** data (we do: `--export-dataset`)  
2. A decision on **what the network predicts** (θ residual? env? rough/metal prior?)  
3. Training outside this binary (PyTorch/etc.), then a C++ load path or export

**Recommended ML first slice (C1):**  
Train a small MLP/CNN: `image features → Δθ` (or init θ), then **refine with the existing physical FD** for 5–15 iters. Hybrid = reliable + fast.

**Not ready for ML yet if you mean:** end-to-end replace path tracing, or fit full textures from one photo without a prior — that needs C2–C3 and more data.

Distance estimate:

- **~1 more physical milestone (B5)** before ML is *worth* it for quality  
- **ML can start in parallel now** using `--export-dataset`  
- **Full photo-to-PBR product** is several milestones after C1  

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

- Env map loads **once**; light rebuild re-stamps `envMapTexIdx`.  
- Materials: `updateRTMaterialParams()` (no BLAS thrash).  
- Lights: `updateRTLightParams()`.  
- Continuous metallic in path tracer (not binary 0.5 threshold).  
- Best-θ per stage.

## Limits (not yet)

- Autodiff / adjoint path tracer  
- **Textured** PBR maps  
- Real photographs as targets  
- HDRI / camera as free θ  
- ML priors (C1 — data factory ready)  
