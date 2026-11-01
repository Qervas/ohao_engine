# OpenPBR step 3: transmission, volume absorption, thin film, dispersion, thin-walled

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md` (section 8, step 3); plan
`docs/plans/2026-10-31-openpbr-step3.md`, tasks T1–T5.
**Status:** built (T1–T5). Rulings O10–O14 are provisional until Frank rules on them; O14 (thin-walled windows passing the
path tracer's shadow rays) is built since, with the gaps below that are closed: `docs/changes/2026-11-01-openpbr-gaps.md`.

## 1. Summary

OpenPBR Surface 1.1.1's dielectric transmission is in the module both renderers shade with
(`shaders/material/openpbr.slang`): rough refraction through GGX microfacets from either side of a solid, the thin-walled
slab, the volume's absorption, the thin film's iridescence and dispersion's IOR. The path tracer is a correct estimator of
it. The deferred frame draws transmissive surfaces in the transparent pass over a copy of the lit frame behind them.

White glass reflects and refracts exactly what it receives:
- on the CPU, within 1% (IOR 1.33 to 2.4, roughness 0.01 to 1, from outside and inside, thin-walled from both sides);
- in both renderers, within 0.25% (the path tracer offline and realtime, and deferred).

Two bugs older than this step were found on the way (section 3): the transparent pass had been running step 1's shader
since step 2, and it shaded a generated sphere's front from behind.

## 2. What was built

- **T1, the module.**
  - **Refraction:** Walter et al. 2007's rough dielectric BTDF on GGX's visible normals, unbounded (bounded VNDF leaves out
    the normals refraction needs): `ggxSampleVisible`, `ggxVisiblePdf`, `refractThrough`, `refractHalf`, `ggxRefract`
    (the form whose integral is the energy refracted: no eta² radiance scaling, which cancels through a closed surface) and
    `ggxRefractPdf`.
  - **The base:** `transmission_weight` T takes the dielectric's share under the specular from the diffuse,
    `(1 - metalness) T (1 - dielectricAlbedo(wo))`, tinted by `transmission_color`, spread over the refraction lobe by its
    single-scatter albedo (Turquin 2019's scaling, so white glass passes all of it). From inside a solid the interface is
    at 1 / eta, and both its lobes are divided by their single-scatter sum (section 3). Thin-walled: either side is the
    outside and the refraction is the reflection lobe mirrored through the surface. Lobe 4 is the transmission;
    `transmissionAlbedo()` joins `albedo()`.
  - **The thin film:** Belcour & Barla 2017's Airy reflectance over the dielectric's F0 or the metal's F82 base, in RGB by
    their Gaussian fits of the colour matching functions, lerped by `thin_film_weight` (`airyReflectance`). The energy
    tables stay the film-less Fresnel's (O13).
  - **Dispersion:** `dispersedIor(nd, abbe, scale, lambda)`, OpenPBR's formula.
  - **Tables:** EssT (refraction entering, relative IOR n), EssDIn and EssTIn (reflection and refraction leaving, 1 / n),
    each [n][rough][mu] with 32 points of n. Every earlier table is unchanged byte for byte; the header has 85,184 floats.
- **T2, data.**
  - `ecs::Material` gains `transmission_weight`, `transmission_color`, `transmission_depth`,
    `transmission_dispersion_scale`, `transmission_dispersion_abbe_number`, `geometry_thin_walled`, `thin_film_weight`,
    `thin_film_thickness` (micrometres: the unit `um` joins the registry's) and `thin_film_ior`, with OpenPBR's defaults.
  - The material table grows from 12 to 15 rows (240 bytes): row 12 `transmission_color`, `transmission_weight`; row 13
    `transmission_depth`, `geometry_thin_walled`, dispersion's scale and Abbe number; row 14 the thin film.
    `material/material_layers.slang`'s `applyTransmissionRows` reads them. The instantiate key holds 45 values.
  - Import: `KHR_materials_transmission`'s factor and `KHR_materials_volume`'s attenuation colour and distance. Assimp
    5.4.3 reads neither `KHR_materials_iridescence` nor `KHR_materials_dispersion`.
  - A transmissive material reads as Blend on the host (`MaterialTable::alphaMode`): the transparent pass draws it, no
    cascade or G-buffer list holds it, and its TLAS instance is non-opaque.
- **T3, the path tracer.**
  - A back-face hit on a transmissive solid (`kHitBack`) is shaded in its outward frame, so the module sees wo below it.
  - A refraction crosses the surface: the next ray starts on the far side, a solid's inside absorbs every segment by
    `exp(-sigma_a t)` with `sigma_a = -ln(transmission_color) / transmission_depth` (O11), and what the crossing ray meets
    counts in full rather than by MIS, because light sampling there only looks above the surface.
  - Dispersion: a path's first dispersive glass draws one of R, G and B (600, 550 and 450 nm), weighs it by 3 and keeps it
    (O12).
  - The cache update keeps ending its path at a refraction (realtime only).
- **T4, the deferred frame.**
  - The transparent pass copies the lit frame (`VfxRenderer::copyRefraction`, forward set binding 8) before it draws.
    A transmissive pixel reads it where its refracted ray stands 0.1 m on (`transmission_depth` when set), averaged over
    8 golden-angle taps on a disc of up to 6% of the frame's height by roughness², tinted and absorbed, weighted by
    `transmissionAlbedo`. `ForwardPush` gains the projection.
  - Glass reflects the prefiltered environment: RT GI's radiance at its pixel is the surface's behind it.
  - RT rays (`rt/alpha_test.slang`) pass glass with the chance of its transmittance, `T 0.96 lum(transmission_color)`,
    dithered by a hash of where they cross (O10). RT GI's hits read the transmission rows and bounce only what glass
    reflects (`specularAlbedo + diffuseAlbedo`).
  - The light loop takes no light from below a surface (`evaluateLight`, `evaluateSphere`): through glass that light is in
    the refracted frame already.
  - The G-buffer path reads the thin film (row 14) for opaque surfaces.
- **T5.** `assets/scenes/glass.scene.json` (clear, frosted, amber, a thin-walled filmed bubble, an anodised metal before a
  striped backdrop) and its goldens `cli_glass_deferred`, `_reference`, `_pt`.

## 3. Bugs found and fixed

- **The transparent pass ran OpenPBR step 1's shader since step 2.** Step 2 put `TransparentTextures` between
  `[shader("fragment")]` and `fragmentMain` in `vfx/transparent_mesh.slang`. The shader no longer compiled, and CMake's
  entry-point parse then found no entry point, so the file got no build rule and no error. The SPIR-V from before step 2
  stayed in use (the copy step kept its timestamp fresh for `ohao doctor`). It read the material table at 12 rows, so
  growing the table to 15 made every blended mesh vanish (`vfx_gpu_test`, the transparency and FlightHelmet goldens).
  `shaders/CMakeLists.txt` now fails the configure step when a file's `[shader(...)]` attributes and parsed entry points
  differ.
- **The transparent pass shaded a generated sphere's front from behind.** It turned the normal by `SV_IsFrontFace`, and
  the generated sphere is wound inside out (the G-buffer already orients by the world position's screen derivatives). It
  now does what the G-buffer does. The panes in the goldens agree either way.
- **Glass seen from inside, between two table IORs,** read 1.1% dark at roughness 1: TIR moves energy between the inside
  reflection and refraction sharply with the IOR. Both are now divided by their sum (which interpolates cleanly), and the
  glass tables have 32 points of n instead of 16.
- **Rough glass in the path tracer** lost 4–8% to the MIS weight of a ray light sampling could not draw (fixed above).
- **Deferred glass** lost its reflection to RT GI's radiance at its pixel (fixed above).
- **A double cosine** in the thin-walled lobe and the inside reflection (`ggxSingle` is f cos already), found by the CPU
  furnace.
- **The coated glass's sampler** rejected its own refraction (a lobe-number check meant for the coat and fuzz).

## 4. Spec and plan corrections

- **The thin film's refraction** keeps the film-less `1 - F` (the plan said the film's).
- **Inside a solid** the two lobes share their single-scatter sum (the plan had the refraction fill `1 - EssDIn`).
- **O14** (thin-walled windows passing the path tracer's shadow rays) is not built: a light behind a window reaches the
  path tracer through BSDF sampling only.

## 5. Results

- **`material_tests`:** 20/20 in 8 minutes (`t3_defaults`, `t3_sampling_chi2` over the sphere with 64 x 64 bins and
  12 x 12 sub-samples, `t3_albedo`, `t3_white_furnace` (worst 0.8%), `t3_thin_film`, `t3_dispersion`). Steps 1 and 2 are
  unchanged.
- **`check_material.py`:** 42/42. White glass at roughness 0.05 and 0.5 and a thin-walled pane at 0.2, face and rim:
  offline 0.9998/1.0001, 1.0024/0.9992, 0.9994/0.9993; realtime the same; deferred 1.0000/1.0000 for all three.
- **Goldens:** 39 of 39; the three glass goldens are new. No other golden moved.
- **`vfx_gpu_test`:** 44/44.

## 6. Known gaps

Closed since (`docs/changes/2026-11-01-openpbr-gaps.md`): deferred refraction is traced through the solid; the path
tracer's shadow rays pass panes (O14). The path tracer's bounce and shadow rays had not met glass at all (a mask).

- **Deferred refraction is one screen-space offset:** a solid glass ball does not invert what is behind it as the path
  tracer's does; glass behind glass, and effects drawn after the copy, are not seen through glass.
- **Shadows through glass** are dithered (O10) in RT direct light and GI; no caustics in the realtime frame. The path
  tracer's shadow rays stop at glass (O14 not built).
- **The realtime path tracer's determinism** was found sensitive to code shape. One formulation of the refraction step,
  identical in what it computes for opaque surfaces, made two captures of the closed box differ. The formulation in the
  tree repeats bit for bit; the cause was not found.
- **Not tested on the GPU:** the volume's absorption against a closed form (a slab of known `sigma_a`), dispersion's
  colour separation.
- **Not imported:** the transmission and volume textures, `KHR_materials_iridescence`, `KHR_materials_dispersion`.
- **Deferred ignores dispersion;** the thin film's colours do not move the energy tables (O13).
- **Budgets:** the transparent pass's copy and glass shading are not benchmarked.

## 7. How to run

```
build/Release/material_tests.exe t3_white_furnace
python tests/renderer/check_material.py
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_glass_deferred
build/Release/ohao run assets/scenes/glass.scene.json --render out.png --mode rt_offline --spp 256 --denoise none --ev100 0
```
