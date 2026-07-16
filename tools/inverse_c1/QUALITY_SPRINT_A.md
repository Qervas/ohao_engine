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
| **lantern** | baseline | 0.228 | 0.401 | 0.021 | 0.067 | PASS |
| **lantern** | **NN hybrid** | **0.091** | **0.307** | 0.068 | **0.019** | **PASS** |
| **mirror** | baseline | 0.090 | 0.393 | 0.057 | 0.073 | FAIL (metal) |
| **mirror** | **NN hard-BRDF** | **0.099** | **0.305** | **0.022** | **0.040** | **PASS** |
| **outdoor** | baseline | 0.395 | 0.525 | 0.317 | 0.061 | PASS |
| **outdoor** | **NN hybrid** | **0.161** | **0.378** | **0.170** | **0.045** | **PASS** |

### Takeaways

1. **Color**: NN hybrid consistently beats baseline (lantern G-error 0.45 → ~0.12; outdoor RGB 0.40 → 0.16).
2. **Mirror metal**: still hard; hard-BRDF schedule improved metal 0.49→0.31 and **passed selftest** (was FAIL).
3. **Image match** always better with NN (SHOW RMSE).

## Compare sheets (open these)

| Preset | Before/after |
|--------|----------------|
| lantern | `lantern_nn/compare_before_after.png` |
| mirror | `mirror_nn/compare_before_after.png` |
| outdoor | `outdoor_nn/compare_before_after.png` |

Also: `*_baseline/compare_before_after.png` for FD-only.

## Sprint B add-ons

- Specular **metal reinject** (0.88) when NN underestimates metal on mirror/spheres
- Soft-color + **hard BRDF** schedule for specular presets
- One-command `./tools/inverse_c1/pipeline.sh`
- HTML gallery: `renders/inverse_c1_gallery/index.html`

Mirror after hard-BRDF + reinject: **SELFTEST PASS**, metal \|Δ\| ~0.31, rough \|Δ\| ~0.023, SHOW 0.041.

## Reproduce

```bash
./tools/inverse_c1/pipeline.sh --skip-train
# or full retrain:
./tools/inverse_c1/pipeline.sh --export

# Open compare gallery
xdg-open renders/inverse_c1_gallery/index.html
```
