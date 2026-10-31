# Frontier F5b: RT reflections; SSR retired

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, step F5: "RT reflections through the same
paths"; retires SSR and the split sum for occluded specular).
**Plan:** `docs/plans/2026-10-31-frontier-f5.md`, F5b.
**Status:** built; F5c (SSAO and the constant ambient) and F5d (ReSTIR GI) next.

## 1. Summary

**Specular indirect light is traced.** RT GI casts one more ray a pixel, its direction from the GGX lobe of the
surface's roughness, shaded as its diffuse rays are (a surface's direct light and the radiance cache behind it, the
environment on a miss), and denoised by the same chain with its spatial filter spread by roughness. Lighting weighs it
by OpenPBR's specular albedo. The split sum (the prefiltered environment weighed by GI's escaped share) remains only for
the frame without ray tracing. SSR, its two passes and lighting's E/W outputs are gone. Mirrors reflect the room: the
Cornell box's chrome sphere was near black.

## 2. What was built

- `rt/gi_trace.slang`: the reflection ray (bounded VNDF, Eto and Tokuyoshi 2023; a direction below the horizon
  mirrored), at the trace's resolution, written with the roughness in alpha (realtime `specOut`; reference the running
  mean in `specRefMean`, bindings 15-16). Its mean over the lobe is the radiance the split sum prefiltered, traced.
- `RTGITechnique`: a second denoising chain (`denoiseChain(cmd, 1)`: the reflection's own images and descriptor sets,
  the guide shared); `GIOutput::reflectionView`.
- `rt/gi_denoise.slang` `atrous`: `params.w` 1 spreads the kernel only as far as the surface is rough (none below 0.05,
  all from 0.4), read from the accumulation's alpha.
- `core/deferred_lighting.slang`: binding 22 the reflection; with RT GI the specular indirect is it times the specular
  albedo and `specularOcclusion` of the material's AO (SSAO only without RT GI); one output (the lit image).
- **Retired:** `SSRPass`, `postprocess/ssr.slang`, `postprocess/ssr_resolve.slang`, `SSRPush`, `SSRResolvePush`, the
  `ssr` pass and its budget (0.04 ms on Cornell 720p: it goes with its pass, as rt.shadows' did in F4), lighting's
  `envSpecular` and `specularWeight` targets, `render.settings ssr`.

## 3. Bugs found and fixed

- None new; the reflection replaced a darkness: indoors, the split sum reflected the environment weighed by GI's escaped
  share and GI's diffuse radiance for the rest, and SSR filled only what the screen showed.

## 4. Spec corrections

- The reflection is the lobe's mean radiance weighed by the specular albedo (the split sum's structure, traced), not an
  estimator of the BSDF's integral; the denoiser blurs it by roughness. ReSTIR on it is not in this step.

## 5. Results

- **The referee** (`check_parity`, deferred/path tracer; after F5a in brackets):

  | scene | ratio | stops |
  |---|---|---|
  | cornell_box | 0.982 (0.967) | 0.10 (0.18) |
  | damaged_helmet | 0.925 (0.941) | 0.22 (0.29) |
  | khronos_metal_rough_spheres | 0.906 (0.918) | 0.23 (0.20) |
  | area_light | 0.980 (0.973) | 0.12 (0.13) |
  | khronos_sponza | 1.008 (1.008) | 0.29 (0.30) |
  | blockout_room | 0.949 (0.948) | 0.09 (0.09) |

  With SSR still on and off the numbers were the same: the traced reflection covers what it replaced.
- **Cost** (`render_bench`, medians, a busy machine): Cornell 720p `rt.gi` 0.21 to 0.27 ms, `rt.gi.denoise` 0.14 to
  0.29 ms, `ssr` 0.04 ms gone; play_hud_1080p `rt.gi` 0.65 to 0.78, `rt.gi.denoise` 0.30 to 0.63 ms.
- **Tests:** `engine_tests` 86/86, `api_tests` 70/70 (the settings tests switch `sss` where they switched `ssr`),
  `check_material` 21/21 (the white furnace 0.999 deferred), `check_analytic` 16/16, `check_validation` 19/19,
  `vfx_gpu_test` 44/44, `light_emitter_gpu_test` 31/31, `exposure_gpu_test` 6/6, `tools/check.py` 57/57; render goldens 33/33, the 25 deferred re-captured after looking
  at them; CLI and serve expectations: the schema hash and `render.settings` without `ssr`.

## 6. Known gaps

- **Reflections reproject by the surface's motion**: a moving camera smears a mirror's reflection for a few frames
  (the long history is clamped to the short one's neighbourhood).
- **The metal-rough spheres and the helmet are 8-9% darker than the path tracer**: the lobe's mean radiance times the
  albedo is the split sum's approximation, and rough metals' multiple bounces between their own facets are in the albedo,
  not in the reflection's light.
- **The denoiser's second chain doubles `rt.gi.denoise`.**
- **Light that changes reaches the indirect light slowly**: after Cornell's lights doubled, the metered exposure was
  still 1% short 150 frames later (`exposure_gpu_test` now meters 300 frames a step; settled, doubling meters +0.9987 EV
  and +1 EV of compensation -1.0004).

## 7. How to run

```console
$ python tests/renderer/check_material.py > /dev/null   # gpu
$ python tests/renderer/check_parity.py > /dev/null   # gpu # slow
```
