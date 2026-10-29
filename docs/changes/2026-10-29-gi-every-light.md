# RT GI's bounce sees every light

**Spec:** none (the renderer plan's "GI quality", Frank 2026-10-29: multi-bounce GI, measured by the referee).
**Status:** built. Gate 51 of 51. Goldens 32 of 32, seven moved (looked at). Budget miss: `cornell_720p` `rt.gi`, for Frank's ruling.

## 1. Summary

The first step toward closing the referee's gap (`docs/changes/2026-10-29-referee.md`). Before adding bounces, the one
bounce deferred has was checked against a path tracer limited to one bounce (a temporary switch, not kept). On Cornell
deferred was still at 0.70 of it, so the first bounce itself was short. One cause is fixed here: a GI ray's hit was lit by
the frame's **4 brightest lights** only. Cornell has 12 of equal intensity, so its walls bounced a third of their light.
Now a hit is lit by every light, at the same cost of one shadow ray.

The rest of the gap is not deferred's alone (section 5): the path tracer's indirect light is not yet a correct estimator,
so it is not yet a referee for bounce light.

## 2. What was built

- **`shaders/rt/gi_trace.slang` `directAt`:** a hit chooses one light with probability proportional to the luminance it
  would give unshadowed, traces one shadow ray to it and divides by that probability.
  - Up to `kGIWeighedLights` (16) lights, each is weighed, in one pass: a weighted reservoir, one random number rescaled at
    every light.
  - Past that, `kGILightCandidates` (8) candidates are drawn by power from an alias table (constant time each) and one is
    chosen among them by what it gives: resampled importance sampling, unbiased for any number of lights.
- **`RTGITechnique::render`:** every raster light but emissive proxies and lights that give nothing, in scene order. Each
  carries its share of the power (luminance of colour × intensity) in `GILight.radiance.w` and its slot of Vose's alias
  table in `cone.zw` (the keep probability, the alias as a uint's bits), built on the host in index order, so the same
  lights give the same table. The light buffer per frame slot grows to the count (it was fixed at 4).
- **`shaders/shared/layout.json`:** `kGIMaxLights` (4) is gone; `kGIWeighedLights` (16) and `kGILightCandidates` (8).
- **`tests/renderer/check_analytic.py`**, the bounce cases, at reference quality (full resolution, the running mean, no
  denoiser): a spot aimed at a wall lights the floor in front of it only by the bounce.
  - 6, 12 and 20 spots at one place, of intensity I/n each, must light the floor as one spot of I does, within 1%.
  - Twenty spots of twenty strengths spread along the wall must light it as the twenty do alone, within 1%. Twenty is past
    `kGIWeighedLights`, so this holds the drawn candidates to the sum.

## 3. Bugs found and fixed

**The bounce saw 4 lights.** With the old cap restored, the new cases fail as the cap predicts (realtime quality; the
spread case then had ten spots):

| Case | Old cap | Now |
|---|---|---|
| 6 spots at one place | ×0.6664 | **×1.0000** |
| 12 spots | ×0.3332 | **×1.0000** |
| 20 spots | ×0.1999 | **×1.0000** |
| spread spots vs. each alone | ×0.1835 | **×0.9998** (20 spots, reference) |

Drawing candidates by power while dividing by a uniform probability (a deliberate break) puts the spread case at ×1.1619:
it catches a biased resampling, which co-located equal lights cannot.

**Found on the way:**

- **The reservoir made NaNs.** The GPU's division can round `wl / wl` below 1, so with the random number above it the first
  light was not taken and nothing was chosen. `cli_helmet_game_reference` went black: 9 NaN pixels in 2 million, kept by
  reference quality's running mean, then spread by TAA and bloom. The first light that gives any is now always taken, a
  share that rounds to 1 too, and the number is held below 1 through its rescaling. The goldens caught it; the gate did not.
- **Weighing 8 candidates drawn from Cornell's 12 lights was visibly grainy** (one light chosen from 8 random draws, against
  every light weighed). Up to 16 lights are now weighed, which covers every reference scene.
- **The realtime denoiser is not linear.** At realtime quality twenty spots together lit the floor 1.5% over the sum of each
  alone; at reference, ×0.9998. Its neighbourhood clamp also leaves this flat floor 2.5% darker than reference (0.1998
  against 0.2050).

## 4. Results

**The referee** (`check_parity.py`, against the 4-bounce path tracer):

| Scene | Before | After |
|---|---|---|
| Cornell | 0.450, 2.80 stops | **0.476, 2.40** |
| Sponza | 0.620, 0.96 | **0.633, 0.93** |
| Helmet room, spheres room, area light, blockout | unchanged (4 lights or fewer) | |

Against a 1-bounce path tracer Cornell went from 0.696 to 0.735: the rest of the gap there is section 5's.

**Cost** (`render_bench`, a busy machine: Chrome used 1.6 cores; the previous commit timed in the same session):

| `rt.gi` | Before | After |
|---|---|---|
| `cornell_720p` (12 lights) | 0.112 ms | 0.128 ms (budget 0.11) |
| `bench10k_1080p` (1,000 lights) | 0.093 ms | 0.135 ms |
| `bench100k_1080p` (1,000 lights) | 0.146 ms | 0.229 ms |
| `play_hud_1080p` | 0.394 ms | 0.391 ms |

The candidates' first version searched cumulative shares (bench100k 0.513 ms); the alias table brought it to 0.229.
**`cornell_720p` `rt.gi` misses its 0.11 ms budget** (0.101 ms quiet before this step): a ruling for Frank.

**Goldens:** 7 moved, each looked at. Cornell's 4 (example and CLI, deferred and reference): walls, floor and the glass
sphere a little brighter, max_abs 47 on 97.4%. Sponza's 2: max_abs 34 and 40. `cli_transparency_deferred`: max_abs 2 on
18% (its four lights; the light is now chosen in one pass, the same expectation and other noise). Every other golden is
exact.

## 5. Known gaps

- **The path tracer's indirect light is not yet a referee.** On Cornell's near walls (rough, roughness 0.95, seen at a
  grazing angle) the path tracer's first bounce adds grey light, independent of the wall's colour: (0.19, 0.17, 0.15) on
  the red wall, whose direct light is 0.03. Its specular bounce is a heuristic: the mirror direction jittered by a cosine
  sample times the roughness, carried with a throughput of 1 (no Fresnel, no BRDF over pdf).
  - Its lobes are not deferred's either: Lambert against Burley, a Schlick-k Smith term against height-correlated Smith,
    and Fresnel to 1 at grazing against deferred's roughness-limited Fresnel. That last one is an IBL approximation
    applied to analytic lights; it keeps a rough surface's grazing Fresnel at 5%.
  - On a black floor (pure specular) under one light, seen 5° above it, the two disagree by ×0.78 (roughness 0.95) and
    ×3.1 (roughness 0.3).
- **Multi-bounce GI**: not built. It waits on the path tracer being a correct estimator, so the gap it closes is a real one.
- **Emission at a GI hit**: a ray that hits an emissive surface brings back nothing of its emission (the path tracer counts it).
- **Nothing in the gate looks for NaNs** in reference GI; the goldens found these.

## 6. How to run

```console
$ python tests/renderer/check_analytic.py
$ python tests/renderer/check_parity.py
```
