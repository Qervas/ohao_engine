# SSR composition: reflected light counted once

**Spec:** none. This follows Frank's rulings of 2026-10-25 (Berlin), made after the
environment-reflections fix (`c4796a2`):
- Screen-space reflections must not add on top of the environment/GI specular.
- Report the helmet's ratio against the path tracer with SSR on and off.
- Fix the next cause of the remaining gap only if it is a ground-truth bug in the same
  composition path.
- Keep every budget value; report noisy benches as noisy.

**Status:** Built and tested.
- SSR now replaces the environment's reflection where its trace hits, under the
  lighting pass's own reflection weight. The weight (split sum and specular occlusion)
  is applied once.
- The next cause of the gap was in the same composition: the constant ambient term was
  applied to metals. It is fixed.
- On the helmet room, the helmet/left-wall ratio went from 0.734 to 0.471 with SSR on
  (0.473 off). The path tracer measures 0.534.

## 1. Summary

Two composition bugs, both verified in the code before any change:

1. **SSR double count (the hypothesis, confirmed).**
   - `tonemapping.frag` added the half-resolution SSR to the frame
     (`hdrColor += ssrAt(...)`) after TAA.
   - The lit image already held the full environment/GI reflection for the same pixels.
   - SSR also weighted its colour with its own plain Schlick Fresnel: no roughness, no
     split-sum fit, no specular occlusion.
   - So a reflected surface carried two reflections under two different weights. On the
     helmet room SSR raised the ratio from 0.676 (SSR off) to 0.734.
2. **Ambient on metals (the next cause, found by measurement).**
   - The constant ambient term was `ambientIntensity * albedo * ao * ssao`, with no
     `(1 - metallic)`.
   - For a metal the G-buffer albedo is its F0, so every metal got a flat F0-tinted
     glow: 0.35 × F0 with the scene's lights. The path tracer has no counterpart.
   - The shader's other diffuse terms (GI, IBL irradiance) already exclude metal.
   - With SSR fixed, the ratio stayed at 0.676. With the ambient made diffuse-only it
     moved to 0.471. The wall's brightness did not change (0.6448): it is dielectric.

## 2. What was built

- **Lighting (`core/deferred_lighting.frag`, `DeferredLightingPass`)** has three
  RGBA16F color attachments:
  - the lit image;
  - `envSpecular`, E: the environment/GI specular the pixel added;
  - `specularWeight`, W: `(F0·A + B) · specularOcclusion(NdotV, ao·ssao, roughness)`.
  
  W is computed before the environment branch, so it exists for every pixel; a scene
  without an environment still weights SSR correctly, with E = 0. Sky writes 0 to all
  three. The ambient term is `albedo · (1 − metallic) · ao · ssao`.
- **SSR's trace (`postprocess/ssr.slang`)** writes `(c·L, c)`: the hit radiance
  premultiplied by the confidence c (distance and roughness fade, at most 0.8), and c.
  It carries no Fresnel or weight of its own. Sky, misses and rough dielectrics write 0.
- **SSR's resolve (`postprocess/ssr_resolve.slang`, new)** runs at full resolution on
  the lit image in place, before bloom and TAA:
  - It upsamples the trace from its 4 nearest samples, weighted bilinearly and by
    distance-from-camera similarity within 5%, using the G-buffer position. Otherwise
    it takes the closest sample.
  - It computes `lit += W·(c·L) − c·E`, which equals
    `lit − E + W·lerp(incoming, L, c)`.
  - Pixels with no reflection are left untouched.
  - `SSRPass` owns both pipelines, a color-attachment→compute barrier before the
    trace, and the lit image's `GENERAL` round trip.
  - `SSRResolvePush` is in `shaders/shared/layout.json`.
- **Post** no longer touches SSR. The tonemap has 2 bindings (the frame and bloom).
  These are removed: `ssrAt`, `ssrOn`, `depthA`/`depthB`, `setSSRView`,
  `setSSRProjection` and post's depth view.

## 3. Bugs found and fixed

- The SSR double count and its separate Fresnel weight (section 1).
- The ambient term on metals (section 1).

## 4. Spec corrections

There is no spec. Monograph units updated to the code as built:
- `deferred/ssr-sss`: a new section, "The resolve replaces the environment's
  reflection", plus updated contracts.
- `deferred/post`, `deferred/lighting`, `deferred/ssao`, `deferred/gizmo`.
- `gpu/dispatch`, `hybrid/gi-technique`, `materials/ggx`.
- `shaders/core-gbuffer-lighting`, `shaders/postprocess-chain`,
  `shaders/overlay-gizmo`.

Also: AGENTS.md has a new gotcha ("Reflected light is counted once"), and
ARCHITECTURE.md's frame diagram is updated.

## 5. Results

Helmet/left-wall brightness ratio: the mean luma of a helmet box over a left-wall box,
`helmet_game.scene.json` deferred, 30 frames, TAA, SSAO and bloom, exposure 1, 1920×1080
through `ohao serve`. The path tracer is the `damaged_helmet` golden. This box metric
differs from the masks of the previous change record, which reported 0.69 / 0.63 / 0.48
for the same three images. Both metrics put the path tracer below deferred.

| Render | SSR on | SSR off |
|---|---|---|
| Before (HEAD `c4796a2`) | 0.734 | 0.676 |
| SSR replaces the env reflection | 0.676 | 0.676 |
| + diffuse-only ambient (as built) | 0.471 | 0.473 |
| Path tracer | 0.534 | |

The re-baselined goldens agree: `helmet_game_deferred` 0.471, `damaged_helmet_deferred`
0.472, against the path tracer's 0.534. Validation was clean (`OHAO_VALIDATION=1`) on the
SSR on/off captures.

Goldens: 16 entries moved, from 11 images (the deferred and reference ones). Every image
was looked at before `--update`, and two full runs pass 22/22 afterwards. Difference
from the old goldens:

| Scene | max_abs | pixels | rmse | why |
|---|---|---|---|---|
| cornell_box_deferred / _reference (+cli) | 147 | 2.7% | 19.5 | the mirror spheres lose the grey ambient wash and reflect the red and green walls, with the open front black at their centre, as in the path tracer |
| damaged_helmet_deferred (+cli) | 186 | 23.6% | 22.9 | the frame reads as dark grey metal, not bright silver |
| helmet_game_deferred / _reference | 186 / 187 | 24.6% / 24.7% | 23.0 / 22.7 | the same |
| khronos_metal_rough_spheres_deferred / _reference | 191 | 29.9% / 30.4% | 18.9 / 18.3 | metal spheres get reflection contrast; gold spheres are saturated gold |
| khronos_flight_helmet_deferred / _reference | 151 | 10.3% / 10.4% | 9.2 / 9.1 | the brass plaque reflects the dark environment behind the camera (its yellow was the ambient) |
| khronos_sponza_deferred / _reference | 150 | 47.7% / 47.8% | 15.2 | the metallic trim is darker; trace hits replace the environment's reflection |

The path-traced goldens are unchanged.

Gates:
- `python tools/check.py`: 37/37.
- Render goldens: 22/22, run twice after the update.
- Monograph structure: OK.

Benchmarks, one run each. No budget value was changed.

- **`render_bench`:** not flagged noisy, 22/22 within budget. One run is a measurement,
  not the five-pass verdict.
  - cornell_720p:
    - frame 1.037 ms (budget 1.5)
    - lighting 0.295 ms (0.32), now writing three attachments
    - `ssr` 0.030 ms (0.04; it was 0.009 before the resolve existed)
  - helmet_game_720p: frame 0.991 ms (3.0).
  - helmet_game_1080p: frame 2.149 ms (5.0), `ssr` 0.128 ms.
  - bench100k_1080p: frame 5.256 ms (8.0).
- **`serve_bench`:** **noisy** (UnrealEditor and others at 47–57% of a core), so not a
  verdict. Every measurement was within its budget, including window_capture_rt_inline
  at 118.3 ms (150).

## 6. Known gaps

- **The helmet's remaining gap.** The ratio is now 0.47 against the path tracer's 0.53,
  so the helmet is slightly darker than ground truth. Visible differences that remain:
  - The path tracer's emissive ring glows cyan; deferred barely shows it.
  - A bright hotspot on the right wall exists only in deferred.
  - The constant ambient (0.35 × diffuse albedo) still lights every dielectric, the
    walls included, on top of RT GI. The path tracer has no such term; removing or
    scaling it is a look decision for the whole frame.
  
  None of these is in the specular composition path, so all are deferred.
- **FlightHelmet has no path-traced ground truth.** `ohao run --mode rt_offline` renders
  the whole model as chrome, which is wrong for its leather, fabric and glass. The
  plaque's darkening is judged from the deferred frame's own inputs: AO and specular
  occlusion are near 1, GI visibility is high through the room's open front, and the
  incoming radiance is low.
- **SSR's trace is unchanged.** It is still a world-space march with a projected-depth
  thickness, against an unjittered matrix. The confidence is still the old fade (at most
  0.8, falling with roughness) and has no screen-edge fade.
- **The resolve reads 4 trace samples and 5 positions per pixel with any confidence.**
  Its cost is inside the `ssr` timer.
- **`cornell_box_rt_realtime.png` is modified in the working tree.** `--update` rewrote
  it with a run differing by its known 1-LSB BVH floor on one pixel. It is not in any
  commit; restoring it (`git restore`) was not permitted from this session.

## 7. How to run

```
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_helmet_game_deferred
python tests/golden/render_golden.py tests/golden/manifest.json --only cornell_box_deferred
python tools/check.py
```
