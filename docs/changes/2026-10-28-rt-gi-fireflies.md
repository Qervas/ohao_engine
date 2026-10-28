# RT GI fireflies under an outdoor sky

**Spec:** none (a defect fix; ROADMAP row 16, moved there from row 9 by Feel step 3). The
environment-light rules are in `AGENTS.md`, "A directional light stands for the sun".
**Status:** fixed (2026-10-28), after Frank's hand test of `blockout_play.scene.json`.

## 1. Summary

Frank saw soft white blobs across the play scene's sunlit walls and on the first-person gun,
and only outdoors. They came from the outdoor HDRI's sun:
- **What it is:** 7 texels over 1,000, peaking at 60,450 against a median sky of 0.215. They carry
  half the map's power on 0.003% of the sphere.
- **How it caused the blobs:** RT GI's two cosine rays a pixel found the sun about once in 10^4
  rays and brought back tens of thousands. Its accumulation and denoiser spread each hit into a
  soft disc.
- **Counted twice:** the scene's directional light already is the sun, with RT shadows, so the
  environment's sun was also a double count.

The fix separates them:
- **The rule:** while a scene has a directional light, it stands for the sun, and the HDRI is
  uploaded with its sun painted out.
- **The result:** RT GI's rays, the deferred IBL and the path tracer light with the sky alone, and
  the sun arrives once, from the light.

On the gate's view, outliers fell from 441 to 88, and the remaining 88 sit along silhouettes. The
mean image luma moved by at most 0.3 levels. Five goldens moved, the scenes with this HDRI and a
directional light, and were re-baselined after looking. The other 25 are exact.

The diagnosis also found a second defect, left for Frank. Every equirect lookup reads the HDRI
upside down, and the path tracer's importance sampling disagrees with its own lookups (section 6).

## 2. Diagnosis: what was proved, and how

All captures are headless `ohao serve` captures of the play scene, 1280x720, with `taa` and
`bloom`. Each was taken at 30 frames from a reset (converged), and at 4 frames (what a moving camera
shows: GI's history is that young after every move). Scripts are in `build/fireflies/`.

**The metric** (`gi_outliers.py`, the same arithmetic as the new gate):
- Capture each view with RT GI on and off.
- Take the luma difference: what GI adds.
- Count the pixels where it exceeds its 5x5 median by more than 12 levels.
- Edges and textures common to both images cancel, and GI's smooth light is removed by the median.
  What remains are GI's isolated spikes.

**The HDRI** (`hdr_stats.py`, on `assets/test_models/env_outdoor.hdr`, 1024x512):

| Texels over | Count | Solid angle | Share of the map's power |
|---|---|---|---|
| 10 | 377 | 9.4e-3 sr | 52.4% |
| 100 | 22 | 5.6e-4 sr | 49.9% |
| 1,000 | 7 | 1.8e-4 sr | 48.6% |

The peak is 60,450 (row 119: elevation +48° in the file). The 99.9th percentile is 7.9 and the
median 0.215. The studio HDRI peaks at 518 (softboxes), with no sun.

**Experiments on five views** (GI outliers, 30 frames). Each variant uses a modified copy of the
HDRI with the same scene:

| Variant | spawn north | spawn south | spawn west | middle north | middle west |
|---|---|---|---|---|---|
| as shipped | 304 | 138 | 450 | 189 | 463 |
| sun painted out (169 texels > 20) | 33 | 6 | 23 | 183 | 119 |
| HDRI flipped upright | 1,097 | 100 | 1,207 | 1,278 | 1,254 |
| flipped and sun painted out | 67 | 15 | 46 | 242 | 140 |

What these prove:
- **The source is the HDRI's sun:** painting it out removes the spikes, and nothing else in the
  frame changed.
- **The engine's lookup is upside down** (section 6). The file's sun sits 48° *below* the engine's
  horizon. Only sunward vertical faces and the gun, whose cosine rays point down past the room's
  floor, could see it: hence "only outdoors", on walls and the gun, not on floors.
- **With the sky upright, the same sun hits every floor:** spikes rise 3–4x. The orientation fix
  alone would have made the fireflies much worse.
- **The residuals are not fireflies.** The spikes left after the fix (middle north 183, middle west
  119) sit along the enemies' silhouettes, where GI's half-resolution upsample meets an edge
  (`spikes_nosun_v5.png`).

**Ruled out:**
- **The denoiser and SSR.** Feel step 3 showed SSR, RT shadows, post and full-resolution GI leave
  them; RT GI off removes them. Here the sky alone removes them with the denoiser unchanged.
- **NaN or infinity.** The spikes have finite values that scale with the HDRI's.
- **The view model's depth range or motion vectors.** The gun's spikes go with the sun, the same as
  the walls'.
- **A pdf mismatch in GI.** GI's estimator is plain cosine sampling, and its expectation is right;
  its variance is what is unbounded.

**Why a plain clamp is not the answer:** Feel step 3 measured a luminance clamp of 16 on escaped GI
rays. It removed the speckles but moved 10 goldens by up to 82 levels and took the sun's reflection
off metals in studio scenes. It also dims any legitimately bright sky. The cause was a delta-like
light in the environment that the scene also lights analytically, so the fix separates the two.

## 3. The fix

**`ohao/render/rt/env_sun.{hpp,cpp}`: `separateEnvSun`.** It works relative to the sky's median
luminance m (a texel median, which the sun cannot move):
- **A sun exists** only if the peak is at least 10^4 m. The outdoor HDRI is 281,000 m; the studio
  HDRI's softboxes are 1,700 m and are lights the environment keeps.
- **The sun's texels** are the peak and every texel 8-connected to it, across the u seam, over 64 m.
  The reason for 64: GI's estimate averages about 64 samples (2 rays over the up-to-32 frames it
  accumulates), so one sample over 64 m adds more than the sky's own radiance to it.
- **It must be compact:** at most 0.02 sr, a cone of about 4.6°. A bright sky patch is not a sun.
- **Painting out:** its texels are set to the mean of the texels bordering it.
- **What it reports:** texels, solid angle, the removed irradiance and its direction (the engine's
  lookup convention), and its share of the map's power.

**`RenderScene::hasDirectionalLight()`.** The renderer reads it at the top of
`VulkanRenderer::render()`. The environment is uploaded once per (path, sunless) pair, so a
directional light added or removed reloads the map.

**Upload** (`gpu/vulkan/env_upload.cpp`): while sunless, the sun is painted out of the pixels before
anything uses them:
- the GPU image (GI's misses, `pt_miss`);
- `EnvPrefilter` (the deferred specular and irradiance);
- the path tracer's importance-sampling CDF.

So every consumer agrees and the sun is counted once. The `gpu` channel logs what was taken out. On
the play scene:

> env map: a directional light stands for the sun; …env_outdoor.hdr lights without its sun (237
> texels, 5.98e-03 sr, 51.3% of its power, peak 60450 over a median sky of 0.215, toward (0.554,
> -0.742, 0.378))

The direction is the sun in the engine's (flipped) lookup: 48° below the horizon. That matches the
diagnosis.

No shader changed, and nothing new runs per frame. The separation is one pass over the map when it
loads.

## 4. Tests

- **`env_sun_test`** (CPU, 6 tests, `quick`):
  - A three-texel sun found and painted with the sky around it, with its irradiance, direction and
    power share.
  - A sun across the u seam is one sun.
  - Studio softboxes (500 on 0.3) are no sun, and the map is untouched.
  - A wide 1e4 patch is no sun.
  - The sun's dim halo goes with it, and an equally bright unconnected texel stays.
  - Empty and black maps.
  - The first test failed once on a 64x32 map, where three texels exceed the 0.02 sr cap; the
    synthetic maps are 256x128.
- **`check_fireflies.py`** (GPU, `gpu`, registered in `cmake/ohao_tests.cmake`):
  - **View and metric:** the play scene from the spawn, looking west, 4 frames after a reset, GI on
    against off, with the metric above.
  - **Before the fix:** 441, failing the gate of 150, and no log record.
  - **After:** 88, plus the log record. Gate 150.

## 5. Results

**Look shots** (`build/fireflies/`, same script and views before and after, `shots.py`). GI
outliers at 30 frames / 4 frames, and mean luma:

| View | Before | After | Mean luma before → after |
|---|---|---|---|
| `outdoor_gun` (the gun against the pillar's sunward face) | 192 / 222 | 4 / 29 | 134.49 → 134.66 |
| `outdoor_open` (the west wall, open sky) | 166 / 294 | 93 / 184 | 119.37 → 119.61 |
| `outdoor_spawn_w` (the spawn, west) | 448 / 539 | 26 / 118 | 129.33 → 129.13 |
| `indoor` (the shade side, away from the sun) | 7 / 11 | 7 / 6 | 125.81 → 125.53 |
| `indoor_helmet` (`helmet_game`, studio HDRI) | – | identical bytes | 78.67 → 78.67 |

Notes on the table:
- **What remains after the fix** in `outdoor_open` and `outdoor_spawn_w` is along the enemy's and
  the rifle's silhouettes (`spikes_after_open_f4.png`), not on walls.
- **The left-edge strip of white blobs** in `before_outdoor_spawn_w.png` is gone in the after shot.
- **The largest single-pixel changes** (up to 160) are the enemy visor's mirror reflection of the
  HDRI sun, which the directional light's own highlight now stands in for. Indoor changed by 7
  pixels over 40 levels, the same visor.

Files:
- `before_/after_` × `outdoor_gun`, `outdoor_open`, `outdoor_spawn_w`, `indoor`, `indoor_helmet`;
- `_f4` for the 4-frame captures, `_nogi` for the references;
- `before_metrics.json` and `after_metrics.json`.

**Goldens:** 25 of 30 exact. Five moved, each a scene with `env_outdoor.hdr` and a directional
light, re-baselined in their own commit after looking (`golden_energy.py`):

| Golden | max_abs | Pixels changed | Mean luma | Where |
|---|---|---|---|---|
| `cli_khronos_sponza_deferred` | 37 | 10.9% | +0.03% | GI firefly blobs baked into the old golden gone; 1-level noise elsewhere |
| `cli_khronos_sponza_reference` | 34 | 10.4% | +0.03% | the same |
| `cli_blockout_room_deferred` | 4 | 17.7% | +0.03% | low-level GI and IBL change |
| `cli_blockout_play_deferred` | 94 | 24.5% | −0.09% | the enemy visor's reflection of the HDRI sun; low-level elsewhere |
| `cli_blockout_play_hud` | 41 | 24.5% | −0.08% | the same |

Indirect energy is not visibly lost: means move ±0.1%, and the 99.9th percentiles are unchanged.

**Validation** (`check_validation.py`, `OHAO_VALIDATION=1`): 14/14 clean through teardown,
including the play scene in deferred, `rt_offline`, `rt_realtime` and `rt_realtime` through NRD.

**Gate** (`python tools/check.py`): 44 of 45 passed, `env_sun_test` and `check_fireflies` among them.
The failure was `monograph_structure`: two units cited the `render()` lines this fix rewrote
(`architecture/invariants`, `gpu/scene-upload`). Their cites and prose were updated, and
`scene-upload` now explains the sun separation; after that it passed 16/16. `check_docs` passed
with the two new suites' commands in `AGENTS.md`.

**Bench costs: NOISY, not a verdict.** No shader and no per-frame work changed, so the frame cannot
cost more. The separation runs once per map load, on the CPU. The benches ran while
`cod25-cod.exe` used 4.4 cores and the GPU:

| Run | Calibration | `play_hud_1080p` frame (median / p95) | `rt.gi` | `rt.gi.denoise` |
|---|---|---|---|---|
| Full `render_bench` | GPU ×8.5 | 2.994 / 41.5 ms | 0.380 ms | 0.417 ms |
| Rerun, that scene only | GPU ×1.91 | 4.488 / 6.49 ms | 0.495 ms | 0.524 ms |

The last quiet measurement, from the SP10 step 4 gate, before this fix: `play_hud_1080p` frame 2.269
ms. No budget was moved.

## 6. Open items for Frank

- **The HDRI is read upside down by every equirect lookup.** `gi_trace.slang`,
  `deferred_lighting.frag`, `env_prefilter.slang`, `pt_miss.rmiss` and `cinematic_composite.comp`
  all use `v = asin(y) / pi + 0.5`, with texel row 0 at v = 0. stb_image loads the file top row
  first and nothing flips it, so +Y reads the file's last row, the ground: a standard HDRI's sky
  lights from below.
  - **The path tracer disagrees with itself:** its importance-sampling CDF
    (`env_sampling.glsl`, `env_cdf.cpp`) puts +Y on row 0. Its light samples aim at texels other
    than the ones its lookups return.
  - **The fix is one convention everywhere.** It moves every HDRI-lit golden (helmets, spheres,
    sponza, blockout) and changes how every studio scene is lit.
  - **Recommended:** fix it as its own change, with the goldens re-baselined after looking.
- **No sun to take out without a directional light.** A scene with an HDRI sun and no directional
  light keeps the sun in its environment, with GI's variance. The principled completion is
  next-event estimation of the separated sun in GI: one shadow ray toward it a traced pixel, the
  same expectation, bounded variance. No checked-in scene needs it today.
- **The sun disc in the path tracer's own sky:** with a directional light, camera rays that miss see
  the sky without the HDRI's sun disc. The deferred background is the procedural sky, whose disc is
  unchanged.

## 7. How to run

```
build/Release/env_sun_test.exe
python tests/renderer/check_fireflies.py
python build/fireflies/shots.py after          # the look shots and their metrics
python tests/golden/render_golden.py tests/golden/manifest.json
```
