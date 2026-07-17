# Render pipelines — multi-pipeline Vulkan foundation

## North star

One Vulkan host, many image-formation pipelines. Inverse rendering picks a **fit backend** and an **eval backend** (often Diff fit → PathTrace eval).

```
Scene / θ / cameras
        │
 Vulkan foundation (device, upload, bindless, offscreen, seed)
        │
 ┌──────┼──────────┬─────────────┐
 ▼      ▼          ▼             ▼
Forward Deferred  Diff-IR      PathTrace
        (raster)  (fwd+grads)  (oracle / FD)
```

| Mode | Role |
|------|------|
| **Deferred** | Realtime / offline raster, GBuffer |
| **PathTrace (RTOffline)** | Physical stills, inverse FD, lab relight oracle |
| **Diff-IR** | Vulkan Deferred studio-mesh raster; tile θ → dense map → bindless GBuffer albedo SoT (`--backend diff`); map PNG export |

## Art of the code (LOC law)

| Rule | Limit |
|------|-------|
| Soft target | ≤ **350** lines per `.hpp` / `.cpp` |
| Hard cap | **500** lines per TU |
| Ban | No new **1k+** files |
| Host | `renderer.cpp` — **wiring only** for new pipelines |

Diff lives under `ohao/render/diff/` as many small C++20 units. Inverse backends under `ohao/inverse/backend/`.

## Inverse backends

| `--backend` | Formation | Grads |
|-------------|-----------|-------|
| `pt` (default) | Path tracer via `RenderSession` | Finite differences |
| `diff` | Diff-IR (`ohao/render/diff/*`) | Tile θ → dense map → Deferred bindless SoT + FD |
| `hybrid` | Diff fit → PT eval | Same as diff for fit; capture-gated holdout/relight under PT |

### Showcase commands

```bash
# Diff-IR albedo inverse (DIFFTEST)
./build/inverse_fit --backend diff --preset lantern --quality draft \
  --show-width 320 --show-height 180 --iters 40 --out-dir renders/diff_demo

# Hybrid: Diff fit → PT capture-gated holdout/relight (DIFFTEST + LABTEST)
./build/inverse_fit --backend hybrid --preset lantern --quality draft \
  --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --show-width 640 --show-height 360 --show-spp 128 \
  --out-dir renders/inverse_lab/lantern_hybrid

# Capture-gated PT lab plate (LABTEST)
./build/inverse_fit --backend pt --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --preset lantern --quality draft --show-width 640 --show-height 360 --show-spp 128 \
  --iters 28 --multi-start 5 --visual-polish --out-dir renders/inverse_lab/lantern_frontier_fit
```

Lab protocol (capture-gated PSNR/SSIM) is backend-agnostic: see `docs/inverse_lab.md`.

## Related

- `docs/inverse.md` — inverse product overview  
- `docs/inverse_lab.md` — multi-view / holdout / relight bar  
- Renovation Phase 0 — determinism contract for offline stills  
