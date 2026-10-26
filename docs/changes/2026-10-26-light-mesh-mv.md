# Light meshes write motion vectors; an area-light golden

**Spec:** none. GT polish after `2026-10-26-area-light-align.md`: the light meshes' missing
motion vectors (a known gap in `2026-10-26-visible-lights.md` and `-light-mesh-polish.md`),
and a golden for the area light built in `fc2371d`.

**Status:** built and tested.

## 1. Summary

- **Motion vectors.**
  - **Before:** a light's emitter mesh (the sphere or rectangle the camera sees) had
    `prevWorld = world`. A moving light's mesh wrote only the camera's motion, so TAA blended
    it with the floor and wall it had just left: darker and smeared toward where it was.
  - **After:** the mesh carries its light's last-frame placement and writes motion vectors as
    any mesh does.
  - **Camera motion** was already reprojected: the G-buffer's vertex shader takes
    `prevViewProj · prevWorld`, and a still light's `prevWorld = world` is correct.
- **Area-light golden.** `assets/scenes/area_light.scene.json` (the area-light test scene of
  the previous job, now checked in), as two exact goldens: `cli_area_light_deferred` and
  `cli_area_light_reference`, the same pattern as the Khronos pack.

## 2. What was built

- `render/scene/render_scene_types.hpp`: `LightDesc::lastFrameMotion`. This is the transform
  from this frame's world space to where the light was last frame (the light entity's
  `prevWorld · inverse(world)`), identity by default.
- `ecs/render_extract.cpp`: `rebuildLights` sets it when the entity's `prevWorld ≠ world`.
  - The extract rebuilds the lights whenever a light entity is `RenderDirty`. That is both the
    frame it moved and the frame after, when the transform system lets `prevWorld` catch up.
  - So the motion lasts one frame, as a mesh's does.
- `render/scene/render_scene.cpp`, `syncEmitters`: an emitter's `prevWorld` is
  `lastFrameMotion · world`.
  - Rigid with the entity: exact for a translated sphere or rectangle, and for a rotated
    rectangle (its edges are the entity's basis times `areaEdge1/2`).
  - When still, identity times `world`, bit for bit `world`: every golden is unchanged.
  - A spot's emitter re-derived for a new viewer (`setViewerPosition`) keeps the motion of the
    last `setLights`.
- `RenderScene::instance(slot)`: a slot's `InstanceGpu` as last written (host copy), so a test
  can read an emitter's matrices.
- `light_emitter_gpu_test`, two new cases:
  - **motion:** a point and an area light moved by (0.5, −0.25, 0.4) m. Their emitters'
    `world` is the new place and `prevWorld` the old (the sphere's scale the same in both). One
    `sync()` later, `prevWorld = world`.
  - **taa:** a point light moving 0.03 m (~1.5 px) a frame for 12 TAA frames (exposure 0.03,
    so the mesh is not clipped) against the same 12 frames with the light still at its last
    position. It compares the sphere plus 4 px around it.
- `assets/scenes/area_light.scene.json`:
  - A floor and a back wall (albedo 0.6, roughness 0.9).
  - One 2 × 1.5 m rectangle 2.5 m up, facing down: intensity 20, radius 0.5, so radiance
    `I/(4πr²)` = 6.4.
  - No environment.
  - `*.scene.json` is already pinned to LF in `.gitattributes`.
  - The scene schema check and `ohao doctor` glob `assets/scenes/`, so both cover it.
- `tests/golden/manifest.json`: `cli_area_light_deferred` (deferred, 30 frames, TAA, SSAO
  and bloom, exposure 1) and `cli_area_light_reference` (the same at `--quality reference`).
  Both exact, max_abs 0.
- AGENTS.md: the golden count, the light-mesh paragraph and `light_emitter_gpu_test`'s
  description.

## 3. Bugs found and fixed

- A moving light's mesh had no object motion. Under TAA its pixels took the background's
  history, and only the neighbourhood clip pulled them back.

## 4. Spec corrections

None.

## 5. Results

**Motion test** (`light_emitter_gpu_test` *motion*): before the fix the point light's
emitter had `prevWorld` (−2.50, 2.75, −1.60) = its world; after the fix (−3.00, 3.00,
−2.00), where it was.

**TAA** (*taa*: the sphere and 4 px around it, 357 px):

| | Mean value, moving / still | Mean \|moving − still\| | Signed mean over the changed box | Pixels changed by > 16 |
|---|---|---|---|---|
| Before (prevWorld = world) | 97.7 / 102.6 (−4.9) | 15.0 | −7.3 | 87 |
| After | 101.7 / 102.6 (−0.9) | 12.1 | −1.4 | 61 |

- The remaining difference is the sphere's edge: 28 pixels left of centre, 33 right.
  - The still light's edge has had 12 jittered frames to anti-alias; the moving one's history
    is a pixel or two behind, as for any moving mesh.
  - That edge is TAA's (velocity sampled at the pixel, not dilated), not the emitter's.
- At ~8 px a frame neither version ghosts much inside the sphere. Its 3×3 neighbourhood is all
  sphere, so the variance clip snaps any history to it: mean |moving − still| inside the
  sphere was 0 before and after.
- The difference is at slow motion, where the neighbourhood spans the old and new positions.

**Tests:**
- `light_emitter_gpu_test` 28/28.
- With the one line reverted, the two motion checks failed. The TAA values measured then
  (−4.9 and 15.0) are outside both TAA checks' bounds (2.5 and 13.5), which were set from the
  two runs.
- `tools/check.py`: 38/38, including `engine_tests` 85/85, `check_scene_schema` 21/21 (the
  new scene included) and `check_docs` 72/72. `ohao doctor` counts 7 scenes, every
  referenced asset present.

**Goldens:**
- 24/24: the 22 unchanged (the fix leaves a still light's matrices bit for bit) and the 2 new.
- The area-light goldens were captured, looked at and re-run: both pass exactly.
- Reference and realtime differ by max_abs 4 on 33% of the pixels, so the two entries test
  different paths.

**Look shots** in `C:/Users/djmax/ohao_refs/look_shots/light_mesh_mv/`:
- `before_taa_moving_point.png`, `after_taa_moving_point.png`, `taa_still_point_reference.png`:
  the *taa* case's last frame (480 × 270, exposure 0.03).
  - Before: the sphere is darker with a soft trailing edge.
  - After: as bright as the still light.
- `golden_area_light_deferred.png`, `golden_area_light_reference.png`: the new goldens. A soft
  pool on the floor under the panel, the wall lit above it, and the panel's mesh seen from
  below.

**Benchmarks:** not rerun. The extract adds one compare (and an inverse for a moved light)
per light rebuild, which runs only when a light changes. The emitter sync adds one matrix
product per light mesh.

## 6. Known gaps

- **TAA's edge.** TAA samples velocity at the pixel, without dilating it to the nearest
  depth. The background pixels a moving mesh just left keep zero velocity and lean on the
  clip. That holds for every mesh, not only light meshes.
- **Lighting is not reprojected.** A moving light's lighting on the floor and walls has no
  motion vector, since the surfaces did not move. TAA's clip and RT shadows' and RT GI's
  histories handle it as they do any lighting change.
- **No moving-light golden.** Goldens render a still world. The moving case is
  `light_emitter_gpu_test`'s.

## 7. How to run

```
build/Release/light_emitter_gpu_test.exe [--keep]
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_area_light_deferred
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_area_light_reference
```
