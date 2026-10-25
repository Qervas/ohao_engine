# Deferred environment reflections: roughness and occlusion

**Spec:** none; Frank's rulings of 2026-10-25 (~19:00 Berlin): keep the DamagedHelmet
re-baseline (`4b482d1`), make the deferred frame's environment reflections respect
material roughness and occlusion, verify the cause in the code first, keep every bench
budget as a target and report noisy runs as noisy.
**Status:** Built and tested. The deferred frame now prefilters the environment by
roughness, uses the split-sum environment BRDF, and occludes environment light by RT GI's
visibility and by AO/SSAO. The helmet reads as dark rough metal instead of white chrome.
Eleven goldens were re-baselined (section 5).

## 1. Summary

The hypothesis was half right. The code showed four faults in the deferred IBL, and only
one of them was about AO.

1. **No roughness.** The environment image has one mip level, and its sampler has
   `maxLod` 0. So `textureLod(envMap, uv, roughness * 9.0)` always read mip 0, and every
   surface mirrored the HDR, sharp, at every roughness. The "irradiance" tap
   (`textureLod(envMap, nUV, 9.0)`) was one raw texel in the direction of N.
2. **No split sum.** The BRDF LUT setter (`setIBLTextures`) had no callers. Binding 9 held
   a 1x1 dummy image that was never written. The shader's fallback for an empty LUT,
   `vec2(max(1 - roughness, 0.04), roughness * 0.25)`, therefore ran for every pixel.
   That fallback is not the split sum: a rough metal kept almost full reflectance.
3. **No environment visibility.** AO already multiplied the specular term (`ao * ssao`),
   so the AO part of the hypothesis was wrong. The real occlusion fault was elsewhere:
   nothing stopped the specular IBL from seeing the sky inside a closed room. AO is a
   small-scale term and cannot do that. The helmet sits in a closed box, so its metal
   mirrored the studio HDR through the walls.
4. **Two smaller faults:**
   - An `F0`-weighted ambient floor for `metallic > 0.5` brightened metals further.
   - Environment.intensity scaled the path tracer's environment but not the deferred
     IBL.

## 2. What was built

- **`EnvPrefilter`** (`ohao/render/deferred/env_prefilter.{hpp,cpp}`,
  `shaders/ibl/env_prefilter.slang`):
  - Built once per environment map, in the upload's own command buffer.
  - From the source equirect it makes a mip chain (scratch; 2x2 means).
  - **Prefiltered image:** the environment's size with `kEnvPrefilterLevels` = 6 levels.
    Level i is the GGX lobe at roughness i/5, from 256 importance samples per texel. Each
    sample reads the chain at the mip its solid angle covers (filtered importance
    sampling, `lod = 0.5·log2(Ωs/Ωp) + 1`). Level 0 is the source itself.
  - **Irradiance image:** `kEnvIrradianceWidth` = 64 by 32, a cosine-weighted mean from
    512 samples.
  - The new constants and the `EnvPrefilterPush` struct live in
    `shaders/shared/layout.json`.
  - The path tracer and RT GI keep reading the single-mip source, so no path-traced
    pixel moves.
- **Lighting** (`shaders/core/deferred_lighting.frag`, `DeferredLightingPass`):
  - Binding 15 is the prefiltered environment, read at `roughness * (levels - 1)` along R.
    Binding 7 is the irradiance, read along N.
  - Both are scaled by Environment.intensity (`LightingParams::envIntensity`).
  - The split sum uses `envBRDFApprox` (Karis's analytic environment BRDF, already in
    `brdf_ggx.glsl`).
  - Specular is occluded by `specularOcclusion(NdotV, ao * ssao, roughness)` (Lagarde).
  - IBL runs only when both images are bound.
  - Removed: the LUT binding and its fallback, the metal floor, and `setIBLTextures`.
- **RT GI carries visibility** (`gi_trace.slang`, `gi_denoise.slang`, `GIOutput`):
  - The output is now the raw incoming radiance (no albedo) in rgb, plus the environment's
    visibility in alpha. Visibility is the fraction of the cosine-weighted rays that left
    the scene.
  - The temporal pass blends visibility with the radiance's own history weight. The
    à-trous and upsample passes filter it with the radiance.
  - Lighting multiplies the diffuse term by albedo itself.
  - Specular takes `prefiltered · vis + max(gi − irradiance · vis, 0)`: the environment
    where GI rays see it, and the bounce light elsewhere.
  - Without RT GI, visibility is 1 and the diffuse term is `kD · albedo · irradiance`,
    occluded.
  - GI misses are scaled by Environment.intensity (`GITracePush.cameraPos.w`).

## 3. Bugs found and fixed

- The four IBL faults in section 1.
- A layout.json edit made with `json.dump` reformatted the whole file. It was redone as a
  textual insert, so the diff stays minimal.

## 4. Spec corrections

None (no spec). The monograph is corrected:
- `deferred/lighting` has a new section, "Image-based lighting: prefiltered, split-sum,
  occluded".
- `deferred/gbuffer`, `deferred/ssao`, `hybrid/gi-technique`, `hybrid/visibility`,
  `materials/ggx`, `path-tracer/hit-miss`, `shaders/core-gbuffer-lighting` and
  `shaders/includes-lighting` now describe and cite the new code.
- AGENTS.md's IBL gotcha is rewritten.

## 5. Results

Goldens: 22 scenes, all pass after the re-baseline (two full runs, deterministic, CLI
twins equal). Before the update, each moved image differed from its old golden by:

| Scene | max_abs | pixels changed | rmse | why |
|---|---|---|---|---|
| cornell_box_deferred, cornell_box_reference | 1 | 0.2% | 0.02 | no environment; only GI's output format moved a few LSBs |
| damaged_helmet_deferred (+cli) | 221 | 96.7% | 82.7 | white chrome becomes dark rough metal |
| cli_helmet_game_deferred / _reference | 221 / 219 | 96.7% | 82.3 / 81.2 | the same helmet room |
| khronos_metal_rough_spheres_deferred / _reference | 240 | 98.3% | 72.9 / 71.8 | reflections widen and dim with roughness, as in the path tracer |
| khronos_flight_helmet_deferred / _reference | 223 | 95.0% | 69.6 / 68.5 | leather, wood and cloth lose the mirror sheen |
| cli_khronos_sponza_deferred / _reference | 226 / 225 | 100% | 28.8 / 28.7 | stone and cloth lose sharp sky glints; the interior takes the sky only where GI sees it |

Path-traced goldens (`cornell_box`, `damaged_helmet`, `cornell_box_rt_realtime` and their
CLI twins) are unchanged.

Helmet brightness against the room's left wall (mean of the helmet's pixels over the
wall's, deferred, 30 frames):

| Render | Ratio |
|---|---|
| Path tracer (target) | 0.48 |
| Old white-chrome golden | 0.90 |
| Prefiltering + split sum only | 0.78 |
| + GI environment visibility (as built) | 0.69 |
| As built, SSR off | 0.63 |

Gates:
- `python tools/check.py`: 37/37. The first run failed `check_layout` and `check_docs`:
  `layout.slang`/`.glsl` were stale against a layout.json doc edit. Regenerating them
  fixed both, and a full rerun passed.
- Doc commands: 104/104.
- Monograph structure: OK.

Benchmarks, run once each. Both runs are **noisy**: UnrealEditor and others were using
36-70% of a core. The numbers are not a verdict, and no budget changed.

- **`render_bench`:** every measurement is within its budget.
  - cornell_720p: frame 1.016 ms (budget 1.5), lighting 0.284 ms (0.32), rt.gi 0.069 ms
    (0.1), rt.gi.denoise 0.153 ms (0.2).
  - helmet_game_720p: frame 0.971 ms (3.0). helmet_game_1080p: frame 2.102 ms (5.0).
  - bench100k_1080p: frame 5.203 ms (8.0).
- **`serve_bench`:** every measurement is within its budget, including
  window_capture_rt_inline at 123.1 ms (150).

The prefilter runs once per environment load, inside the upload's submission, and is
not in any per-frame pass.

## 6. Known gaps

- **SSR is added on top of the IBL** in tonemapping, so reflections are counted twice
  where SSR hits: +9/255 on the helmet's mean. That accounts for most of the remaining
  gap to the path tracer (0.69 against 0.63 with SSR off). The fix is to blend SSR over
  the IBL by its confidence instead of adding it; not done here.
- **RT GI's half-resolution noise** shows as low-frequency blotches on flat walls. It was
  there before (it disappears with RT GI off). The old fallback BRDF's sheen hid it.
- **The environment BRDF is Karis's fit,** not an integrated LUT. It is within a few
  percent of the LUT and has no multiscatter energy compensation.
- **The prefilter assumes the GGX lobe is symmetric around R** (n = v = r). Grazing
  reflections are slightly too sharp. This is the standard split-sum error.
- `site/assets/units/deferred-lighting-bindings.svg` is now unreferenced and out of date.
  It is left in place: this session cannot delete files.

## 7. How to run

```
python tests/golden/render_golden.py tests/golden/manifest.json --only damaged_helmet_deferred
python tests/golden/render_golden.py tests/golden/manifest.json --only khronos_metal_rough_spheres_deferred
python tools/check.py
```
