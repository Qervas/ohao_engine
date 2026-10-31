# Frontier F5a: every bounce through the radiance cache

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, step F5: "the radiance cache behind RT GI for
every bounce").
**Plan:** `docs/plans/2026-10-31-frontier-f5.md`, F5a.
**Status:** built; F5b (RT reflections), F5c (the fills) and F5d (ReSTIR GI) next.

## 1. Summary

**RT GI carries every bounce.** A GI ray's hit reflects its albedo times its direct light and the light one more ray
brings back from the radiance cache, and adds that to the cache, so each frame carries light one bounce further. The
referee (`check_parity`, the deferred frame against the path tracer) moves every scene toward the path tracer: Cornell
0.863 to 0.967 of its brightness, the blockout room 0.780 to 0.948.

## 2. What was built

- **RT GI owns a `RadianceCache`** (the path tracer's class, its own instance: 1M cells, 36 MiB, a history of 8 samples)
  at the trace's bindings 12-14. `render()` records the cache's begin-frame barrier (and its clear after a temporal reset
  or at a reference run's start), the trace, then the resolve, all timed as `rt.gi`.
- **The hit** (`rt/gi_trace.slang`): its geometric normal from the triangle's vertices
  (`rt/alpha_test.slang` `hitGeometricNormal`, through the instance's object-to-world), facing the ray; its reflected
  light `albedo x (directAt / pi + indirectAt)`. `indirectAt` traces one cosine ray from the hit: where it lands, that
  surface's emission plus its cell (jittered within the voxel's face, if at least a voxel away), else the environment.
- **A pixel's first ray computes its hit's light and adds it to the cell; its other rays read their hit's cell**
  (computing only where it is empty or within a voxel). Pixels nearer the camera than 0.3 m (the first-person view
  model, drawn remapped) read but never feed it.
- `RadianceCache::create(..., maxSamples)`: a cell's history is a parameter (the path tracer keeps 256). Its barriers
  name the compute stage as well as the ray tracing one.

## 3. Bugs found and fixed

- **Direct light at a GI hit was taken "facing the ray"** (the hit had no normal): every bounce was darker than the
  true normal gives. With the geometric normal the referee went from 0.926 to 0.967 on Cornell, 0.887 to 0.918 on the
  metal-rough spheres. Before F5a this was the whole of GI's bounce.
- **A cell's history of 256 samples** kept the first frames (fewer bounces) in a cell for a hundred frames: 8.
- **A reference run after realtime frames** read the realtime frames' cache (`sp2b_t10`): a reference run's start clears
  it.
- **The arms changed the frame past their footprint by a level** (`light_emitter_gpu_test`): pixels feed the cache, and
  the arms hide some and launched others from where no world surface is. Near pixels no longer feed it; the test holds
  the light past the footprint the same (mean 1%, blocks 5%), as it does for emitters, not the bits.

## 4. Spec corrections

- The plan's F5a began with every GI ray computing its hit's light (two more rays a GI ray: +0.5 ms on the play scene at
  1080p). Reading the cache for a pixel's second and later rays costs less and, with direct light at the true normal,
  is closer to the path tracer.

## 5. Results

- **The referee** (`check_parity`, deferred/path tracer, realtime deferred at 32 frames):

  | scene | before | after | stops before | stops after |
  |---|---|---|---|---|
  | cornell_box | 0.863 | 0.967 | 0.68 | 0.18 |
  | damaged_helmet | 0.797 | 0.941 | 0.93 | 0.29 |
  | khronos_metal_rough_spheres | 0.743 | 0.918 | 0.93 | 0.20 |
  | area_light | 0.913 | 0.973 | 0.35 | 0.13 |
  | khronos_sponza | 0.974 | 1.008 | 0.33 | 0.30 |
  | blockout_room | 0.780 | 0.948 | 0.41 | 0.09 |

- **The closed box** (`check_material`, deferred against `Le / (1 - rho)` = 2): 0.986 at 256 frames (0.937 at 32; one
  bounce gave 1.5, 0.75 of it). The white furnace unchanged (0.999).
- **Cost** (`render_bench`, medians, a busy machine): `rt.gi` Cornell 720p 0.15 to 0.21 ms, play_hud_1080p 0.45 to
  0.65 ms. Registers: the trace 96 (was 80), the resolve 20.
- **Tests:** `engine_tests` 86/86, `check_analytic` 16/16, `check_material` 21/21, `check_direct_light` 4/4,
  `deferred_determinism_gpu_test` 95/95 (the cache sums integers: the same bits run to run), `check_validation` 19/19,
  `light_emitter_gpu_test` 31/31; render goldens 33/33, the 25 deferred re-captured (bounce light in the shadows,
  looked at frame by frame against the old ones), the 8 path-traced unchanged.

## 6. Known gaps

- **Multi-bounce light converges over tens of frames** where a cell takes few samples a frame (the closed box at
  320x180: 0.937 at 32 frames): each bounce waits on the one before. Light that moves is followed with the same lag.
- **A second ray landing within a voxel reads no cell** (a cell would feed itself): tight corners get one bounce less.
- **The cache is diffuse**: a hit reflects its albedo, its specular lobe as diffuse, as GI's bounce did.
- **Specular indirect** is still the split sum, SSR and SSAO: F5b and F5c.

## 7. How to run

```console
$ python tests/renderer/check_material.py > /dev/null   # gpu
$ python tests/renderer/check_parity.py > /dev/null   # gpu # slow
```
