# OpenPBR step 4: subsurface scattering

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md` (section 8, step 4); plan
`docs/plans/2026-11-01-openpbr-step4.md`, tasks T1–T4.
**Status:** built (T1–T4). Rulings O15–O18 are provisional until Frank rules on them. O17 is built in part: the
specular is spread with the diffuse.

## 1. Summary

OpenPBR Surface 1.1.1's subsurface is in the module both renderers shade with: `subsurface_weight` takes the
dielectric's diffuse slot for a volume of `subsurface_color`, its mean free path `subsurface_radius x
subsurface_radius_scale`. The path tracer walks the volume (Chiang, Kutz & Burley 2016). Everything else shades a local
diffuse lobe in `subsurface_color`, which the deferred frame's SSS pass spreads by Burley's normalized diffusion profile at
the pixel's world-space free path. A white subsurface sphere reflects what it receives within 0.2% in the path tracer
(offline and realtime) and in deferred.

## 2. What was built

- **T1, the module and the data.**
  - `MaterialInputs` gains `subsurfaceWeight`, `subsurfaceColor` and `subsurfaceLocal`. The base's diffuse slot is
    `(1 - S) EON(base) + S EON(subsurface_color)` when local (`slotEval`, `slotAlbedo`; S = 0 runs the old arithmetic).
    Walked (`subsurfaceLocal` false, the path tracer's), the subsurface is lobe 5: what enters is
    `(1 - metalness)(1 - T) S (1 - the specular's albedo)`, drawn cosine-weighted into the surface; `pdf()` leaves it out
    (it is no density over directions), and the base samples itself as it does glass (`delegated()`).
  - `subsurfaceSingleScatter`: van de Hulst's inversion of `subsurface_color` to the single-scatter albedo.
  - `ecs::Material` gains `subsurface_color`, `subsurface_radius` (metres, O15), `subsurface_radius_scale` and
    `subsurface_scatter_anisotropy` with OpenPBR's defaults; `subsurface_weight` keeps its field (O18). The material table
    grows to 17 rows (272 bytes): row 15 the colour and radius, row 16 the scale and anisotropy
    (`applySubsurfaceRows`). The instantiate key holds 53 values. A subsurface material is a layered one
    (`MaterialTable::layered`: RT direct light's layered kernel).
- **T2, the path tracer.** Lobe 5 starts `walkSubsurface`: from 1 mm under the surface, exponential steps at
  `sigma_t = 1 / free path`, Henyey–Greenstein scattering, the single-scatter albedo a step, Russian roulette after 8 steps,
  at most 1024. Meeting the surface leaves it, cosine-weighted about the outward normal (O16), and the path goes on from
  there, what it meets counted in full. A walk follows one channel, chosen uniformly and weighed by 3 (section 3). Its
  random numbers are a PCG stream of its own: a walk outlasts any Sobol group.
- **T3, the deferred frame.** `postprocess/sss.slang` reads each pixel's material through the G-buffer's layers target
  (bindings 7 and 8): Burley's d per channel (the free path over `s = 1.85 - A + 7 |A - 0.8|^3`), taps at the pixel's world
  footprint (`SSSPush.params.y`, from the projection), at most 10 pixels a step, weighed by `e^(-r/d) + e^(-r/3d)`, and the
  result mixed in by `subsurface_weight`. RT direct light, RT GI's hits and the transparent pass shade the local lobe.
- **T4.** `assets/scenes/subsurface.scene.json` (skin-like, marble, jade, and a plain diffuse sphere of the skin's
  colour) and its goldens `cli_subsurface_deferred`, `_reference`, `_pt`.

## 3. Bugs found and fixed

- **Spectral MIS lost energy.** Drawing each step on a uniformly chosen channel and weighing by the channels' mean density
  let skin-like free paths, 4x apart, multiply their weight ratios over hundreds of steps into a tail no frame samples: a
  white sphere at radius 0.02 read 0.924. A walk now follows one channel (0.9998 at radius 0.05).
- **A non-absorbing volume's walk** ends only by leaving: its step count's tail (about N^-1/2) passes any cap. A white
  volume whose free path is under ~1/50 of the object loses energy to the 1024-step cap (radius 0.02 with skin's scale,
  blue 5 mm in a 1 m sphere: 0.986). Absorbing volumes end by roulette long before it.

## 4. Spec and plan corrections

- **O17:** the SSS pass spreads the lit colour, specular included; the plan's separate diffuse image is not built.
- **The walk's spectral sampling:** one channel a walk, not the channels' mean density (section 3).

## 5. Results

- **`material_tests`:** 24/24 (`t4_defaults`, `t4_van_de_hulst`, `t4_local`: chi-square, `albedo()` and a white local
  subsurface within 1%; `t4_walked`: what enters plus what reflects is white within 1%, lobe 5 points inward, `pdf()` 0
  below).
- **`check_material.py`:** 48/48. White subsurface spheres, face and rim: radius 0.05 offline 0.9998/0.9984, realtime the
  same, deferred 0.9985/0.9986; radius 0.5 with anisotropy 0.5: 1.0003/0.9991, deferred 0.9985/0.9986.
- **Goldens:** 42 of 42; the three subsurface goldens are new; no other golden moved.

## 6. Known gaps

- **The path tracer's subsurface is noisy in colour:** a walk lights one channel at three times its weight.
- **The deferred spread blurs the specular** and does not carry light through thin parts (an ear lit from behind).
- **White volumes under ~1/50 of the object** lose energy to the walk's step cap (section 3).
- **The walk leaves through no rough interface** (O16) and treats any surface it meets as the boundary (a mesh inside a
  mesh ends the walk).
- **Not imported:** glTF has no subsurface extension in use; `KHR_materials_diffuse_transmission` is not read.
- **Budgets:** the walk's cost and the SSS pass's per-pixel profile are not benchmarked.

## 7. How to run

```
build/Release/material_tests.exe t4_walked
python tests/renderer/check_material.py
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_subsurface_deferred
build/Release/ohao run assets/scenes/subsurface.scene.json --render out.png --mode rt_offline --spp 512 --denoise none --ev100 0
```
