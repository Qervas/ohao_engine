# Deferred and the path tracer light and show the same scene

**Spec:** none (two inconsistencies found by `docs/changes/2026-10-29-hdri-orientation.md`, section 5; Frank's ruling 2026-10-29:
"deferred follows the scene").
**Status:** fixed. Gate 50 of 50 (`python tools/check.py`, labels quick, gpu, docs). Goldens 32 of 32, 3 moved and looked at.

## 1. Summary

Two differences between the renderers, neither a choice anyone had made:

1. **The background.** With an HDRI in the scene, the path tracer's camera rays that miss show the HDRI; the deferred frame drew
   its procedural Preetham sky there instead, so the sky behind an HDRI-lit scene was not the sky lighting it.
2. **The fallback sun.** A scene without lights was lit in deferred by a made-up directional light that followed the procedural
   sky's time of day (blending to a moon at night, with its own ambient of 0.12 to 0.04) and was traced by RT shadows. The path
   tracer lit the same scene by its environment alone.

Now the deferred frame follows the scene, as the path tracer does.

## 2. What changed

- **`shaders/core/deferred_lighting.frag`:** a sky pixel (no geometry) with an environment bound (IBL, flag bit 0) is the
  environment along the view ray: level 0 of the prefiltered image, which `EnvPrefilter` fills with a copy of the source, times
  `Environment.intensity`. Without an environment it stays black for the sky pass.
- **`ohao/render/deferred/deferred_renderer.cpp`:** the sky pass runs only without an environment (`environmentBackground`).
- **`ohao/gpu/vulkan/buffer_setup.cpp` `setFrameLights`:** the scene's raster lights and nothing else; the ambient is 0.35 (or
  `setAmbientIntensity`'s) for every scene. `m_fallbackSun`, its day/night blending and its RT shadow light
  (`render_dispatch.cpp`, `kSunAngularRadius`) are deleted; RT shadows trace the scene's shadow casters, none without lights.

The procedural sky stays for scenes without an environment (Cornell, transparency: `Environment {}` binds no image).

## 3. Tests

`tests/renderer/check_env_orientation.py` now renders its scene with **no light** (its null light, the workaround for the fallback
sun, is gone) and checks the sky band in all three captures, not only the path tracer's:

| Capture | Floor blue/red | Sky blue/red (mean RGB) |
|---|---|---|
| deferred, RT GI | 2.052 | 1.936 (110, 160, 213) |
| deferred, IBL | 2.051 | 1.936 (110, 160, 213) |
| path traced | 1.766 | 2.092 (98, 147, 205) |

The deferred sky band reads about 10 levels above the path tracer's on the test's 64×32 map: the band includes the horizon, where
the bilinear filter blends the two halves (the deferred lookup samples the RGBA16F prefiltered level 0, the path tracer the RGBA32F
source).

## 4. Results

**Goldens:** 3 moved, the three whose camera sees the sky (`cli_blockout_room_deferred` max_abs 82, 19.4% of pixels;
`cli_blockout_play_deferred` 96, 25.4%; `cli_blockout_play_hud` 89, 25.4%): the procedural sky replaced by the HDRI's sky, clouds
above the horizon, every other pixel unchanged. Looked at side by side before updating. The helmet, sphere and Sponza scenes see no
sky and did not move; every checked-in scene has a light, so no golden moved for the fallback sun.

**Gate:** 50 of 50, `check_fireflies` among them at 32 (the speck metric, `2026-10-29-hdri-orientation.md` section 7).

## 5. Found on the way, not changed

The path tracer's capture of the play scene (`rt_offline`, 512 spp, OIDN, 1280×720, from the spawn looking west) is blown out to near
white where the deferred frame is not. Its exposure or its sun against the deferred frame's needs its own look.

## 6. Docs

AGENTS.md: a gotcha "Deferred and the path tracer light and show the same scene"; the constant ambient is 0.35 for every scene.
Monograph: `gpu/dispatch` ("The lights the deferred renderer sees", replacing "The fallback sun that follows the sky"),
`gpu/buffers-alloc` ("A scene without lights", replacing "The default light and its π"), `deferred/sky` (lighting writes the
environment; the sky pass runs only without one), `deferred/clustered-lighting`, `hybrid/shadow-technique`, `materials/lights`.

## 7. How to run

```console
$ python tests/renderer/check_env_orientation.py
$ python tests/golden/render_golden.py tests/golden/manifest.json
```
