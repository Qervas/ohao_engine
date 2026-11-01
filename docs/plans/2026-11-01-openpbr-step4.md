# OpenPBR step 4: subsurface scattering

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md`, section 8, step 4 ("skin and marble against the path tracer").
**Status:** T1–T4 built (2026-11-01): `docs/changes/2026-11-01-openpbr-step4.md`; the separate diffuse
image built since (`docs/changes/2026-11-01-openpbr-gaps.md`). Frank: "continue ... until all done, all renderer work done" (autopilot).
Rulings O15–O18 (below) are provisional until he rules.

**Goal:** OpenPBR Surface 1.1.1's subsurface in the module, a random walk in the path tracer, and a diffusion profile in
the deferred frame in place of the screen-space blur's strength. A skin-like and a marble material enter the goldens.

## The model (OpenPBR 1.1.1)

- **Subsurface** (`subsurface_weight` S 0, `subsurface_color` 0.8, `subsurface_radius` 1, `subsurface_radius_scale`
  (1, 0.5, 0.25), `subsurface_scatter_anisotropy` 0): the dielectric base under the specular is
  `lerp(lerp(diffuse, subsurface, S), transmission, T)`. `subsurface_color` is the albedo the volume shows (its multiple
  scattering), `subsurface_radius x subsurface_radius_scale` the mean free path per channel, in metres (ruling O15).
- **The path tracer** (Chiang, Kutz & Burley 2016; Christensen & Burley 2015): light that the specular leaves,
  `(1 - metalness)(1 - T) S (1 - E_spec(wo))`, enters by a cosine-weighted direction into the surface and walks the
  volume: `sigma_t` = 1 / the mean free path, single-scatter albedo from `subsurface_color` by van de Hulst's inversion,
  Henyey–Greenstein scattering of `subsurface_scatter_anisotropy`, each step's distance drawn on one channel chosen
  uniformly and weighed by the channels' mean pdf (spectral MIS). A step that reaches the surface leaves it there, by a
  cosine-weighted direction out (ruling O16: no second specular interface on the way out). At most 256 steps; Russian
  roulette as on a path.
- **Everything else** (deferred lighting, RT direct light, RT GI's hits, the transparent pass) evaluates the subsurface
  locally as a diffuse lobe of albedo `subsurface_color` (EON at the base's diffuse roughness), which the deferred frame then
  spreads: `OpenPBRBase.subsurfaceLocal` (true) chooses it; the path tracer turns it off and walks.
- **The deferred spread** (ruling O17): the SSS pass blurs a subsurface pixel's diffuse light by Burley's normalized
  diffusion profile (Christensen & Burley 2015, `R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r)`, d from the mean free path
  and the albedo) at the pixel's world-space radius, separably in screen space over the pixel's footprint. The lighting
  pass writes a subsurface pixel's diffuse light to its own image, so the specular is never blurred.

## Tasks

### T1. The module, the data, the CPU tests
- `MaterialInputs` gains the five parameters; `OpenPBRBase` the subsurface lobe (local: EON in `subsurface_color`; or
  walked: `subsurfaceShare(wo)` and lobe 5, sampled cosine-weighted into the surface).
- `ecs::Material` (`subsurface_weight` keeps its name, now OpenPBR's), `MaterialDesc`, the table's rows 15
  (`subsurface_color`, `subsurface_radius`) and 16 (`subsurface_radius_scale`, `subsurface_scatter_anisotropy`):
  `kMaterialRows` 17.
- `material_tests` `t4_*`: defaults; the local lobe's albedo, chi-square and white furnace; the van de Hulst inversion's
  round trip.

### T2. The path tracer
- Lobe 5 starts a random walk inside the mesh (`walkSubsurface`); the path goes on from where it leaves, what it meets
  counted in full.
- `check_material.py`: a white subsurface sphere in the furnace within 1%, its local lobe in deferred within 1%.

### T3. The deferred frame
- The lighting pass's subsurface image (diffuse light of subsurface pixels); `postprocess/sss.slang` blurs it by the Burley
  profile at each pixel's radius and adds it back. The material's radius comes from the table by the layers target's
  index.

### T4. Scene, goldens, docs
- `assets/scenes/subsurface.scene.json`: skin-like (red-tinted radius), marble (white, long radius), jade (green) spheres
  under the studio light; deferred, reference and path traced in the goldens; the change record.

## Rulings (provisional; Frank's to make)

- **O15, units.** `subsurface_radius` in metres (OpenPBR leaves the scene's unit); 1 cm skin is `subsurface_radius`
  0.01 with the default scale.
- **O16, the walk's exits.** Light leaves the volume by a cosine-weighted direction, not through the rough specular
  interface (Chiang's simplification): the white furnace holds exactly.
- **O17, deferred spread.** A separable screen-space Burley profile over each pixel's world-space radius, on the diffuse
  light only, replacing the blur's strength; thin parts (ears) do not transmit light from behind.
- **O18, `subsurface_weight` keeps the field** the screen-space blur's strength used: scenes that set it now get
  OpenPBR's subsurface (colour 0.8, radius 1 m by default: a soft, white scatter) unless they set the rest.
