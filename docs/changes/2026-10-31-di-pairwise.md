# RT direct light: spatial reuse by pairwise MIS

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, step F4: ReSTIR DI against reference quality);
F4b's known gap (`docs/changes/2026-10-31-frontier-f4b.md`: the showcase settles about 2% dark).
**Status:** built.

## 1. Summary

**The showcase no longer settles dark, and reuse no longer breeds outliers.** Sponza at night's realtime frame settled
at 0.978-0.983 of reference quality. The cause was the engine's spatial reuse, not the scene. Under 1/Z (the M of the
inputs whose surface could have drawn the chosen point), a neighbour's point that lands far brighter here than where it
was drawn keeps that whole ratio as weight. Spatial output feeds the next frame's temporal reuse, which feeds the
neighbours' spatial reuse, so such a weight compounds: with the boiling filter off, a wall at the frame's edge reached
750x its light by frame 60, and the frame's mean 1.20 at frame 70. The boiling filter (drop a reservoir over 20x its 8x8
group's mean) caught these, and dropped legitimate bright points with them: the 2%. Spatial reuse is now pairwise MIS
(Bitterli 2022, thesis section 5.8; RTXDI's default), which bounds what a neighbour can give. Temporal reuse keeps 1/Z.

## 2. What was built

- `rt/restir_di.slang` `spatialReservoir`: this pixel's reservoir canonical, each of the k neighbours weighed against it
  alone, `m_i = (1/k) a_i / (a_i + b)`, `m_c = (1/k) sum_i b / (a_i + b)`, `a_i = k M_i p_i V_i`, `b = M_c p_c`; inputs
  stream in with `m p_c W`, `W = wsum / p_c`. `V_i` is a ray from the neighbour's surface to the canonical point (as
  1/Z's rays were, now for every pixel with a point, not only lit ones).
- `Combiner::addWeighted` (an input with a given resampling weight).
- The boiling filter stays (it now drops only what RIS itself makes; see 3).

## 3. How it was found (Sponza at night, 960x540, mean against reference quality)

| run | frame 30 | frame 60 | frame 120 | frame 200 |
|---|---|---|---|---|
| direct light alone (GI off), before | 0.989 | 0.980 | 0.981 | 0.984 |
| the same at full resolution | | | 0.991 | |
| the boiling filter off | 1.003 | 1.157 | 1.071 | 1.000 |
| pairwise MIS in both reuses, no filter | 0.996 | 0.991 | 0.990 | 0.993 |
| pairwise spatial, 1/Z temporal, no filter | 0.996 | 0.990 | 0.990 | 0.993 |
| **built: pairwise spatial, 1/Z temporal, the filter** | **0.994** | **0.987** | **0.987** | **0.991** |

- The 2% split: direct light 0.989 at frame 30 (1.010 at frame 1), GI's share (18% of the frame) about 0.957.
- Without the filter the excess sat in blobs (one 200x its reference at full resolution), and grew roughly 2x every few
  frames from about frame 50: a feedback, not one bad sample. Without spatial reuse, no blobs at 120 frames (0.994 full,
  0.988 half): the feedback runs through spatial reuse.
- Pairwise MIS in the temporal reuse too made the frame noisier (check_direct_light's relative MSE 0.045 against
  0.039, across frames 20-60): at half resolution last frame's reservoir is another pick of the block, and the balance
  swung toward this frame's 8 candidates. It stays 1/Z.
- Without `k` in `a_i`, this pixel kept half the weight against two neighbours (equal inputs should take a third each):
  relative MSE 0.046.
- Without the filter, golden captures showed speck clusters (Cornell's back wall, the helmet's visor at max_abs 166): RIS
  against an unshadowed target keeps a dim light's point with an occluded light's weight, which no reuse weighting
  prevents. With the filter back they are gone and the mean moved 0.003.

## 4. Results

- `check_direct_light`: frame 1 0.982, frame 30 0.988 (0.974 and 0.983 before), regions 0.963-1.056, relative MSE
  0.0380 (0.0391).
- **The referee** (`check_parity`, deferred/path tracer; after the GI denoiser fix in brackets): cornell_box 0.986
  (0.986), damaged_helmet **1.004** (0.982), khronos_metal_rough_spheres **0.997** (0.969), area_light 0.989 (0.989),
  khronos_sponza 1.016 (1.014), blockout_room 0.961 (0.960).
- `tools/check.py` 58/58; `check_analytic` 16/16; render goldens 33/33 after the 17 realtime deferred scenes were re-captured (the worst regions
  looked at: noise moved, a chrome highlight a pixel over on Cornell, max_abs 60).
- **Cost** (`render_bench` medians, a busy machine, old against new): helmet_game_1080p `di.trace` 0.33 to 0.41 ms,
  play_hud_1080p 0.13 to 0.15 ms: a ray a neighbour for every pixel with a point.

## 5. Known gaps

- Half resolution sits about 1% under full resolution on Sponza at night (0.987 against 0.995 at frame 120): a sample's
  visibility is its trace pixel's.
- GI's share of that frame is about 4% dark.
