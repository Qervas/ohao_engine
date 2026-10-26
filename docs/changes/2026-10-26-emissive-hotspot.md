# Emissive colour fixed; the right-wall hotspot diagnosed

**Spec:** none. Owner job of 2026-10-26, after the ambient kill (`25bfe37`). Two
deferred-only look bugs against the path tracer: DamagedHelmet's cyan emissive ring barely
shows, and a bright hotspot sits on the right wall.

**Status:**
- Emissive: built and tested.
- Hotspot: diagnosed, not fixed. Its causes are two light-semantics mismatches between
  the renderers plus a deferred albedo bug whose fix depends on them. Choosing the
  semantics is Frank's decision; the options are in section 6.

## 1. Summary

**Emissive (fixed).**
- The G-buffer kept only the luminance of the emissive texture (`gBuffer1.a`), and
  lighting rebuilt emission as `albedo × luminance`.
- So an emitter glowed in its base colour. DamagedHelmet's cyan ring and panel lines
  came out dim grey.
- The path tracer emits the texture's colour.
- The deferred frame now has an emitted-radiance attachment and adds it in its own colour.

**Hotspot (diagnosed).** On `helmet_game.scene.json` the WindowLight (intensity 600, at
(12, 3, 5), 3 m from the right wall at x = 15) lights the right wall 2.9× the path
tracer's radiance at the hotspot. The same frame is 0.07–0.41× the path tracer's
radiance elsewhere. A controlled scene (`build/light_units.scene.json`: one wall, one
point light at 3 m, range 100, no environment) isolates three causes, all confirmed by
measurement:
1. **Point-light units differ by 4×.**
   - Deferred lights with `color × I / d²` (radiant intensity `I`, as glTF's
     KHR_lights_punctual defines it).
   - The path tracer samples a sphere of radius `r` with radiance `I / (4πr²)`, whose
     radiant intensity is `I/4` (`pt_raygen*.rgen`, light type 0).
   - Its spot light uses `I`, as deferred does.
   - Measured on the wall's centre: deferred 0.0803, the path tracer 0.0397 (linear).
     With the albedos of item 3 that is `I` against `I/4` exactly.
2. **Range.**
   - Deferred windows every point and spot light to zero at `Light.range`, as the
     component documents ("Distance at which a Point/Spot light's contribution reaches
     zero"). Clustered lighting depends on that hard zero.
   - The default range is 10 m and the helmet room authors none, so in a 30 m room
     deferred loses all direct light past 10 m from each light: the back wall reads
     0.07× the path tracer.
   - The path tracer ignores range.
3. **Albedo.**
   - `gbuffer.frag` computes `albedo = vertexColour × baseColour`.
   - Primitives (QuadPrim, BoxPrim) carry their colour as vertex colour and base colour
     both, so every wall is shaded with `colour²`: 0.25 where the path tracer, whose
     albedo is base colour × texture ("an untextured material is its base color, once"),
     uses 0.5.
   - This is a plain deferred bug. Fixed alone, it would double every primitive's
     brightness while point lights stay 4× the path tracer's, making the hotspot and the
     mismatch worse. So it waits for the units decision.

## 2. What was built

- **`GBufferPass`:** a sixth target, `kEmissive` (5, RGBA16F; depth stays 4, `kDepth`),
  colour output 4 of `core/gbuffer.frag`, cleared to 0. All six-target arrays, the
  render passes' colour references, the blend states shared by the instance, culled and
  meshlet pipelines, and the descriptor set follow `GBUFFER_COUNT`.
- **`core/gbuffer.frag`:** `outEmissive = emissiveTexture.rgb × emissiveStrength`. The
  normal target's alpha keeps the emissive luminance only to exclude emitters from SSS.
- **`DeferredLightingPass`:** binding 17 is the emissive view, and lighting adds
  `texture(gBufferEmissive).rgb` unlit.
- **`pt_closesthit.rchit`:** emission is multiplied by the material's emissive strength
  too, so the two paths agree when the strength is not 1. Every checked-in scene has
  strength 1 or 0, so no path-traced golden moved.

## 3. Bugs found and fixed

- Emission lost its colour in the deferred G-buffer (section 1).
- The path tracer ignored the emissive strength.

Found, not fixed (section 6): the point-light unit mismatch, the range mismatch, and
deferred's vertex-colour × base-colour albedo.

## 4. Spec corrections

There is no spec. Monograph units updated: `deferred/gbuffer` (six attachments, the
emissive attachment), `shaders/core-gbuffer-lighting` ("The emissive attachment"),
`deferred/sky` (cite). AGENTS.md has two new gotchas: the emissive attachment, and the
open light-semantics mismatch.

## 5. Results

Emissive ring on DamagedHelmet: linear luminance over the 1,108 pixels the path tracer
shows as cyan, from `build/ring_measure.py`.

| Render | Ring luminance | RGB |
|---|---|---|
| Deferred before | 0.062 | (0.044, 0.067, 0.074) |
| Deferred as built | 0.283 | (0.085, 0.341, 0.291) |
| Path tracer | 0.367 | (0.150, 0.429, 0.389) |

The ratio to the path tracer went from 0.17 to 0.77.

**Helmet/left-wall ratio:** 0.534 → 0.585 (`damaged_helmet_deferred`; 0.584
`helmet_game_deferred`), against the path tracer's 0.534. This is not a regression in
the helmet: the ring lies inside the helmet box and now glows as in the path tracer. The
rest of the ratio is still distorted by section 1's lighting mismatch.

**Hotspot** (unchanged by this job). Linear radiance by region from
`build/linear_compare.py`, `helmet_game_deferred` (exposure 1) against `damaged_helmet`
(path tracer, exposure 0.5):

| Region | Deferred median | Path tracer median | Ratio | Deferred 99th pct | Path tracer 99th pct |
|---|---|---|---|---|---|
| Right-wall hotspot | 2.375 | 0.816 | 2.91 | 6.79 | 1.61 |
| Right wall, low | 0.032 | 0.236 | 0.13 | | |
| Left wall | 0.202 | 0.492 | 0.41 | | |
| Back wall | 0.026 | 0.367 | 0.07 | | |

Goldens: three images moved, because only DamagedHelmet has an emissive texture. Each
was looked at before its `--update --only`:

| Scene | max_abs | pixels | rmse |
|---|---|---|---|
| damaged_helmet_deferred (+cli) | 125 | 4.3% | 6.6 |
| helmet_game_deferred | 125 | 4.3% | 6.6 |
| helmet_game_reference | 120 | 4.4% | 6.3 |

Every other golden, path-traced ones included, is unchanged.

Gates:
- `python tools/check.py`: 37/37.
- Render goldens: 22/22 on a full run after the update.
- Monograph structure: OK.

Benchmarks, one run each. Both are **noisy** (CPU calibration more than 25% over its
reference, other processes busy), so neither is a verdict. No budget value was changed.
- **`render_bench`:**
  - cornell_720p: frame 1.049 ms (budget 1.5), lighting 0.290 ms (0.32).
  - gbuffer 0.083 ms (0.10): it was about 0.063 before the sixth attachment.
  - helmet_game_1080p: frame 2.197 ms (5.0).
- **`serve_bench`:** window_capture_inline 8.7 ms (20), window_capture_rt_inline
  128.8 ms (150).

## 6. Known gaps: the hotspot, for Frank's decision

The three causes in section 1 must be resolved together.
- **Light units.** Which is canonical: radiant intensity `I` (deferred, the path
  tracer's spot, KHR_lights_punctual), or the path tracer's sphere radiance
  `I/(4πr²)` (intensity `I/4`)?
  - Changing the path tracer moves every path-traced golden and the reference itself.
  - Changing deferred darkens every point light 4×.
- **Range.** Should `range` stay a hard cutoff, which the path tracer would then honour
  too? Or should the default become physical, which needs a different bound for the
  froxel lists?
- **Albedo.** Base colour × texture, as the path tracer does. This lands with the units
  decision, since alone it doubles every primitive while lights stay 4× apart.

Also still open:
- **Ring luminance is 0.77 of the path tracer's.** The path tracer's ring pixels also
  carry the lighting mismatch above.
- **Emission ignores the glTF emissive factor and KHR_materials_emissive_strength.** The
  loader reads neither, in both paths.

## 7. How to run

```
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_helmet_game_deferred
python build/ring_measure.py tests/golden/damaged_helmet.png 0.5 tests/golden/helmet_game_deferred.png 1.0
python tools/check.py
```
