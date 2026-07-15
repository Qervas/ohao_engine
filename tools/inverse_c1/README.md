# C1 — Neural θ prior for inverse rendering

Hybrid pipeline:

```
synthetic dataset  →  CNN (image → θ)  →  physical FD refine (inverse_fit)
```

The network learns a **good color / material / light init**.  
`inverse_fit` still owns image-consistent polish (no end-to-end PT replacement).

## 1. Export dataset

```bash
# Single preset (quick)
./build/inverse_fit --export-dataset 256 --preset lantern --quality draft \
  --out-dir renders/inverse_c1_lantern

# Multi-preset merge (recommended)
chmod +x tools/inverse_c1/export_dataset.sh
./tools/inverse_c1/export_dataset.sh 128 renders/inverse_c1_data
# → renders/inverse_c1_data/dataset/{config.json, meta.jsonl, 00000.png, ...}
```

Each sample is a random θ + FIT-resolution path-traced PNG.

## 2. Train

```bash
python3 tools/inverse_c1/train.py \
  --data renders/inverse_c1_data/dataset \
  --out tools/inverse_c1/checkpoints \
  --epochs 40 --batch 32
```

Writes `tools/inverse_c1/checkpoints/best.pt` (+ `best_meta.json`).

Albedo RGB dims are **3× loss-weighted** so color is prioritized over lights.

## 3. Infer prior

```bash
python3 tools/inverse_c1/infer.py \
  --model tools/inverse_c1/checkpoints/best.pt \
  --image renders/some/target_show.png \
  --out renders/some/theta_prior.json
```

## 4. FD refine with NN init

```bash
./build/inverse_fit --selftest --preset lantern --quality draft \
  --theta-init renders/some/theta_prior.json \
  --no-multi-start \
  --out-dir renders/inverse_c1_hybrid
```

Or use the wrapper (two-pass: targets → infer → refine):

```bash
chmod +x tools/inverse_c1/hybrid_fit.sh
./tools/inverse_c1/hybrid_fit.sh --preset lantern --quality draft
```

## Layout of θ (studio default 12D)

| Indices | Name |
|---------|------|
| 0–2 | primary albedo RGB |
| 3–4 | rough, metal |
| 5–7 | pedestal RGB |
| 8–10 | key / fill / rim I scales |
| 11 | env scale |

Must match the export preset flags (`--map-ground` changes dims).

## Tips

| Goal | Setting |
|------|---------|
| Better color | more samples (1k–8k), multi-preset export |
| Faster train | draft FIT, 96×54 net input (default) |
| Mirror metal | include `mirror` / `spheres` in export |
| Photo path | `--target-image` + infer on photo + `--theta-init` |

## Not C1 yet

- Real-photo domain gap (C2)
- Full-res texture / SVBRDF fields (C3)
- Differentiable path tracer
