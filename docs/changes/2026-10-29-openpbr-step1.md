# OpenPBR step 1: the base layers in both renderers

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md` (sections 1–6, rulings O1–O5); plan
`docs/plans/2026-10-29-openpbr-step1.md`, tasks T1–T6.
**Status:** built (T1–T6). Gate 54 of 54 (quick, gpu, docs); goldens 32 of 32. Four Cornell budgets missed (`lighting`, `gbuffer`, `rt.gi`, frame); budgets are proposed for Frank's ruling (section 5).

## 1. Summary

The deferred frame and the path tracer used to implement different surfaces: deferred had Burley diffuse with a fitted
split sum, and the offline path tracer had a heuristic specular bounce that washed rough walls grey. The referee
(`docs/changes/2026-10-29-referee.md`) could not tell a deferred error from a path tracer error. Both now shade OpenPBR
Surface 1.1.1's base layers through one Slang module, `shaders/material/openpbr.slang`:

- EON diffuse;
- GGX specular with energy compensation;
- the exact dielectric Fresnel;
- F82-tint metals.

The path tracer is a correct estimator of that module, held by closed forms. Deferred is judged against it.

Deferred against the path tracer:

| Scene | Before step 1 | After T5 |
|---|---|---|
| Cornell | 0.476 | **0.881** (0.65 stops) |
| Blockout | 0.458 | **0.782** |
| Area light | 0.721 | **0.941** |
| Sponza | 1.352 | **1.019** |

## 2. What was built

- **T1, the module.** `shaders/material/openpbr.slang` contains:
  - EON diffuse, ported from the authors' MIT listings (`LICENSE-EON.md`);
  - GGX with bounded VNDF sampling;
  - the exact dielectric Fresnel, with `specular_weight`'s modulated IOR;
  - F82-tint metals;
  - Kulla-Conty compensation from energy tables the module integrates itself.

  `tools/material/openpbr_tables` writes `ohao/render/material/openpbr_tables.hpp`, and the `openpbr_tables --check`
  CTest holds it byte for byte. slangc compiles a kernel over the module to C++ (`tools/material/openpbr_cpu`), so
  `material_tests` runs the module's own code on the CPU:
  - chi-square of `sample()` against `pdf()`;
  - `albedo()` against its integral;
  - the white furnace;
  - reciprocity per lobe.
- **T2, the component (O1, O2).** `ecs::Material` holds OpenPBR's base under its own names and defaults, replacing
  `MaterialOverride`. Every scene file was rewritten forward only. The material table has 7 rows (112 B).
  `KHR_materials_ior`, `_specular` and `_emissive_strength` come in through Assimp 5.4.3. Instantiate's material cache
  keys on all 19 values.
- **T3, the reference path tracer.** `shaders/rt/path_tracer.slang` is the offline profile and
  `PathTracerShaderSet`'s default:
  - one path a sample;
  - light and environment sampling weighed against BSDF sampling by the power heuristic;
  - Russian roulette from the third vertex;
  - 32 bounces.

  A bounce ray that meets a light's emitter mesh weighs it by the light's index, `InstanceGpu.emitterLight.w`.
  `LightTable::setEmitterLights` flags lights without a mesh, which are sampled at full weight. Emissive proxies are
  raster only.

  For the referee: `ohao run --bounces N` and `render.capture {bounces}`. `tests/renderer/check_material.py` adds the
  white furnace in both renderers and a closed box of emitting Lambertian walls. The old raygens are deleted
  (`pt_raygen.rgen`, `pt_raygen_offline.rgen`).
- **T4, deferred on the same BSDF.**
  - `shaders/core/deferred_lighting.slang` replaces `deferred_lighting.frag`. It runs on
    `includes/lighting/clustered_lights.slang`, the clustered loop the forward passes share. That loop now evaluates
    OpenPBR: point and directional lights exactly; spheres and rectangles by representative point with the lobe
    widened (`withRoughness`).
  - The split sum and the diffuse fill take the module's own directional albedos (`specularAlbedo`, `diffuseAlbedo`).
  - The G-buffer has 8 attachments. `kSpecular` is RGBA8: specular colour, and weight / 1.5. `kOpenPBR` is RG8: diffuse
    roughness, and (IOR − 1) / 2.55. The codes hit the defaults exactly (170 and 50).
  - Albedo is multiplied by `base_weight`, and emission is colour × luminance × texture (O4). The 0.04 roughness floor
    is gone.
  - The OpenPBR tables are bound by every lit pass: lighting, the transparent pass, RT GI and the path tracer
    (`render::OpenPBRTablesBuffer`).
- **T5, GI's hits and back faces.**
  - A GI ray's hit resolves its own material at its uv (`rt/alpha_test.slang` `hitMaterial`, textures included) and
    bounces its OpenPBR albedo plus its emission. It used to bounce a flat per-instance 0.73 grey.
  - The G-buffer turns a back face's normal to the side the camera sees, by the cross of the position's screen
    derivatives.
- **T6, close.**
  - Deleted:
    - the `GiMaterialLegacy` component, with `giMaterialLegacyPacked` and `GiModelInputs`;
    - the layout constant `kInstanceGpuGiAlbedoOffset`;
    - the unused `includes/brdf/brdf_common.glsl` and `brdf_ggx.glsl`.
  - The instance field `giAlbedoPacked` is now `emitterLight`: rgb is an emitter mesh's radiance, w its light index,
    zero elsewhere.
  - AGENTS.md, ROADMAP (row 17), layout.json's docs and the monograph's material, deferred, hybrid and path tracer
    units are updated.

## 3. Bugs found and fixed

- **GGX's D was 0/0 at alpha 1e-4.** It was written as NoH²(a² − 1) + 1. It is now written in the half vector's
  components.
- **A linear 16-point μ axis** left an IOR 3 mirror 1.1% off at 85°. The tables now sample μ on sqrt(μ).
- **The old offline path tracer's specular bounce** was a jittered mirror with throughput 1, which washed every rough
  wall grey. The old `pt_raygen.rgen` was dead code.
- **Normal maps in the path tracer** were applied in a Frisvad basis. They now use the vertex tangents' frame, as the
  G-buffer does.
- **G-buffer 8-bit codes.** Storing specular weight as weight / 2 read 1.0 back as 0.996, and a white rough metal lost
  1.1% in the furnace. The codes now hit the defaults exactly.
- **Back faces.** The inside of a box was lit and traced from its outside. `gl_FrontFacing` disagreed with the generated
  sphere's winding, so the flip uses the screen derivatives.
- **GI bounced a flat grey** for every instance. Sponza's arcade bounced far lighter than its stone, so deferred was at
  1.352 of the path tracer there; it is now 1.019.
- **The path tracer's descriptor pool** was short of storage buffers once the tables were bound; it was sized up (11×).
- **A validation format mismatch.** `rtShadowVisibility` declared as `uint4` mismatched the R32G32 image; it is
  `Sampler2D<uint2>` now.
- **Instantiate's material cache** keyed on seven values, so two materials differing only in alpha shared one table
  entry.

## 4. Spec corrections

- **glTF's KHR extensions** come through Assimp 5.4.3, which already reads them. Our own glTF material reader waits for
  an extension Assimp lacks.
- **Scene files were rewritten forward only**, with no codec migration. An omitted roughness was written out as the old
  default 0.5, so each scene keeps its look.
- **The realtime path tracer stays GLSL** (`pt_raygen_realtime.rgen`, `ggx_aniso.glsl`) until the many-lights spec
  (O5). Only the offline profile is OpenPBR.
- **OpenPBR's layered whole is not reciprocal.** Its albedo scaling applies at wo alone, so `material_tests` checks
  reciprocity per lobe.
- **`gpu/vulkan/material*.cpp` stay.** The plan deleted them "if unused", but `engine_tests`, `renderer_pipeline_tests`
  and the model loader still use them.

## 5. Results

### The gate and the goldens

- **The gate:** `tools/check.py` (quick, gpu, docs) passed 54 of 54. `material_tests` 8/8, `check_material` 14/14,
  `check_analytic` 14/14.
- **The goldens:** 32 of 32 after T6. T6 moved no image. Over the step, T3 moved 6 path-traced goldens and T4 and T5
  each moved 24 deferred goldens; every one was looked at before its update, with notes in the manifest.
- **Expectations:** T6 removed a registered runtime type, which moved `schemaHash` in the CLI and serve expectations.
  Nothing else in them changed, and no `world.hash` moved.

### The white furnace

White materials under a uniform sky of radiance 1 must render as 1, within 1% (`check_material.py`). Each cell gives
face-on / rim:

| Material | Path tracer | Deferred |
|---|---|---|
| dielectric, roughness 0.3 | 0.9992 / 1.0003 | 0.9994 / 0.9994 |
| dielectric, roughness 1, diffuse roughness 1 | 0.9997 / 1.0004 | 0.9994 / 0.9994 |
| dielectric, IOR 3, roughness 0.6 | 0.9997 / 1.0002 | 0.9994 / 0.9994 |
| metal, roughness 0.3 | 1.0002 / 1.0000 | 0.9994 / 0.9994 |
| metal, roughness 1 | 1.0000 / 0.9992 | 0.9990 / 0.9992 |

Before the module, the old offline path tracer gave 1.16–1.18 on the dielectrics.

### The closed box

Emitting Lambertian walls, Le = 1, ρ = 0.5:

| Case | Expected | Measured |
|---|---|---|
| Every bounce | 2.0000 | 2.0000 |
| `--bounces 1` | 1.7500 | 1.7500 |
| `--bounces 0` | 1.5000 | 1.5000 |
| Deferred (direct and one bounce, as `--bounces 0`) | 1.5000 | 1.5029 (×1.0020) |

### The analytic referee

`check_analytic.py`, against `albedo/π × irradiance`:

| Case | Deferred | Path tracer |
|---|---|---|
| Sun at 90° | ×1.018 | ×1.011 |
| Sun at 45° | ×1.021 | ×1.014 |
| Point light | ×1.017 | ×1.009 |
| Spot light | ×1.017 | ×1.009 |

GI's bounce cases are all ×1.0000, and the spread case is ×0.9998.

### The referee: deferred against the path tracer

`check_parity.py`, after T6:

| Scene | deferred/PT | Stops |
|---|---|---|
| Cornell | 0.881 | 0.65 |
| Helmet | 0.835 | 0.93 |
| Spheres | 0.767 | 0.93 |
| Area light | 0.941 | 0.35 |
| Sponza | 1.019 | 0.33 |
| Blockout | 0.782 | 0.41 |

Before step 1, Cornell was 0.476, the blockout 0.458, the area light 0.721 and Sponza 1.352. The plan's referee at
`--bounces 1` was not run: `check_parity.py` has no bounce option yet.

### Benchmarks

Budgets are measured but not changed; Frank rules.

The machine never went quiet. `wait_quiet.py` waited 30 minutes, then 15, with dwm.exe at 1.3–1.4 cores the whole time.
These are GPU timestamps, which CPU load disturbs less, but they are not a quiet run.

`render_bench` passed 31 of 35. The four misses are all on `cornell_720p` (12 lights):

| Pass | Median | Budget | Before step 1 | Proposal |
|---|---|---|---|---|
| `lighting` | 0.682 ms | 0.32 | ~0.37 (a standing miss since SP5) | **0.70**, provisional, until an optimisation step |
| `gbuffer` | 0.113 ms | 0.10 | — | **0.12** |
| `rt.gi` | 0.125 ms | 0.11 | 0.130–0.139 before T5 | **0.13**, the ruling pending from `2026-10-29-gi-every-light.md` |
| frame | 1.531 ms | 1.5 | — | **1.6**, or hold 1.5 and win it back in lighting |

- **`lighting`:** OpenPBR's EON diffuse and its energy-table lookups for each of 12 lights cost about 0.05 ms a light,
  against about 0.022 ms before.
- **`gbuffer`:** two more targets (8 now).
- **`rt.gi`:** `hitMaterial`'s fetches and every light.

Every other scene passes: `helmet_game_720p` 1.082 ms, `helmet_game_1080p` 2.301, `bench100k_1080p` 4.941,
`play_hud_1080p` 2.339 (budget 2.45), `play_vfx_1080p` 1.842.

`serve_bench` passed 18 of 18. The inline 16-spp `rt_offline` capture, `window_capture_rt_inline`, is 123.7 ms (p95
133.4) against 150 ms, so no change is proposed.

## 6. Known gaps

- **Closed rooms are darker in deferred** (Cornell 0.881, spheres 0.767): one GI bounce against the path tracer's many.
  Multi-bounce GI is the next step.
- **The realtime path tracer is on the old BRDF** until the ReSTIR many-lights spec (O5).
- **NRD's composite for metals in the reference path:** first-hit direct light goes to the diffuse channel.
- **Sampler choice is ignored on both profiles.** The offline profile ignores `SamplerType::Sobol`. The realtime profile
  asks for PCG but its pipeline is specialised to Sobol.
- **A scene without an Environment** still differs between the renderers. That is the physical-sky sub-project
  (Hillaire, accepted).
- **Cornell's lighting is 0.682 ms** with 12 lights (section 5). An optimisation step should come before ReSTIR.
- **Snapshot bytes grew** because `Material` is larger. `scene_bench` was not rerun for this step.
- **Monograph diagrams:**
  - `deferred-gbuffer-channels.svg` draws the old six targets;
  - `pt_bindings.svg` lists bindings 0–35 only;
  - `materials-lights-packing.svg` still draws an area light collapsed to a cone;
  - `path-tracer-raygen-dual-path.svg` and `shaders-includes-common-oct-path.svg` are on disk unreferenced.
- **Unused includes** now have no includer: `light_attenuation.glsl`, `common.glsl`, `math.glsl`, `encoding.glsl`.

## 7. How to run

```console
$ build/Release/material_tests.exe > /dev/null
$ build/Release/openpbr_tables.exe --check ohao/render/material/openpbr_tables.hpp > /dev/null
$ python tests/renderer/check_material.py > /dev/null   # gpu
$ python tests/renderer/check_analytic.py > /dev/null   # gpu
$ python tests/renderer/check_parity.py > /dev/null   # gpu # slow
```
