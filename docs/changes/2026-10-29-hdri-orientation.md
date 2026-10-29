# The HDRI the right way up (ROADMAP row 16)

**Spec:** none (a defect fix); `docs/ROADMAP.md` row 16, "every HDRI looked up upside down" (found 2026-10-28);
AGENTS.md gotcha "An equirectangular environment's row 0 is the zenith".
**Status:** fixed. Goldens 32 of 32, 13 images moved and looked at. `check_fireflies` went red at the fix (357 against 150) and
passes at 32 after section 8's diagnosis and Frank's ruling on what it counts.

## 1. Summary

`stbi_loadf` loads an equirectangular HDRI with its first row (the zenith in every standard capture) as row 0. The path
tracer's importance sampler (`env_sampling.glsl`, `acos(y) / pi`) maps it that way. Every lookup of the environment did not:
`v = asin(y) / pi + 0.5` put +Y on the file's last row, so the sky lit every scene from below, the ground from above, and the
path tracer's sampler steered samples at the mirror image of what its miss shader shaded. All lookups now write
`v = 0.5 - asin(y) / pi`, the same number as the sampler's.

## 2. What changed

| Site | Change |
|---|---|
| `shaders/core/deferred_lighting.frag` `equirectUV` | `0.5 - asin(y) / pi` (prefiltered environment and irradiance) |
| `shaders/includes/lighting/clustered_lights.slang` `equirectUV` | the same (the forward `transparent` pass, `vfx.expand`'s ambient) |
| `shaders/ibl/env_prefilter.slang` `dirToUv`, `uvToDir` | both directions flipped together, so the built images keep the lookups' convention |
| `shaders/rt/gi_trace.slang` | an escaped GI ray's environment lookup |
| `shaders/rt/pt_miss.rmiss` `dirToEquirect` | the path tracer's miss, now agreeing with its sampler |
| `ohao/render/rt/env_sun.cpp` | the sun finder's row elevation (`rowElevation`): the outdoor HDRI's sun is now reported toward (0.554, **+0.742**, 0.378), above the horizon (it was −0.742); the same 237 texels, 51.3% of the power, are painted out |

`shaders/rt/cinematic_composite.comp` already wrote `0.5 - theta / PI` and is unchanged.

## 3. Tests

- **New: `tests/renderer/check_env_orientation.py`** (CTest `check_env_orientation`, label gpu). It writes a 64×32 HDRI blue above
  the horizon (0.2, 0.4, 1.0) and orange below (1.0, 0.4, 0.2), a white floor and one light that gives nothing (a point light 100 m
  down with a 1 m range: without any light, deferred lights the scene with its fallback sun), and captures it through `ohao serve`
  deferred with RT GI, deferred with RT GI off, and path traced. The floor, and the path tracer's sky, must come out with mean
  blue / mean red ≥ 1.5.

  | Capture | Before (floor, sky) | After |
  |---|---|---|
  | deferred, RT GI | 0.487 | **2.052** |
  | deferred, IBL | 0.488 | **2.051** |
  | path traced | 0.566, sky 0.478 | **1.766**, sky **2.092** |

  The deferred floor's mean RGB went from (203, 146, 99) to (99, 146, 203): an exact mirror, the flip and nothing else.
- **`env_sun_test`**: its sun moved from row 80 to row 40 of 128 (above the horizon under the right mapping), the lookup it checks
  is `0.5 - asin(y) / pi`, and it asserts the sun's `y > 0`. 6 of 6.

## 4. Results

**Goldens** (`tests/golden/render_golden.py`): every HDRI-lit scene moved; every scene without an HDRI stayed exact (Cornell,
the animation rig, the area light, transparency; Cornell path traced within its 1 LSB floor). Looked at side by side before
updating (the helmet deferred and path traced, the metal spheres, Sponza, the play scene): the helmet's visor and the metal
spheres now reflect the studio's softboxes overhead, the lower spheres and the labels are lit from above instead of by the floor,
the play scene's shadows are filled by blue skylight from above, and Sponza (enclosed) barely moves. The deferred and path-traced
helmets now agree on the floor's warm light.

| Golden image (manifest names sharing it) | max_abs | rmse |
|---|---|---|
| `damaged_helmet` (+ `cli_`) | 206 | 43.96 |
| `damaged_helmet_deferred` (+ `cli_`) | 242 | 40.38 |
| `helmet_game_deferred` | 242 | 40.03 |
| `helmet_game_reference` | 231 | 40.17 |
| `khronos_metal_rough_spheres_deferred` (+ `cli_`) | 229 | 40.12 |
| `khronos_metal_rough_spheres_reference` | 226 | 41.05 |
| `khronos_flight_helmet_deferred` (+ `cli_`) | 155 | 36.14 |
| `khronos_flight_helmet_reference` | 139 | 36.57 |
| `khronos_sponza_deferred` | 33 | 2.17 |
| `khronos_sponza_reference` | 35 | 2.25 |
| `blockout_room_deferred` | 69 | 16.57 |
| `blockout_play_deferred` | 69 | 10.79 |
| `blockout_play_hud` | 68 | 10.73 |

After the update: 32 of 32.

**The gate** (`python tools/check.py`, `build/check/check.json`): 49 of 50. The miss:

- **`check_fireflies`: 357 against 150** (88 with the sky upside down). 344 of the 357 (96%) lie within 3 px of a luminance edge,
  which is 7.4% of the frame: the enemy's outline, its rifle, the gun's edge, the platform's. None are clusters on sunlit surfaces,
  the signature of the original sun fireflies, and the sun is still found and taken out (the log line above). The cause is RT GI's
  half-resolution upsample bleeding across silhouettes, the mechanism the fireflies record already named for its 88; the correct sky
  is brighter above than the ground was, so GI adds more (6.7 levels on flat pixels, p99 29.3) and the same edge error crosses the
  12-level threshold more often. **The gate stays at 150** (Frank's ruling, 2026-10-29): the GI upsample at edges is the next step.

Benches were not run: the change moves no cost (the same lookups, one subtraction each way).

## 5. Found on the way, not changed

Both fixed the same day: `docs/changes/2026-10-29-deferred-pt-parity.md`.

- **Deferred never shows the HDRI behind the scene.** Its background is the procedural sky pass; the path tracer shows the HDRI.
  A scene lit by an HDRI therefore looks different in the two renderers wherever the sky is in view.
- **Deferred adds a fallback sun to a scene without lights**; the path tracer does not. A test of the environment alone needs a
  light that gives nothing (as `check_env_orientation.py` does).

## 6. Docs

AGENTS.md (the gotcha rewritten as the convention; `check_env_orientation.py` in the GPU suites; the fireflies paragraph's state),
ROADMAP row 16, and the monograph: `sampling/env-cdf` ("One latitude convention: the file's") and `path-tracer/hit-miss` ("One
equirectangular convention, one texture") describe the single convention; the `sampling-env-cdf-vflip` figure is deleted. Citations
resolve (`test_monograph_structure.py` 16 of 16).

## 7. The fireflies gate, diagnosed (follow-up the same day)

Section 4's reading (the half-resolution upsample bleeding across silhouettes) was tested and is **not** the cause:

- **An upsample change made no difference.** A search of the 4x4 half-resolution samples for the pixel's own surface where the
  bilinear taps hold too little of it moved the count from 357 to 363. It was reverted.
- **History and TAA, not GI's output, set the count.** The same view: TAA at 4 frames 363, without post at 4 frames 717, without
  post at 32 frames 653, TAA at 32 frames 64.
- **The metric counted more than fireflies.** Of the 363 pixels where what GI adds rose 12 levels over its 5x5 median:
  - 34 were bright specks in the GI-on image (over its own 5x5 median by 12): fireflies.
  - 76 were dark specks of the GI-off image that GI-on does not have.
  - 253 were neither: thin features (the rifle, fingers, the gun's edge) that GI lights differently from what is behind them. A
    thin feature fills less of its 5x5 than its surroundings, so any lighting difference between the two counts. Under the sky
    upside down the bounce was dim and these stayed under 12.

**Ruling (Frank, 2026-10-29):** count only GI's specks, both tests at 12 levels, the gate unchanged at 150. Result: **32**.
**The gate still catches its purpose:** with the sun separation disabled for one run (`env_upload.cpp`, restored and rebuilt
after), the same metric counted **524**, a fail.

## 8. How to run

```console
$ python tests/renderer/check_env_orientation.py
$ build/Release/env_sun_test.exe
$ python tests/golden/render_golden.py tests/golden/manifest.json
```
