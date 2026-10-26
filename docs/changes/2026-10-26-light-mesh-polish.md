# Light mesh polish: mirrors reflect light meshes, spot meshes follow their cone

**Spec:** none. Owner job of 2026-10-26, following `2026-10-26-visible-lights.md` and
`2026-10-26-bloom-tone.md`.

**Status:**
- (a) Mirrors and SSR show light meshes: built and tested.
- (b) Spot meshes are bright inside their cone, dark outside: built and tested.
- (c) Light-mesh motion vectors: not done. That would mean tracking each emitter's previous
  transform across frames, beyond the cheap scope.

## 1. Summary

**What a mirror showed before (measured, `build/mirror.scene.json`):**
- **Deferred:** lit point and spot lights as true points. A mirror showed a stretched
  pinpoint streak where the light's mesh should be.
- **Path tracer:** showed no reflection of a light at all, even at 256 spp. Its light
  sampling picks points on the light's sphere, and a mirror's GGX lobe is too narrow for
  them to land in. Its specular rays could not hit a light mesh (they trace
  `kRayMaskScene`).

**(a) Reflections, one rule per renderer, consistent:**
- **Deferred:**
  - A point or spot light with an emitter mesh carries its sphere radius
    (`LightGpu.params.z`; the slot was a shadow-map index no shader read, removed with its
    `lightSpaceMatrix` rule).
  - Its specular is a sphere light's (`evaluateBRDFSphere` in `brdf/brdf_ggx.glsl`, Karis
    2013): the representative point on the sphere closest to the reflection ray, and the
    GGX lobe widened to `a' = a + r/(2d)` and evaluated there.
  - On a mirror that is a disc at the mesh's radiance `I/(πr²)` × F, for lights on screen or
    off, with or without SSR.
  - SSR still treats an emitter pixel as a miss, so the analytic reflection stands in.
- **Path tracer:**
  - A near-mirror first hit (roughness < `ptMirrorRoughness`, 0.05) takes its specular
    light from its first specular ray, which traces `kRayMaskCamera` and so can hit a light
    mesh.
  - The closest hit returns the mesh's radiance with `envPdf` 0, so the miss branch weights
    it 1.
  - That hit's light samples drop their GGX specular, so the reflection is counted once.
  - Rougher surfaces keep light sampling as before.

**(b) Spots:**
- A spot's mesh radiance is the light's radiance × the path tracer's cone factor toward the
  camera: `clamp((cos θ − cos outer)/(cos inner − cos outer + 0.001))`, θ measured from the
  light to the camera.
- `RenderScene::setViewerPosition`, called each frame before `recordUpdates`, re-derives
  the spot emitters only when the viewer moved. No shader changed for this.

## 2. What was built

- `shaders/includes/brdf/brdf_ggx.glsl`:
  - `evaluateBRDFCore` (diffuse, multi-scatter and the sum, given a specular and its F);
  - `evaluateBRDF` keeps its exact expression tree;
  - `evaluateBRDFSphere` is new.
- `core/deferred_lighting.frag`: point and spot lights with `params.z > 0` use
  `evaluateBRDFSphere`.
- `render/scene/light_table.*`:
  - `params.z` = the emitter radius (`max(radius, 0.01)`, 0 for radius 0, area lights and
    emissive proxies);
  - `lightSpaceMatrix()` and the shadow-map-index rule deleted;
  - `lightEmitter(desc, viewer)` applies the spot cone.
- `RenderScene::setViewerPosition`, called from `render_dispatch.cpp` (both paths).
- The three raygens: `firstHitMirror`. The first-hit light sample drops its GGX term, and
  the first specular ray takes `kRayMaskCamera`.
- `pt_lights.glsl`: `ptMirrorRoughness`.
- `pt_closesthit.rchit`: an emitter hit sets `envPdf = 0`.
- `layout.json` docs; `buffer_setup.cpp` (the fallback sun's `params.z` = 0).
- Tests:
  - `light_emitter_gpu_test`: the downward spot's mesh is dark from outside its cone; new
    `spot_seen_from_inside_its_cone_is_bright`.
  - `engine_tests`: "raster lights carry their sphere radius, sorted by order" replaces the
    legacy shadow-map-index case.
- Docs:
  - AGENTS.md (the emitter gotcha), ARCHITECTURE.md;
  - monograph: hybrid/visibility, hybrid/shadow-technique, gpu/buffers-alloc,
    shaders/includes-lighting, shaders/includes-shadow, shaders/rt-shaders,
    scene/render-scene.

## 3. Bugs found and fixed

- **Deferred:** a smooth surface reflected a light as a pinpoint (point-light specular).
- **Path tracer:** a mirror reflected no light at all (light sampling cannot reach a delta
  lobe, and specular rays could not see light meshes).
- **Spot meshes:** they glowed on-axis from every direction.
- **Found while building:** `distributionGGX` floors its denominator at `EPSILON`. At mirror
  roughness (α ≈ 0.0016–0.0025) the true denominator is about 1e-10, so the GGX peak is capped
  far below `1/(πα²)`. Karis's `D(α)·(α/α')²` normalisation multiplied that capped value
  and rendered the reflection as a dark capsule with a bright rim. Evaluating the lobe at α'
  (peak `1/(πα'²)`, the normalisation's target) avoids the floor.

## 4. Spec corrections

None.

## 5. Results

**Tests:**
- `light_emitter_gpu_test` 20/20 with validation on, 0 messages:
  - downward spot's centre pixel from outside its cone: 0 (deferred) and 0 (path traced),
    against 44 / 157 without the mesh;
  - the spot turned toward the camera: 255 and 255;
  - lights out of view: byte-identical with and without meshes in all three modes.
- `engine_tests` 85/85; `tools/check.py` 38/38; render goldens 22/22 after the re-baseline.

**Goldens (all 22 re-baselined, each looked at, manifest notes):**

| Golden | max_abs | Pixels changed | rmse | What |
|---|---|---|---|---|
| Cornell deferred / reference | 241 | 18.7% / 18.9% | 6.9 | metal and glass spheres reflect the 12 lights as discs |
| Helmet room deferred / reference | 231 / 220 | 10% | 9.5 / 9.0 | the visor reflects the ceiling light |
| Khronos | ≤ 45 | 6–7% | 1.4–1.5 | |
| Sponza | 86 | 50% | 4.1 | the point light's highlights on stone and gold |
| Path-traced Cornell | 180 | 0.1% | 1.5 | the glass sphere, roughness 0.02, now reflects the lights |
| Path-traced Cornell realtime | 112 | 0.4% | 0.9 | |
| Path-traced helmet | 198 | 0.4% | 4.9 | smooth texels reflect the room's lights |

**Look shots** in `C:/Users/djmax/ohao_refs/look_shots/light_mesh_polish/` (before = HEAD
`2c65fee`, after = this change):
- `before_/after_mirror_deferred.png`, `before_/after_mirror_pt.png`: a mirror floor, a point
  and a downward spot light. Before, deferred shows a streak and the path tracer nothing.
  After, deferred shows a soft disc and the path tracer a crisp one. The before shots used
  floor roughness 0.05; the after shots use 0.02, below `ptMirrorRoughness` (at 0.05 the
  path tracer stays on light sampling).
- `before_/after_spot_inside_deferred.png`, `_pt.png`: a spot aimed at the camera (bright)
  beside one aimed away (dark after, on-axis bright before).
- `before_/after_spot_outside_deferred.png`, `_pt.png`: the light-mesh room, its downward
  spot seen from outside the cone.
- `before_/after_helmet_ssr_deferred.png`: the helmet room with SSR; the visor reflects the
  ceiling light.

**Benchmarks:** not rerun. Deferred adds a few ALU ops per light with a radius; the path
tracer adds a branch at the first hit.

## 6. Known gaps

- **Disc shape.** Deferred's mirror reflection is a sphere light's lobe: a soft,
  view-stretched disc, where the path tracer's is crisp. Energy and position agree.
- **Roughness 0.05 to ~0.2.** The path tracer reflects light meshes only below roughness
  0.05. Above it, light sampling converges to the reflection but is noisy at low spp.
- **Spot reflections.** A spot mesh's radiance is evaluated toward the camera, once per
  frame. Its reflection in a path-traced mirror uses that value, where the true one depends
  on the mirror point's direction. Deferred's analytic reflection uses the mirror point
  (correct).
- **Area lights.** Their reflections are unchanged (no radius; out of scope with the area
  light semantics).
- **Motion vectors.** Light meshes still have none (prevWorld = world).

## 7. How to run

```
build/Release/light_emitter_gpu_test.exe --keep
build/Release/ohao run build/mirror.scene.json --render out.png --mode deferred --frames 30
python tests/golden/render_golden.py tests/golden/manifest.json
```
