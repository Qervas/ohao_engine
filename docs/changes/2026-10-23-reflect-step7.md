# Sub-project 3, build step 7: mesh sources, instantiate and the asset resolver

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 7; "Components: authoring vs runtime", "Instantiate system and scene view", "Budgets (gated)", test 11, open decision 1)
**Status:** Done, except the save and validate budgets (section 5). The step 7 code is committed with this record. The stale `.ohscene` cleanup is a separate earlier commit, `de58ae9`.

## 1. Summary

Step 7 makes meshes data. `MeshSource` (a variant of a model asset and six primitives) and `MaterialOverride` are authoring components. The `instantiate` system builds `MeshRenderer` from them through the same path as `spawnModel`. Model files resolve through an `AssetResolver`, and `validate` can check that referenced assets exist.

The gated budgets were re-run on the grown bench10k. Every budget passes except save pretty and validate, which are still over their 10 ms budgets. That needs Frank's ruling (section 5).

## 2. Components (`ohao/ecs/components.hpp`)

### 2.1 Authoring components

- `MeshSource` holds one alternative: `ModelAsset`, `SpherePrim`, `CubePrim`, `PlatformPrim`, `CylinderPrim`, `ConePrim`, `QuadPrim` or `BoxPrim`.
- `QuadPrim` defaults to a unit quad in the XY plane facing +Z. `BoxPrim` defaults to a unit box around the origin. Canonical files omit default fields, so every field needs a default. The spec's step 7 note records this.
- `MaterialOverride` holds `baseColor`, `roughness`, `metallic` and `emissiveStrength`. Scalars only in v1.
- The generators `PrimitiveMeshGenerator::generateQuad` and `generateBox` (`ohao/scene/asset/primitive_mesh_generator.hpp`) are the example builders, made reusable. Test 11 checks them against the example builders.

### 2.2 Runtime components

- `InstantiateDirty` is the tag set when `MeshSource` or `MaterialOverride` changes.
- `InstantiateError {code, message}` is set when a mesh cannot be built. The entity then has no `MeshRenderer`.

## 3. Instantiate and the asset resolver

### 3.1 Trigger and work (`ohao/ecs/instantiate.hpp`)

- Constructing, updating or replacing `MeshSource` or `MaterialOverride` tags the entity `InstantiateDirty`.
- Removing `MaterialOverride` re-instantiates the entity with the default material.
- Removing `MeshSource`, or destroying the entity, removes what instantiate made and drops the entity's use of the mesh. The mesh itself is released with its last user (section 3.2).
- The system is added with `World::addSystemBefore("transform", ...)`, so a new mesh renders in the frame its `MeshSource` appears. `addSystemBefore` (`ohao/ecs/world.hpp`) is new in this step.
- The work is the same as `spawnModel`. The model comes from the resolver or a generator. Its geometry is uploaded with `render::uploadModelMesh`, and its materials come from `render::modelMaterials`, with the override applied. Then `MeshRenderer` is emplaced or replaced (keeping `visible`), with `GiMaterialLegacy` and `EmissiveProxyLight`.
- Dirty entities are processed in `CreationOrder`, so meshes register in a deterministic order.

### 3.2 Caching and release

- Meshes are cached per distinct `MeshSource` value, with a user count. When the last user goes, the mesh is released through the new `RenderScene::releaseMesh` (`ohao/render/scene/render_scene.hpp`).
- `releaseMesh` marks the mesh released (`MeshInfo::released`, so `info()` returns null) and retires its BLAS after `MAX_FRAMES_IN_FLIGHT` frames. Dead TLAS slots get reference 0.
- Mesh indices are not reused, and arena space is not reclaimed yet. This is deferred (section 8).

### 3.3 Materials

- Materials are registered once per distinct value, not per entity. This is the step 7 ruling in the spec.
- The reason is `MaterialTable::kCapacity` (4096). The table is fixed-size and never frees, so per-entity registration would exhaust it in a 10k-entity scene, and every edit would leak a slot.
- Equal values give the same `MaterialHandle`.

### 3.4 Backend seam

- `InstantiateBackend` (`ohao/ecs/instantiate.hpp`) has four methods: `uploadMesh`, `releaseMesh`, `modelMaterials` and `material`.
- `installInstantiate` has two overloads: one takes the `RenderScene` and `BindlessTextureManager`, the other takes a shared backend. Tests use a fake backend, so test 11 runs without a GPU.
- `instantiateErrors(world)` lists the entities that carry `InstantiateError`, in creation order.
- Failures are `E_ASSET_NOT_FOUND`, `E_ASSET_INVALID` (from the resolver) and `E_MESH_INVALID` (a model with no triangles, or a failed upload). Each is logged once.

### 3.5 Asset resolver (`ohao/ecs/asset_resolver.hpp`)

- `AssetResolver` has three methods: `loadModel`, which returns `Result<std::shared_ptr<const Model>, AssetError>`; `exists`, which checks without loading; and `absolutePath`, which is empty for a rejected path.
- `AssetError {code, message}` carries `E_ASSET_NOT_FOUND` (no such file, or a path outside the project) or `E_ASSET_INVALID`.
- `FileAssetResolver` takes a project root. It rejects empty, absolute and `../` paths, and caches loaded models by normalized path until `clearCache()`.
- `validateScene` and `scene.validate` take `checkAssets`, which uses `exists` and does not load. `scene.validate` returns `E_COMMAND` when the session has no resolver. `Session::setAssetResolver` supplies one.

### 3.6 Model upload split (`ohao/render/scene/model_upload.hpp`)

- `uploadModel` is now `uploadModelMesh` (geometry, one BLAS) plus `modelMaterials` (one descriptor per model material, with the legacy RT tint) plus `emissiveProxy`.
- Callers that share a mesh but not its model keep `GiModelInputs` and use `giMaterialLegacyPacked` for the RT GI word.

## 4. Other changes

- Four new error codes: `E_ASSET_NOT_FOUND`, `E_ASSET_INVALID`, `E_MESH_INVALID` and the warning `W_UNSAVEABLE_MESH`. The last is a save-report warning for an entity that has a `MeshRenderer` but no `MeshSource` (one built by `spawnModel` from an in-memory model). The entity saves without a mesh, and the warning names its ID. The catalogue now has 30 codes.
- `scene.validate` takes `checkAssets` (section 3.5). The step 5 API note that said it "arrives with assets" is now done.
- bench10k changed composition. Every renderable has a `MeshSource` primitive (all six procedural kinds, from a size palette) and a `MaterialOverride` (color and roughness palettes, metallic on one in five). The scene is 5.13 MB pretty and 3.77 MB compact. `Camera` and `Environment` are still missing until step 8.
- Two codec changes, made to bring validate and save down:
  - The variant decoder builds its error strings only on error (`ohao/reflect/decode.cpp`).
  - The pretty writer has a short-decimal fast path for 0.001 <= |v| < 16 (`ohao/reflect/json_writer.cpp`). A value takes the fast path only when `float(m)/1e6f == a` holds for `m = nearbyint(a*1e6)`, and trailing zeros are stripped. The path is tested exact against `std::to_chars` over every 4-digit decimal and over a stride sweep.

## 5. Results

Release build, single thread, this machine. Medians in ms from the final quiet run.

| Operation (bench10k) | Median | Budget | Result |
|---|---|---|---|
| `saveScene` pretty | 13.93 | 10 | OVER |
| `loadScene`, into an empty world | 18.57 | 20 | ok |
| `validateScene` | 14.22 | 10 | OVER |
| `takeSnapshot` | 0.653 | 1 | ok |
| `restoreSnapshot`, unchanged | 0.696 | 1 | ok |
| `restoreSnapshot`, 1% of rows changed | 0.480 | 2 | ok |
| `restoreSnapshot`, into an empty world | 2.389 | 10 | ok |
| `world.apply`, batch of 100 `field.set` | 0.186 | 0.2 | ok |
| `world.query`, 1,000 ids, full components | 1.223 | 2 | ok |

- bench100k ratios run from 9.6x to 11.0x against the paired bench10k medians, all within the 12x limit.
- Before the two codec changes, validate measured 19.4 ms and save 14.9 ms. The changes brought them to 14.22 and 13.93 ms. That is still over budget.
- A noisy earlier run also put `apply` at 0.210 ms and two bench100k ratios over their limits. The quiet re-run passed all of them. Treat those as machine noise. The gate is run on a quiet machine.
- Across the two full runs, validate measured 13.85 and 14.22 ms and save 13.88 and 13.93 ms.

Save and validate need Frank's ruling (spec open decision 1). The spec lists three options:
1. scale the budgets with the scene (x1.6 by size);
2. allow multi-threaded save and validate;
3. a codec rewrite: a validate that streams without the value tree, and per-type specialized writers.

Until this is ruled, `scene_bench --check` fails on these two lines only.

## 6. Tests

- `scene_tests` 49/49, including 11 new `t11_*` tests. Per the spec's test 11 "as built" note, they cover mesh sharing and release, per-value materials, restore re-instantiating, the generators against the example builders, `W_UNSAVEABLE_MESH`, and `checkAssets`. All use the fake backend.
- `api_tests` 14/14. New: `t11_validate_checks_assets_when_asked`.
- `reflect_tests` 30/30.
- `instantiate_gpu_test` 5/5 (`tests/renderer/instantiate_gpu_test.cpp`). Cornell box, deferred, 960x540. A `spawnModel` build and an instantiate build render bit-identical pixels. The shared sphere mesh is released only with its last user. Validation messages: 0 + 0.
- Render goldens 5/5 PASS, tolerances unchanged:
  - `cornell_box` and `cornell_box_deferred`: max_abs 0;
  - `damaged_helmet`: max_abs 1;
  - `damaged_helmet_deferred`: max_abs 1;
  - `cornell_box_rt_realtime`: max_abs 3.
- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200`: 0 validation messages, including teardown.
- `ecs_tests` 26/26; `engine_tests` 58/58; `runtime_tests` 21/21; `force_generator_tests` 54/54; `physics_backend_tests` all pass; `renderer_test` ALL PASS; `check_scene_schema.py` 14/14.

## 7. Spec adjustments made in this step

- "Components: authoring vs runtime": `QuadPrim` and `BoxPrim` defaults, with the step 7 note.
- "Instantiate system and scene view": `AssetResolver` returns `Result`, the `InstantiateBackend` seam, `releaseMesh`, the per-value material rule.
- "Budgets (gated)": the step 7 note with the bench10k composition, the codec changes and the two over-budget lines.
- Test plan, test 11: the "as built" note and the GPU half.
- Build order: step 7 marked done, except the save and validate budgets.
- Open decisions: decision 1 (save and validate budgets) is recorded.

## 8. Known gaps and deferred

- Save pretty (13.93 ms) and validate (14.22 ms) are over their 10 ms budgets. This needs Frank's ruling (section 5).
- `releaseMesh` does not reuse mesh indices or reclaim arena space yet.
- The material table never frees, so it holds at most 4096 distinct values (`MaterialTable::kCapacity`).
- The RT shared-BLAS risk is decided in step 8, when the examples convert.
- `Camera` and `Environment` are not in bench10k yet. The gate is re-run on the full scene in step 8.

Next: step 8 (`Camera`, `Environment`, scene view; `cornell_box` and `model_viewer` converted with `--save-scene`, existing goldens must still pass).
