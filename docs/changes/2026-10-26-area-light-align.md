# Area lights aligned: the deferred frame lights the path tracer's rectangle

**Spec:** none. Frank's ruling (2026-10-26, final): align the deferred frame to the path
tracer. A real rectangle area light in deferred, in the path tracer's units `I/(4πr²)`; the
path tracer unchanged.

**Status:** built and tested.

## 1. Summary

**Before:**
- The deferred frame lit an `AreaRect` as a spot (light type 2): a point at the
  rectangle's corner, `I/d²`, with the component's default 30°/45° cone along the entity's
  −Z axis.
- The path tracer samples the rectangle itself at radiance `I/(4πr²)`, one-sided toward
  `edge1 × edge2`, with no range.
- On the area-light test scene (`build/area.scene.json`: a floor, a back wall and a
  2 × 1.5 m panel 2.5 m up facing down), deferred lit the wall with a spot's disc and left
  the floor nearly dark: 0.05× the path tracer under the light.

**After:** the deferred frame lights the same rectangle, as light type 3:
- **Diffuse:** the rectangle's exact irradiance, Lambert's edge integral (the polygon's
  vector form factor).
- **Specular:** the representative point, the reflection ray where it meets the rectangle,
  with the lobe widened by its projected size.
- **Units:** radiance `I/(4πr²)`, one-sided, no range and no `1/d²`, as the path tracer has
  it.
- **Clustered lighting:** lists a rectangle in every froxel.
- **RT GI's bounce:** sees it as a point at its centre with a cosine profile.
- **The light's mesh:** the rectangle at the path tracer's radiance, black behind
  (`2026-10-26-visible-lights.md`); already consistent, unchanged.

## 2. What was built

- `shaders/includes/brdf/brdf_ggx.glsl`, `evaluateBRDFRect(surface, P, centre, e1, e2, Le)`:
  - **One-sided:** returns 0 behind the rectangle's plane.
  - **Diffuse:** `J = ½ Σ acos(v̂ᵢ·v̂ᵢ₊₁) normalize(v̂ᵢ × v̂ᵢ₊₁)` over the four corners, turned
    toward the rectangle. Irradiance is `Le · max(N·J, 0)`; the part below the horizon is
    not clipped.
  - **Specular:** the representative point, clamped onto the rectangle. The GGX lobe is
    evaluated at `a' = a + r_eq/(2d)`, with `r_eq = sqrt(A·cosθ/π)` the projected area as a
    disc. It is weighted as a source of intensity `Le·A·cosθ`, so a mirror shows the
    rectangle at `Le × F`.
  - Both go through `evaluateBRDFCore`, with diffuse toward `Ĵ`.
- `core/deferred_lighting.frag`: type 3 lights through `evaluateBRDFRect`; type 2 is the
  spot alone.
- `render/scene/light_table.cpp`: an `AreaRect` is type 3, packed as
  - `position` = the centre;
  - `direction.xyz` = edge 1, `direction.w` = range (unused);
  - `color.w` = `I/(4π·max(r, 0.01)²)`;
  - `params.xyz` = edge 2, `params.w` = the RT shadow slot.
- `compute/light_cluster.slang`: type 3 hits every froxel, as directional does. A rectangle
  has no range, so no froxel can drop it, and the lists still equal the full loop.
- `render/rt/rt_gi_technique.cpp`: the bounce lights from an area light as a spot at its
  centre, cones 0° and 90° (a linear ramp that is cos θ). It has intensity `Le·A` toward
  its face and range 1e9 (none).
- RT shadows were already right: they sample the rectangle (`rt_shadow_technique.cpp`).
- `shaders/shared/layout.json` (`LightGpu` docs), AGENTS.md (light semantics),
  ARCHITECTURE.md (clustered lighting), and three monograph units: materials/lights,
  gpu/buffers-alloc, hybrid/gi-technique.

## 3. Bugs found and fixed

- A deferred area light was a spot at its corner with an unauthored cone. Its energy and
  shape were unrelated to the path tracer's rectangle.
- RT GI's bounce saw an area light as a point of intensity `I`, in the old units.

## 4. Spec corrections

None.

## 5. Results

**Correctness of the integral.** The vector form factor against a 400,000-sample Monte
Carlo integral of the same rectangle:

| Point | Formula | Monte Carlo |
|---|---|---|
| Floor under the centre | 0.4121 | 0.4120 |
| Floor 2 m to the side | 0.0618 | 0.0618 |
| On the back wall | 0.0431 | 0.0432 |

**Deferred / path traced on the test scene.** Linear radiance with the tonemap inverted,
both renders at exposure 0.5, deferred 30 frames, the path tracer 512 spp with no denoiser,
24-px blocks, emitter and clipped pixels left out (`build/area_strata.py`,
`build/area_point.py`):

| | Before | After |
|---|---|---|
| Floor point under the light | 0.0262 vs 0.574 (0.05) | 0.557 vs 0.574 (**0.97**) |
| Strongly lit floor blocks (PT > 0.2), median | 0.061 | 0.763 |
| Strongly lit wall blocks, median | 0.068 | 0.834 |
| Lit floor (0.1–0.2) | 0.083 | 0.691 |
| All lit blocks, median | 0.165 | 0.587 |

**With bounces suppressed** (albedo 0.1, `build/area_dark.scene.json`):
- floor under the light: **1.04** (0.105 against 0.101);
- the lit wall: 1.00–1.03.

**What remains is not the area light.**
- **Dim regions (0.5–0.6):** the wall above the panel gets no direct light at all, so this
  is one GI bounce against the path tracer's many.
- **The near floor at a grazing view (0.55–0.88):** this is the grazing specular. Deferred's
  Fresnel is roughness-limited to `max(1 − roughness, F0)`; the path tracer's is full
  Schlick.
- A point light in the same low-albedo room shows the same pattern, lower: 0.37–0.69 on the
  near floor (`build/point_dark.scene.json`).

**Tests and goldens:**
- `tools/check.py` passes 36 of 38. The two failures (`monograph_structure` and
  `check_docs`, which runs it) were stale citations, since fixed: the monograph now passes,
  16/16, and `check_docs` 105/105.
- `engine_tests`, `light_cluster_gpu_test` and `light_emitter_gpu_test` (whose room has an
  area light) pass.
- Render goldens 22/22 unchanged: no golden scene has an area light.

**Look shots** in `C:/Users/djmax/ohao_refs/look_shots/area_light_align/` (before = HEAD
`9f73679`):
- `before_look_deferred.png` and `before_look_pt.png`: deferred shows a spot's disc on the
  wall and a dark floor; the path tracer shows a soft pool under the panel.
- `after_look_deferred.png` and `after_look_pt.png`: the same pool and wall glow in both.
- Deferred renders with TAA, SSAO and bloom at exposure 1; the path tracer at 256 spp.

**Benchmarks:** not rerun. No benchmark scene has an area light.

## 6. Known gaps

- **Horizon.** The polygon irradiance is not clipped at the horizon. A rectangle partly
  below a surface's horizon is under-counted there, and the total is clamped at 0 (LTC or
  Frostbite's horizon handling would fix it).
- **Mirrors.** Specular is a representative point with a widened lobe, the same
  approximation as the sphere lights: a mirror shows a soft rectangle, not a crisp one.
- **Froxel lists.** Every area light takes a slot in every froxel list (256 per froxel).
  That suits a few panels, not hundreds.
- **Range.** `range` means nothing for an area light in either renderer.

## 7. How to run

```
build/Release/ohao run build/area.scene.json --render d.png --mode deferred --frames 30 --exposure 0.5
build/Release/ohao run build/area.scene.json --render p.png --mode rt_offline --spp 512 --denoise none
build/Release/light_cluster_gpu_test.exe
build/Release/light_emitter_gpu_test.exe
```
