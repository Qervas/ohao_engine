# Light measurement aligned, and lights you can see

**Spec:** none. Owner job of 2026-10-26, following `2026-10-26-emissive-hotspot.md` (whose
hotspot was blocked on light-measurement rulings). Frank's rulings (t68s352): the path tracer
adopts deferred's `I/d²` for point lights, and cuts lights at `range` exactly as deferred does.
Visible light meshes: Frank, t68u.

**Status:** built and tested, all four parts.
- A: the path tracer measures point and spot lights as deferred does.
- B: path-traced goldens re-baselined.
- C: the deferred albedo fix applied (it now brings the renderers into agreement), the
  hotspot re-measured.
- D: point, spot and area lights are visible meshes in both renderers.

## 1. Summary

**A. Light units and range (path tracer).**
- A point light's sphere emitter had radiance `I/(4πr²)`, which radiates intensity `I/4`.
  Its radiance is now `I/(πr²)` (normalised by the projected area), which delivers exactly
  `I/d²` under both samplers.
- Spot lights already delivered `I`. They had two faults:
  - they sampled their emitter at the inner cone angle read as a radius;
  - they squared the cone ramp, where deferred's is linear.

  They now use the light's radius and the linear ramp.
- Point and spot lights are windowed by deferred's `(1-(d/range)⁴)²`
  (`includes/rt/pt_lights.glsl`).

**C. Albedo (deferred).**
- `gbuffer.frag` multiplied the vertex colour into the albedo. Primitives carry their colour
  as vertex colour and base colour both, so every wall was its colour squared (0.5 shaded as
  0.25).
- It is now base colour × albedo texture, as `pt_closesthit` computes it.
- With A in place this brings the two renderers into agreement (section 5), so it was applied.

**D. Visible lights.** `RenderScene::setLights` gives every point, spot and area light an
emitter instance: a unit sphere scaled to the light's radius, or a unit quad mapped onto its
rectangle. Its radiance sits in the instance's `giAlbedoPacked.rgb`, flagged
`kInstanceEmitter`.
- **Radiance.**
  - Point and spot: `I/(πr²)`, so from afar the sphere is the source of intensity `I` that
    lights the scene.
  - Area: the path tracer's own radiance for it, `I/(4πr²)`.
- **No self-shadowing, no double count.** The mesh has its own TLAS mask
  (`kTlasMaskEmitter`). Only the path tracer's camera ray passes `kRayMaskCamera`; every
  other ray passes `kRayMaskScene`: path-tracer light samples, bounces and specular chains,
  and deferred's RT shadow and GI queries.
- **Shadows.** CSM cascades skip `kDrawEmitter` draws.
- **The G-buffer** marks an emitter with emissive alpha 1 and metallic −1:
  - lighting outputs its radiance alone;
  - SSR treats a hit on it as a miss;
  - RT shadows and GI skip its pixels as they skip sky.
- **The path tracer's closest hit** returns its radiance (black behind) and ends the path, as
  at a miss.
- **Shared constants.** The masks and flags are in `shaders/shared/layout.json`, one source for
  C++, GLSL and Slang. The dead `rt_visibility.hpp` and `rt/includes/rt_masks.glsl` are
  deleted: four unlinked copies of `0x01`.

## 2. What was built

- `shaders/includes/rt/pt_lights.glsl` (A, new):
  - `ptLightRadius`, `ptEmitterArea`, `ptRangeWindow`;
  - the ray-mask rule;
  - includes `shared/layout.glsl`.
- The three raygens:
  - the light blocks use it (A);
  - every trace passes `kRayMaskScene`, the camera ray `kRayMaskCamera` (D).
- `light_table.cpp`:
  - `GPULight.extra2 = (range, radius)` for points and spots (A);
  - `lightEmitter(LightDesc)`, the pure light → mesh rule (D).
- `render_scene.cpp` (D):
  - emitter assets, registered at the first light that needs them: a 24×12 UV sphere
    (counter-clockwise from outside, asserted per triangle at build), a unit quad and one
    black double-sided material;
  - `syncEmitters` (slots reused in light order, so an unchanged list uploads nothing),
    `setLightEmitters(bool)` and `lightEmitterSlots()`;
  - the TLAS mask per instance class and `Draw::emitter` → `kDrawEmitter`.
- `InstanceDesc::emitter` and `kInstanceEmitter` (D).
- `core/gbuffer.frag`:
  - albedo = base × texture (C);
  - the emitter branch (radiance on `gl_FrontFacing`, black behind) (D).
- `core/deferred_lighting.frag`: an emissive alpha of 1 outputs the radiance alone (D).
- `postprocess/ssr.slang` + `SSRPass`: binding 6, the emissive target; a march that lands on
  an emitter breaks as a miss (D).
- `cull/instances.slang`: cascades skip `kDrawEmitter` (D).
- `rt/shadow_trace.slang`, `rt/gi_trace.slang`: `kRayMaskScene`; metallic < 0 is skipped as
  sky (D).
- `rt/pt_closesthit.rchit`: the emitter branch (D).
- Tests:
  - `tests/renderer/light_emitter_gpu_test.cpp` (new, registered `gpu`);
  - `cull_parity_gpu_test` t06 counts the light's emitter draw.
- Docs and monograph:
  - AGENTS.md: the light-semantics gotcha rewritten, an emitter gotcha, the new suite;
  - ARCHITECTURE.md;
  - monograph units: hybrid/visibility (mask sections rewritten), hybrid/shadow-technique,
    path-tracer/as, shaders/rt-shaders, shaders/rt-includes, scene/render-scene,
    deferred/gbuffer, graph/meshlets, gpu/rt-build, gpu/bindless.

## 3. Bugs found and fixed

- **Part A, path tracer.**
  - Point lights at a quarter of their intensity.
  - Spot lights sampled at the inner cone angle as a radius, with a squared cone ramp.
  - Range ignored.
- **Part C, deferred.** The G-buffer squared primitives' colour.
- **Found while building D: RT shadows' `shadowLightsDropped` counted an emitter's own pixels.**
  - The shadow trace ran on them, and a pixel on a spot light's sphere counted the spot plus
    four point lights. `engine_tests` sp2b_t09's spot case caught it.
  - Emitter pixels are now skipped by RT shadows and GI. This also shrank the 1-LSB
    denoiser halo around emitters from 242 to 226 pixels.

## 4. Spec corrections

None (no spec). The previous record's open question (section 6 there) is answered by the
rulings above.

## 5. Results

**Measurements against the path tracer.** `ohao run`, linear, deferred at exposure 0.5 so
nothing clips.

The one-wall light test (`build/light_units.scene.json`: point light intensity 9 at 3 m, range
100, no environment) at the wall's centre:

| | Deferred | Path tracer | Deferred / PT |
|---|---|---|---|
| Before A | 0.0803 | 0.0397 | 2.02 |
| After A (old albedo) | 0.0803 | 0.1550 | 0.518 |
| After C | — | — | 1.011 |

- `I/d²` predicts 0.153 for the path tracer.
- After A the remaining gap is exactly the squared 0.5 albedo; after C it is gone.
- Toward the wall's edges the ratio is 1.01–1.09 (Burley against Lambert at grazing angles).
- Falloff, side over centre: path tracer 0.211, deferred 0.215.
- With range 5 m both renderers reach zero at the same x.

The helmet room's right-wall hotspot (`helmet_game.scene.json`):

| | Deferred | Path tracer | Deferred / PT |
|---|---|---|---|
| Hotspot median, radiance | 2.368 | 2.608 | 0.91 |
| Hotspot peak, radiance | 9.51 | 11.48 | — |

- Before the job the path-traced peak was 1.61–1.95 against deferred's 6.79 (2.9–3.2×).
- Elsewhere in the room deferred stays darker: right wall away from the hotspot 0.22, left
  wall 0.68, back wall 0.09.
- These regions are reached by no light within its 10 m range. What lights them there is the
  path tracer's many bounces against deferred's one GI bounce, not light measurement.

Helmet/wall ratio:

| Render | Ratio |
|---|---|
| Deferred golden | 0.462 |
| Deferred, `ohao run` | 0.480 |
| Path tracer, `ohao run` | 0.612 |
| Path tracer golden (`model_viewer`) | 0.630 |

The earlier 0.534 was measured before A and C.

**Visible lights (`light_emitter_gpu_test`, 18/18, validation on, no messages).**
- Three emitter instances (point, spot, area), none for the sun; none when turned off.
- Lights out of view, meshes on vs off: max_abs 0 on 0 pixels in all three modes (deferred
  with RT shadows and GI, deferred with CSM, path traced).
- In view, path traced: nothing changed outside the meshes' footprints.
- In view, deferred:
  - outside the footprints, max_abs 1 on 226 pixels, all within 24 px of a mesh (the GI and
    shadow denoisers' neighbourhoods see its depth);
  - nothing past 24 px.
- Each emitter's centre pixel is bright: 241–255 with its mesh, 44–180 without.

**No lighting regression.** I built once with the meshes off by default and rendered all 22
goldens against their previous baselines:
- 22/22 passed;
- deferred max_abs 0;
- path traced within the floor (cli_cornell_box 2 px and cli_cornell_box_rt_realtime 3 px at
  1 LSB).

So the masks, the SSR binding, the G-buffer reordering and the trace skips change nothing but
the meshes themselves.

**Goldens re-baselined (with the meshes, each looked at, manifest notes per golden).**

| Golden | max_abs | Pixels changed | rmse |
|---|---|---|---|
| `cornell_box` | 209 | 1.8% | 14.6 |
| `cornell_box_deferred` | 242 | 2.1% | 18.2 |
| `cornell_box_reference` | 242 | 2.0% | 18.2 |
| `cornell_box_rt_realtime` | 233 | 3.1% | 18.1 |
| `damaged_helmet` | 228 | 1.8% | 20.1 |
| `damaged_helmet_deferred` | 219 | 32.8% | — |
| `helmet_game_deferred` | 219 | 32.8% | — |
| `helmet_game_reference` | 210 | 32.5% | — |
| `khronos_flight_helmet_deferred` / `_reference` | 220 / 211 | 51% | — |
| `khronos_metal_rough_spheres_deferred` / `_reference` | 212 / 204 | 30% | — |
| `khronos_sponza_deferred` / `_reference` | 233 | 36.7% | — |

Cornell's 12 spheres, and the helmet room's two ceiling lights in the path-traced
`damaged_helmet`, account for the small changes. The large ones are the meshes' bloom glare
in the post-processed captures (the heatmap of `damaged_helmet_deferred` is a halo centred on
the two spheres). The earlier parts' goldens are in their own commits.

**Gates.**
- `tools/check.py` passed 37/37 after C.
- With D, `tools/check.py --all`: 38 of 43 passed (the 22 render goldens among them).
  - `monograph_structure` and `check_docs` (which runs it) failed on two stale cites, now
    fixed. Both pass alone: 16/16 and 105/105.
  - The three benchmarks were **noisy**: the machine was loaded (other processes busy; CPU
    calibration over 25% above its reference). They are neither passes nor regressions.
  - `render_bench` stopped at its calibration, so there is no measurement of the emitter
    spheres' cost in bench10k/100k yet.
  - `scene_bench` (CPU, the ECS; nothing here touches it) and `serve_bench` were noisy too.

## 6. Known gaps

- **Bloom.**
  - A light's mesh is as bright as the light really is: a 0.1 m sphere of intensity 600 has
    radiance ~19,000.
  - The bloom pass spreads a fraction of that over a third to a half of the frame in the
    helmet, Sponza and Khronos captures.
  - That is the bloom's threshold and strength, not the emitter; tuning it is a look
    decision for Frank.
- **Area lights still differ between the renderers.**
  - Deferred lights an AreaRect as a spot (light table type 2).
  - The path tracer lights it as its rectangle at `I/(4πr²)`, its radius unrelated to the
    rectangle.
  - The visible quad uses the path tracer's radiance. Unifying area-light semantics is not
    covered by the rulings.
- **Mirrors.** Mirrors and SSR do not reflect a light's mesh: the light's reflection is its
  analytic specular, and a perfect mirror shows no bulb.
- **Spot bulb.** A spot light's sphere shows its on-axis radiance from every direction; there
  is no housing outside the cone.
- **Moving lights.** A light's mesh has no motion vector (prevWorld = world): TAA can ghost
  a moving light's mesh.
- **Deferred halo.** Deferred keeps a 1-LSB halo within 24 px of an emitter: the RT GI and
  shadow denoisers' depth neighbourhoods.
- **Bench scenes.** They now carry one sphere per light: 1,000 more instances of 528
  triangles in bench10k/100k. Unmeasured: the render_bench run was noisy (section 5).
- **Light test scenes.** The one-wall and range test scenes are scratch files in `build/`,
  not checked in.

## 7. How to run

```
build/Release/light_emitter_gpu_test.exe            # --keep leaves the compared PNGs in build/
build/Release/cull_parity_gpu_test.exe
build/Release/engine_tests.exe
python tests/golden/render_golden.py tests/golden/manifest.json
python tools/check.py --all
```
