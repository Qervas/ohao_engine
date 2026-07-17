# inverse_lab

Lab-grade inverse rendering track (multi-view capture → holdout / relight **PSNR/SSIM**).

## Quick start (frontier bar)

```bash
./build/inverse_fit --export-capture --preset lantern --quality draft --views 3 \
  --show-width 1280 --show-height 720 --show-spp 256 --fit-spp 64 --map-res 2 \
  --out-dir renders/inverse_lab/lantern_frontier

./build/inverse_fit --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --preset lantern --quality draft \
  --show-width 1280 --show-height 720 --show-spp 256 --fit-spp 64 \
  --iters 36 --multi-start 5 --visual-polish --polish-iters 8 --polish-spp-mul 4 \
  --out-dir renders/inverse_lab/lantern_frontier_fit 2>&1 | tee renders/inverse_lab/lantern_frontier_fit/run.log

python3 tools/inverse_lab/test_metrics_and_maps.py renders/inverse_lab/lantern_frontier_fit
python3 tools/inverse_lab/eval_bundle.py renders/inverse_lab/lantern_frontier_fit
```

**LABTEST** requires holdout PSNR ≥ 28 dB, relight ≥ 26 dB, holdout gain ≥ 8 dB vs wrong-init.

## Docs

- Architecture / ladder / measured numbers: [`docs/inverse_lab.md`](../../docs/inverse_lab.md)
- Schema: [`schema.md`](schema.md)

## Levels

| Level | What |
|-------|------|
| L1–L2 | Multi-view capture + map materials + PSNR/SSIM bar ✅ |
| L3 | Denser UV / hero maps |
| L4 | Differentiable renderer (Mitsuba / nvdiffrast) |
| L5 | Neural materials + public benchmarks |
