# Ambient kill: walls lit by RT GI only

**Spec:** none. This follows Frank's rulings of 2026-10-26 (Berlin, t67u), after the SSR
composition fix (`5bb8b13`):
- No constant ambient floor on walls; RT GI, real lights and emissives provide the fill.
- Decide and document what happens without RT GI.
- Report the helmet ratio, Cornell's wall brightness against the path tracer, and the RT
  GI wall noise.
- Keep every budget value.

**Status:** Built and tested.
- The constant ambient is now only the fallback of a frame with neither RT GI nor an
  environment.
- With RT GI, the walls are lit by direct light and its bounce only.
- Helmet/left-wall ratio: 0.534 with SSR on and 0.546 off, against the path tracer's 0.534.

## 1. Summary

The hypothesis was right, and the code has exactly one such term.
- `deferred_lighting.frag` added `ambientIntensity · albedo · (1 − metallic) · ao · ssao`
  to every pixel, whatever else lit it.
- Its value is 0.35 with scene lights. With the fallback sun it is 0.12 by day and 0.04
  by night (`buffer_setup.cpp`; `setAmbientIntensity` overrides it).
- On top of RT GI's bounce it put a flat floor of light under every wall. The path
  tracer has no such term: its miss returns black without an environment.
- No other path adds a constant ambient. The path tracer, RT GI, the sky and particles
  were checked.

The rule as built: indirect diffuse light has one source per frame.
1. RT GI's rays, when they ran.
2. Otherwise the environment's irradiance (the IBL diffuse term), when an environment
   is bound.
3. Only with neither: the constant ambient. This case is no `VK_KHR_ray_query`, or
   `render.settings` `rtGI` off, and no environment.

The fallback stays so that such a frame does not go black in its shadows. With an
environment but no RT GI, the irradiance term was already the fill, so the constant was
a double fill there too and is dropped as well.

## 2. What was built

- **`core/deferred_lighting.frag`:** the ambient line is now
  `if (rtGI) ambient += gi·albedo·(1−metallic)·ao; else if (!ibl) ambient += ambientIntensity·…`.
  `ibl` (flag bit 0) is hoisted and shared with the IBL block.
- **Comments:** `VulkanRenderer::setAmbientIntensity` and
  `DeferredLightingPass::setLights` now say the value is a fallback.
- **Tests (`engine_tests`):**
  - `sp2b_t05`'s ambient-only runs turn RT GI off, since the ambient is only a fallback
    now. Its point-lit runs keep RT GI.
  - New test `ambient_is_a_fallback_only_without_rt_gi`: with RT GI on, an ambient of 0.6
    changes no pixel against 0; with RT GI off, it brightens over 10,000 pixels.
    `engine_tests` passes 85/85.

## 3. Bugs found and fixed

- The constant ambient was applied on top of RT GI and on top of the environment's
  irradiance (section 1).

## 4. Spec corrections

There is no spec. Monograph units updated:
- `hybrid/gi-technique`: "The bounce the ambient term cannot know" and "Where the
  bleed lands".
- `deferred/lighting`, `deferred/ssao`, `deferred/post`, `shaders/core-gbuffer-lighting`.

AGENTS.md has a new gotcha on the one-source rule, and the RT GI paragraph is updated.

## 5. Results

**Helmet/left-wall ratio.** The metric is the same box metric as the SSR change
(`helmet_game.scene.json`, deferred, 30 frames, TAA, SSAO and bloom, 1920×1080 through
`ohao serve`); the path tracer is the `damaged_helmet` golden.

| Render | SSR on | SSR off | Left wall (luma) |
|---|---|---|---|
| Before (`5bb8b13`) | 0.471 | 0.473 | 0.645 |
| Ambient fallback only (as built) | 0.534 | 0.546 | 0.522 |
| Path tracer | 0.534 | | 0.610 |

The re-baselined goldens agree: `helmet_game_deferred` 0.534, `damaged_helmet_deferred`
0.535, and the wall 0.646 → 0.523.
- The ratio now matches, but the wall is darker than the path tracer's. RT GI traces
  one bounce; the path tracer traces all of them.
- Before, the flat fill stood in for the missing bounces, and the helmet was
  correspondingly too dark against it.

**Cornell** (`cornell_box.scene.json`, 640×360, deferred 30 frames; mean luma of each
wall region):

| Render | Back wall | Red wall | Green wall |
|---|---|---|---|
| Deferred before | 0.581 | 0.138 | 0.239 |
| Deferred as built | 0.360 | 0.085 | 0.125 |
| Path tracer (1024 spp) | 0.330 | 0.298 | 0.352 |

- The back wall moved to within 0.03 of the path tracer.
- The red and green walls cannot be compared. The path tracer's are barely coloured: the
  red wall's median is (0.39, 0.26, 0.21) against deferred's (0.46, 0.04, 0.04).
- A grey speckle covers the whole path-traced frame. It does not fall from 512 to 1024
  spp, and `--denoise oidn` leaves it as it is (section 6).

**RT GI noise on flat walls.** The helmet room's left wall, measured by
`build/wall_noise.py`:

| Measure | Before | After |
|---|---|---|
| Fine grain (deviation from a 9×9 local mean) | 0.39% of the mean | 0.72% |
| Blotches (std of 16×16 block means, plane removed) | 4.9% | 10.3% |

- In absolute terms the blotches went from 0.032 to 0.054.
- Both numbers are upper bounds: they include the wall's stucco texture and real light
  falloff.
- The flat fill used to hide the denoiser's residual. It is visible now as grain and
  low-frequency mottling on the walls.
- It is RT GI's realtime output (half resolution, 2 rays, à-trous, temporal), not a
  composition bug, so it is deferred.

**Goldens.** 16 entries moved, from 11 images. Each was looked at before its
`--update --only` (only the moved scenes were updated), and a full run afterwards passed
22/22. Difference from the old goldens:

| Scene | max_abs | pixels | rmse |
|---|---|---|---|
| cornell_box_deferred / _reference (+cli) | 151 | 98.8% | 52.7 / 52.5 |
| damaged_helmet_deferred (+cli), helmet_game_deferred | 157 | 97.1% | 46.4 / 46.2 |
| helmet_game_reference | 158 | 96.9% | 44.8 |
| khronos_metal_rough_spheres_deferred / _reference | 148 / 146 | 97.4% | 56.5 / 54.7 |
| khronos_flight_helmet_deferred / _reference | 138 / 139 | 98.1% | 50.5 / 48.7 |
| khronos_sponza_deferred / _reference | 89 | 99.9% | 19.3 |

The path-traced goldens are unchanged.

Gates:
- `python tools/check.py`: 37/37, `engine_tests` 85/85 among them.
- Render goldens: 22/22 on a full run after the update.
- Monograph structure: OK.

Benchmarks, one run each. Both are **noisy** (CPU calibration more than 25% over its
reference, other processes at 54–58% of a core), so neither is a verdict. No budget
value was changed.
- **`render_bench`:** every budgeted measurement is under its value.
  - cornell_720p: frame 0.987 ms (1.5), lighting 0.281 ms (0.32).
  - helmet_game_1080p: frame 2.041 ms (5.0).
- **`serve_bench`:** window_capture_rt_inline 122.4 ms (150).

The change removes a multiply-add from lighting; no pass was added.

## 6. Known gaps

- **Walls are darker than the path tracer** in the helmet room (0.52 against 0.61).
  RT GI is one bounce. More bounces (or a radiance cache) would close it; a constant
  would not.
- **RT GI's noise is visible on flat walls** (section 5). Options are a stronger denoiser
  or more rays, a performance trade-off.
- **The path-traced Cornell is not usable as ground truth.** Its walls are desaturated
  under a speckle that more spp and OIDN do not remove. It also renders the right
  sphere as reflective where deferred shows white diffuse.
- **The path tracer renders FlightHelmet all chrome** (from the previous change), so
  FlightHelmet still has no ground truth.
- **The fallback ambient's value is unchanged:** 0.35, or the sky's 0.12/0.04. It now
  applies only to frames without RT GI or an environment.

## 7. How to run

```
build/Release/engine_tests.exe
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_helmet_game_deferred
python tools/check.py
```
