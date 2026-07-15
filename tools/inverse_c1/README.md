# C1 — Neural θ prior (unified studio θ, generalization ladder)

Hybrid pipeline:

```
synthetic dataset  →  CNN (image → θ)  →  physical FD refine (inverse_fit)
```

**Unified θ layout** is always **studio 12D** (same names/dims across levels).  
What changes per level is **how diverse the images are** — not a different inverse problem per scene.

## Generalization ladder

| Level | What varies | Export flags | Goal |
|-------|-------------|--------------|------|
| **L0** | θ only (single preset, fixed cam/lights) | default | Baseline |
| **L1** | θ + multi-preset (lantern/outdoor/mirror/helmet/spheres) | multi-preset merge | Unified multi-hero |
| **L2** | L1 + cam orbit, light positions, hero yaw | `--domain-rand --export-views random` | **Best general studio prior** |
| **L2e** | L2 + LDR exposure jitter | + `--export-exposure-jitter` | Photo-ish domain gap (lite) |

All levels share:

```
primary.RGB, rough, metal, pedestal.RGB, key/fill/rim I, env.scale
```

So one network can be trained on L2 and used for any studio preset fit.

### Measured quality

**Prior alone (held-out, L2 HQ ~400 samples, larger CNN):**

| Level | RGB MAE | param RMSE | Gate |
|-------|---------|------------|------|
| L0 | 0.22 | 0.25 | WEAK |
| L1 | 0.21 | 0.24 | WEAK |
| L2 (240) | 0.16 | 0.22 | PASS |
| **L2 HQ (~400)** | **0.085** | **0.21** | **PASS (strong RGB)** |

Gates: RGB MAE &lt; 0.18 (strong &lt; 0.12) and RMSE &lt; 0.28.

**Hybrid FD (lantern draft selftest):**

| Method | primary \|Δ\|RGB | SHOW RMSE | metal \|Δ\| |
|--------|------------------|-----------|------------|
| Baseline FD (wrong init) | (0.06, **0.45**, 0.17) | 0.067 | 0.40 |
| **NN prior → soft FD** | **(0.07, 0.06, 0.06)** | **0.021** | 0.30 |

When the prior already matches well (SHOW RMSE &lt; 0.08), `inverse_fit` uses **soft refine only** so full staged FD cannot destroy NN color.

## Quick start (recommended = L2)

```bash
# Build exporter
cmake --build build --target inverse_fit -j

# Export full ladder (or just L2)
QUALITY=draft LEVELS="L2" ./tools/inverse_c1/export_ladder.sh 128 renders/inverse_c1_ladder

# Train
python3 tools/inverse_c1/train.py \
  --data renders/inverse_c1_ladder/L2/dataset \
  --out tools/inverse_c1/checkpoints/L2 \
  --epochs 80 --width 128 --height 72

# Eval (held-out)
python3 tools/inverse_c1/eval.py \
  --model tools/inverse_c1/checkpoints/L2/best.pt \
  --data renders/inverse_c1_ladder/L2/dataset

# Hybrid FD refine
./build/inverse_fit --selftest --preset lantern --quality draft \
  --nn-model tools/inverse_c1/checkpoints/L2/best.pt \
  --out-dir renders/inverse_c1_hybrid
```

One-shot ladder:

```bash
QUALITY=draft EPOCHS=60 ./tools/inverse_c1/run_ladder.sh 96 renders/inverse_c1_ladder
```

## Manual export flags

```bash
# L2 sample
./build/inverse_fit --export-dataset 256 --preset lantern --quality draft \
  --domain-rand --export-views random --export-exposure-jitter \
  --out-dir renders/my_export
```

| Flag | Effect |
|------|--------|
| `--domain-rand` | Random cam orbit, light positions, hero yaw per sample |
| `--export-views primary\|random\|all` | View selection |
| `--export-exposure-jitter` | Random LDR exposure gain on PNG |

## Infer + FD

```bash
python3 tools/inverse_c1/infer.py \
  --model tools/inverse_c1/checkpoints/L2/best.pt \
  --image target.png --out theta_prior.json

./build/inverse_fit --selftest --theta-init theta_prior.json --no-multi-start
# or
./build/inverse_fit --selftest --nn-model tools/inverse_c1/checkpoints/L2/best.pt
```

## Before / after comparison images

After every `inverse_fit` run (if Python is available), sheets are written into the out-dir:

| File | Content |
|------|---------|
| `compare_before_after.png` | Target \| Init \| Recovered \| \|Diff\|×5 |
| `compare_target_recovered.png` | Target \| Recovered |
| `compare_multiview.png` | Per-view target / recovered / diff |

Manual rebuild:

```bash
python3 tools/inverse_c1/make_compare.py renders/inverse_c1_compare/lantern_nn
python3 tools/inverse_c1/make_compare.py   # also builds baseline vs NN sheet if present
```

Headline sheet from last hybrid compare:

`renders/inverse_c1_compare/COMPARE_baseline_vs_nn.png`  
(target | bad init | baseline FD | NN hybrid)

## What is *not* generalized yet

| Missing | Notes |
|---------|--------|
| Free camera as θ | Cam is randomized in L2 export but not recovered |
| Arbitrary room geometry | Still product-studio layout |
| Cornell box | Different θ dims — train separate or pad later |
| Real photos (full C2) | L2e is synthetic exposure only |

## Files

| Path | Role |
|------|------|
| `export_ladder.sh` | L0–L2e dataset factory |
| `run_ladder.sh` | export → train → eval all levels |
| `train.py` / `eval.py` / `infer.py` | ML loop |
| `model.py` | Color-aware CNN (mean-RGB residual on albedo) |
