# Frontier F4b: direct light at half resolution

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 2, "Direct light": MegaLights' structure).
**Follows:** `docs/changes/2026-10-30-frontier-f4.md` (its section 6: four gated frame budgets missed; this step is
the second option offered there).
**Status:** built; the gated frame budgets are still missed (smaller gaps), a ruling for Frank (section 6).

## 1. Summary

**ReSTIR DI resamples and traces at half resolution and shades at full**, as MegaLights does: one pixel of each 2x2
block, picked by a hash each frame, draws the candidates, reuses last frame's and its neighbours' reservoirs and traces
the shadow rays; every pixel then shades two of its four nearest samples through its own material. A quarter of the
rays; the frame's mean is closer to reference than F4's was (0.996 against 0.883 at frame 1 on the showcase), its
noise once converged about the same.

**A showcase and its test**: `assets/scenes/sponza_night.scene.json` (Sponza at night, 32 coloured lamps under the
arcades and the 5 atrium lamps, no sun, no sky) and `tests/renderer/check_direct_light.py`, which holds the realtime
frame to reference quality on it.

## 2. What was built

- **Passes** (41 registered): `di.sample` (half: candidates, temporal reuse, a visibility ray, the boiling filter),
  `di.trace` (new; half: spatial reuse, the final shadow ray, the boiling filter again), `di.shade` (full: every pixel
  shades two of its 4 nearest trace pixels' samples, drawn by their bilinear weights among those on its surface, with
  OpenPBR's exact lobes), `di.denoise` (full).
- **`render.settings rtDirect.resolution`** (`half`, the default, or `full`); `GIResolution` is `TraceResolution`,
  shared with `rtGI.resolution`. Reference quality is full resolution without reuse, as before.
- **`rtDirect.candidates` defaults to 8** (sampling at half resolution costs a quarter as much).
- The images stay the frame's size; half resolution uses their top-left quarter, so switching quality never
  reallocates. A changed resolution drops the reservoirs' history.
- **The denoiser** stops its a-trous kernel by luminance (SVGF, Schied et al. 2017): the mean square luminance of each
  signal accumulates with the frames (`m_moments`, RGBA32F), the history's alpha holds the deviation, and a tap further
  from the centre than 4x the larger of the two pixels' deviations weighs less, once a pixel has 4 frames. The firefly
  clamp is gone.
- **Pipeline statistics**: `OHAO_PIPELINE_STATS=1` enables `VK_KHR_pipeline_executable_properties` and every compute
  pipeline `rtc::createComputePipeline` makes logs the driver's statistics (register count, ...).

## 3. Bugs found and fixed

- **A grazing sample lit its neighbour 100x over.** A trace pixel's W is about 1 / its own target; shaded at a pixel
  where the target is far larger (a column's sample on the ceiling beside it), it made a blob 300x the reference.
  A sample now shades at most 4x the target it had at its trace pixel (1x from another surface).
- **Bright blobs at half resolution survived the firefly clamp**: an outlier sample shades a blob about 4 pixels wide,
  and a 3x3 clamp sees its neighbours as bright too. The boiling filter now runs after the spatial reuse as well (a
  spatial combination made a spike that temporal reuse then carried: 330x reference at frame 60).
- **The frame darkened 2% over 60 frames.** Temporal reuse's Z counted last frame's M without a visibility ray; at full
  resolution last frame's surface was the same pixel, at half it is another pick of the block. A ray from it: 0.991 of
  reference at frame 60 (was 0.981), DI alone.
- **The denoiser darkened the frame 3%** with SVGF's stop on the centre's deviation alone: a bright pixel (high
  deviation) took its dark neighbours while they (low deviation) refused it. The larger of the two deviations makes it
  symmetric.
- **A fade of the wide a-trous steps by history** (tried first) sharpened Sponza's shadows but left a soft penumbra
  noisy at 16 frames (2.56 levels from reference; F4's test fails over 2): replaced by the luminance stop.
- **`di.shade` ran at a third of the GPU's occupancy** (128 registers): its inline ray query (a ray for pixels whose
  taps are on other surfaces) cost 32 registers, the view's table lookups hoisted out of the loop 16 more. Neither is
  there now (72 registers); pixels without a tap on their surface bound each sample to 1x its own target instead. And a
  loop over the 4 trace pixels ran four evaluations in every warp whatever each lane drew: it loops over the two draws.

## 4. Spec corrections

- Shading is at full resolution and reads two of the 4 nearest trace pixels' samples; a sample's visibility is its
  trace pixel's (MegaLights' downsampled tracing). The spec's "resample and trace at half resolution" holds.

## 5. Results

- **The showcase** (`sponza_night`, 1280x720, the whole deferred frame with RT GI, linear radiance against 512 frames
  of reference quality; the lamps' spheres left out):

  | frame | F4: mean | F4: relative MSE | F4b: mean | F4b: relative MSE |
  |---|---|---|---|---|
  | 1 | 0.883 | 0.060 | 0.996 | 0.086 |
  | 4 | 0.939 | 0.032 | 0.988 | 0.047 |
  | 30 | 0.964 | 0.025 | 0.984 | 0.029 |
  | 60 | | | 0.981 | 0.030 |

  `check_direct_light` (960x540, 256 reference frames): frame 1 x1.009, frame 30 x0.980, regions x0.942-x1.016,
  relative MSE 0.0350 (limits 3%, 8%, 0.04).
- **Highlights**, converged (30 realtime frames against 256 of reference quality, the reference's brightest 0.5% of
  pixels and 2 px around them): Cornell x0.988 (F4 x0.991), the helmet x1.000 (F4 x1.000).
- **Cost** (`render_bench`, medians; both runs noisy: the GPU calibration over 25% of its reference, other processes
  busy):

  | Scene | F4 DI | F4b DI (sample / trace / shade / denoise) | F4 frame | F4b frame | budget |
  |---|---|---|---|---|---|
  | cornell_720p | 1.565 | 1.188 (0.164 / 0.137 / 0.259 / 0.628) | 2.430 | 2.078 | 1.50 |
  | helmet_game_1080p | 3.554 | 2.445 (0.364 / 0.345 / 0.516 / 1.220) | 5.800 | 4.725 | 5.00 |
  | play_hud_1080p | 2.308 | 1.607 (0.263 / 0.143 / 0.256 / 0.945) | 4.757 | 3.924 | 2.45 |
  | play_vfx_1080p | | 1.402 (0.190 / 0.138 / 0.258 / 0.816) | 3.918 | 3.128 | 2.79 |

  Registers (`OHAO_PIPELINE_STATS`): initial 72, spatial 96, shade 72, temporal 40, atrous 47.
- **Tests:** `engine_tests` 86/86 (the F4 drift test prints the signed bias beside the mean difference: frame 16 +0.38
  levels); `tools/check.py` 57/57; render goldens 33/33: the 24 deferred re-captured with notes (the sample sequence:
  half resolution, 8 candidates, the denoiser), the 8 path-traced unchanged, `cli_sponza_night_deferred` new. CLI and
  serve expectations: the schema hash and `render.settings`' `rtDirect` (candidates 8, resolution).

## 6. Known gaps

- **Three gated frame budgets are still missed** (cornell_720p 2.08 / 1.50, play_hud_1080p 3.92 / 2.45, play_vfx_1080p
  3.13 / 2.79; helmet_game_1080p now within its 5.00). The denoiser is the largest part (0.63 ms at 720p: a temporal
  pass and three a-trous passes at full resolution). For Frank: rule the costs into the budgets.
- **Early frames are noisier than F4's** (relative MSE 0.086 against 0.060 at frame 1) with a quarter of the rays;
  converged the two are alike, and the mean is much closer from the first frame.
- **About 2% dark** once converged on the showcase (0.981 at frame 60; the boiling filter and the shade bound), and the
  helmet scene's light outside its highlights x0.902 of reference (F4 x0.916).
- **A shadow edge is resolved at half resolution** (a sample's visibility is its trace pixel's), softened by the
  jittered pick and the accumulation.
- **The denoiser keeps luminance stopping off for a pixel's first 4 frames** (its variance is too poor): a disocclusion
  is filtered without it.

## 7. How to run

```console
$ python tests/renderer/check_direct_light.py > /dev/null   # gpu
$ OHAO_PIPELINE_STATS=1 build/Release/ohao run assets/scenes/cornell_box.scene.json --render build/doc_tmp/stats.png --mode deferred --frames 1 > /dev/null   # gpu
```
