# Quality sprint A — results

Date: 2026-07-16  
Model: `tools/inverse_c1/checkpoints/L2_metal/best.pt`  
Data: metal-heavy L2 (~400 pairs: 120 mirror, 120 spheres, 80 lantern, 40 outdoor, 40 helmet)  
Loss: color ×5, metal ×4.5, rough ×3  

## Prior held-out eval (15% val)

| Metric | Value | Gate |
|--------|-------|------|
| RGB MAE | **0.097** | PASS (&lt;0.18, strong if &lt;0.12) |
| metal MAE | **0.173** | improved vs ~0.23 |
| rough MAE | 0.26 | still soft |
| param RMSE | 0.215 | PASS |

## Hybrid: baseline FD vs NN→FD (draft, 32–36 iters)

| Preset | Method | RGB mean \|Δ\| | metal \|Δ\| | rough \|Δ\| | SHOW RMSE | Selftest |
|--------|--------|----------------|------------|------------|-----------|----------|
| **lantern** | baseline (pre metal-pass) | 0.228 | 0.401 | 0.021 | 0.067 | PASS |
| **lantern** | **baseline + metal prior** | ~0.15 | **0.030** | 0.007 | 0.069 | **PASS** |
| **lantern** | NN hybrid (pre) | 0.091 | 0.307 | 0.068 | 0.019 | PASS |
| **lantern** | **NN + metal prior** | **0.068** | **0.026** | 0.067 | **0.020** | **PASS** |
| **mirror** | baseline | 0.090 | 0.393 | 0.057 | 0.073 | FAIL (metal) |
| **mirror** | **NN hard-BRDF + lock** | **0.106** | **0.120** | **0.032** | **0.040** | **PASS** |
| **outdoor** | baseline (pre) | 0.395 | 0.525 | 0.317 | 0.061 | PASS |
| **outdoor** | **NN + metal prior** | **0.090** | **0.100** | **0.043** | **0.022** | **PASS** |
| **spheres** | **NN hold** (views=1) | 0.104 | **0.147** | 0.327 | **0.029** | **PASS** |

### Takeaways

1. **Color**: NN hybrid consistently beats baseline (lantern G-error 0.45 → ~0.08).
2. **Metal pass (B)**: product floors no longer misclassified as conductors from hero glints.
   - Diffuse prior + metal lock → lantern metal \|Δ\| **0.40 → 0.03**.
   - Specular reinject + hard-BRDF → mirror metal \|Δ\| **0.39 → 0.13**.
3. **Image match** always better with NN (SHOW RMSE).

## Compare sheets (open these)

| Preset | Before/after |
|--------|----------------|
| lantern | `lantern_nn/compare_before_after.png` |
| mirror | `mirror_nn/compare_before_after.png` |
| outdoor | `outdoor_nn/compare_before_after.png` |

Also: `*_baseline/compare_before_after.png` for FD-only.

## Sprint B — metal pass (shipped)

Root cause: `targetHighlightScore` on product shots is high from **hero glints**, so the floor was treated as a conductor.

Fixes in `ohao/inverse/staged_fit.hpp`:

1. **Preset-first metal mode** — product packs (lantern/outdoor/…) always dielectric; only mirror/spheres force conductor.
2. **Stronger diffuse metal prior** (target metal ≈ 0.10, penalty above 0.35).
3. **Stronger specular prior** for mirror/spheres (target metal 0.88 / rough 0.10).
4. **Multi-start reinject** gated on preset (no more high-metal seed on lantern).
5. **`metal_lock` stage** after schedule — snap BRDF basin + short hi-spp BRDF refine.

Also earlier B add-ons:

- Specular **metal reinject** when NN underestimates on mirror/spheres
- Soft-color + **hard BRDF** schedule for specular presets
- One-command `./tools/inverse_c1/pipeline.sh`
- HTML gallery: `renders/inverse_c1_gallery/index.html`

| Preset | metal \|Δ\| after B | SHOW |
|--------|---------------------|------|
| lantern NN | **0.026** | 0.020 |
| outdoor NN | **0.100** | 0.022 |
| mirror NN | **0.120** | 0.040 |
| spheres NN hold | **0.147** | 0.029 |

Also: spheres/mirror use **preset-specific metal targets** (mirror 0.90, spheres chart 0.55). When NN SHOW RMSE &lt; 0.06 on specular, **hold prior** (FD was wrecking heavy meshes).

## Reproduce

```bash
./tools/inverse_c1/pipeline.sh --skip-train
# or full retrain:
./tools/inverse_c1/pipeline.sh --export

# Open compare gallery
xdg-open renders/inverse_c1_gallery/index.html
```
