# OpenPBR step 3: transmission, volume absorption, thin film, dispersion, thin-walled

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md`, section 8, step 3 ("furnace with transmission; a glass
scene against the path tracer").
**Status:** T1–T5 built (2026-11-01): `docs/changes/2026-11-01-openpbr-step3.md`. Frank: "continue ... until all done,
all renderer work done" (autopilot). Rulings O10–O14 (below) are provisional until he rules; O14 built since
(`docs/changes/2026-11-01-openpbr-gaps.md`).

**Goal:** OpenPBR Surface 1.1.1's dielectric transmission in the module (rough refraction, the thin-walled slab), the
volume's absorption, the thin film's iridescence and dispersion; the path tracer a correct estimator of them; the
deferred frame drawing transmissive surfaces in the transparent pass by refracting the lit frame behind them. A glass
scene enters the goldens and the referee.

Every task ends with the engine running and every gate green. A task that moves goldens says which and why, and looks at
them before `--update`.

## The model (OpenPBR 1.1.1)

- **Transmission** (`transmission_weight` T 0, `transmission_color` 1, `transmission_depth` 0): the dielectric base is
  `lerp(diffuse, transmission, T)` beneath the specular. The specular dielectric becomes a BSDF: reflection by F,
  refraction by 1 − F through the GGX microfacets (Walter et al. 2007), both from `specular_roughness` and its
  anisotropy, at the relative IOR (`specular_ior` outside over 1, inverted inside).
  - **Energy:** single-scatter GGX loses energy in refraction as in reflection. The whole dielectric BSDF is scaled by
    `1 / E(mu_o)`, E its single-scatter albedo, reflection and refraction together (Turquin 2019's scaling, which
    OpenPBR's albedo scaling already makes non-reciprocal): a white, non-absorbing glass reflects and refracts exactly
    what it receives, from either side. New tables: `EssRT` and `EssRTInside` (the dielectric's reflection plus
    refraction, outside at eta in [1, 3] and inside at 1 / eta), and `EssDInside` (reflection alone inside), on EssD's
    16^3 grid.
  - **Volume** (ruling O11): `transmission_depth` > 0 gives the interior an absorption coefficient
    `sigma_a = -ln(transmission_color) / transmission_depth`; the path tracer attenuates every segment inside by
    `exp(-sigma_a t)`. At depth 0 `transmission_color` tints the refraction at the surface. `transmission_scatter` (a
    scattering medium) is subsurface's (step 4).
- **Thin-walled** (`geometry_thin_walled` false): a slab with no inside. Refraction passes straight through (the lobe
  about −wo, as rough as the specular), reflection is the specular's from either side; no volume.
- **Dispersion** (`transmission_dispersion_scale` 0, `transmission_dispersion_abbe_number` 20): the IOR by wavelength,
  `n(lambda) = n_d + (n_d - 1) / V (523655 / lambda^2 - 1.5168)` with V = abbe / scale (lambda in nm). The path tracer
  (RGB) draws one channel at a path's first dispersive refraction, weighs it by 3 and refracts at that channel's
  wavelength (ruling O12: 600, 550 and 450 nm). Deferred ignores it.
- **Thin film** (`thin_film_weight` 0, `thin_film_thickness` 0.5 µm, `thin_film_ior` 1.4): Belcour & Barla 2017's Airy
  reflectance of a film over the base (the dielectric, or the metal through its F82 Fresnel), in RGB by their Gaussian
  fits of the CIE sensitivities, replacing F lerped by the weight; refraction takes 1 − F. The albedo tables are read
  at the film-less Fresnel (ruling O13: the film moves colour, the energy tables do not follow it).

## Tasks

### T1. The module and its tables

- `shaders/material/openpbr.slang`: `MaterialInputs` gains the eight parameters; the base's dielectric gets the
  refraction lobe (eval, bounded VNDF sampling with the refracted direction's Jacobian, pdf), both sides
  (`wo.z < 0`: inside, eta inverted), thin-walled; the scaling by `EssRT`; the dispersion IOR; the Airy thin film.
  `OpenPBRSurface` keeps its lobes: lobe 4 is the transmission.
- `tools/material/openpbr_tables`: EssRT, EssRTInside, EssDInside, from the module's own code (op 8: a refraction
  sample's weight).
- `material_tests` `t3_*`: defaults; reciprocity of the refraction lobe with eta^2 (`f(wo, wi) / eta_i^2 =
  f(wi, wo) / eta_o^2`); chi-square of sampling over the sphere (64 x 64 bins); `albedo()` against the integral; the
  white furnace: white glass (IOR 1.5 and 1.33, roughness 0.01 to 1) from outside and inside, thin-walled, within 1%;
  the thin film at zero thickness the film-less Fresnel; dispersion's IOR at the d line.

### T2. Data

- `ecs::Material` gains the eight fields (OpenPBR's names and defaults); `render::MaterialDesc`, the material table's
  rows (12 → 14: `transmission_color`, `transmission_weight`; `transmission_depth`, `geometry_thin_walled`, the abbe
  number over the scale, `thin_film_weight`; `thin_film_thickness`, `thin_film_ior` and two free), the instantiate key.
- Import: `KHR_materials_transmission` (factor), `KHR_materials_volume` (attenuation colour and distance:
  `transmission_color`, `transmission_depth`; thickness ignored), `KHR_materials_iridescence` (factor, IOR,
  thickness maximum), `KHR_materials_dispersion` where Assimp reads them.
- A transmissive material is `kDrawTransparent` (the transparent pass draws it, as a Blend one) and its TLAS
  instance non-opaque (ruling O10: RT direct light's and GI's rays pass it with probability T (1 − F) x
  `transmission_color`, stochastic, so glass casts a tinted shadow; no caustics).

### T3. The path tracer

- Ray generation shades transmission from both sides: a hit on a back face of a transmissive material is inside;
  segments inside attenuate by the volume. Next-event estimation through a refractive surface is impossible (it
  bends): a light behind glass reaches the camera through BSDF sampling, weighed by MIS against nothing (ruling O14:
  thin-walled glass passes shadow rays straight, tinted, as a window must).
- `check_material.py`: the furnace with white glass, rough and smooth, solid and thin-walled, within 1%; a closed form
  for absorption: a slab of known thickness and `sigma_a` in front of a uniform emitter, `exp(-sigma_a d)` within 1%.

### T4. The deferred frame

- The `transparent` pass draws transmissive surfaces after the others' depth: the lit frame (before transparent
  surfaces) copied into a mip chain; a transmissive pixel samples it at the screen position of its refracted ray,
  offset by `transmission_depth` (or 0.1 m), at the mip of its roughness, tinted by the volume over that distance,
  weighed by T (1 − F); its reflection by the specular's albedo as today. Thin-walled: no offset.
- RT direct light and RT GI pass transmissive instances as above.

### T5. Scene, goldens, the referee, docs

- `assets/scenes/glass.scene.json`: a smooth glass sphere, a rough (frosted) one, a tinted absorbing one, a
  thin-walled pane, an iridescent coated sphere, before a textured backdrop under the studio light; deferred, reference
  and path traced in the goldens.
- The referee re-run with the glass scene; `render_bench` before and after (`transparent`, `pt.trace`); budgets
  proposed, not set; the change record.

## Rulings (provisional; Frank's to make)

- **O10, shadows through glass.** Rays meeting a transmissive surface pass it stochastically by its transmittance, so
  glass casts a tinted, softened shadow; caustics are left out of the realtime frame (the path tracer draws them where
  BSDF sampling finds the light).
- **O11, the volume.** Beer–Lambert from `transmission_color` at `transmission_depth`; at depth 0 the colour tints the
  interface. No scattering medium until step 4.
- **O12, dispersion in RGB.** One channel per path at 600, 550 and 450 nm, weighed by 3: colour fringes with RGB's
  coarseness, no spectral path tracer.
- **O13, the thin film's energy.** The film changes F's colour; the energy tables stay the film-less dielectric's.
- **O14, thin-walled windows pass shadow rays.** A thin-walled transmissive surface does not bend light, so the path
  tracer's shadow rays pass it tinted (next-event estimation stays possible behind a window).

## Tests

- `material_tests` (T1), `check_material.py` (T3), the goldens (T5, looked at), `check_parity.py`'s glass row,
  validation clean, `tools/check.py` green after every task.
