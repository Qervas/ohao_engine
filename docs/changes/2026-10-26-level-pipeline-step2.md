# Level pipeline, step 2: structure and per-mesh geometry

**Spec:** `docs/specs/2026-10-26-level-pipeline-design.md` ("glTF → world mapping": "Mesh
nodes", "Where the data comes from"; "Why" (normals); "Build order" step 2; tests 2 and 3);
plan `docs/plans/2026-10-26-level-pipeline.md`, Task 2.

**Status:** built and tested; the benchmark budgets are unjudged (every run noisy, section 5).

## 1. Summary

**What step 2 delivers:**
- A glTF structure reader, `ohao/level` (`ohao_level`): a `.glb` or `.gltf` file's node
  tree, TRS (a `matrix` decomposed), `extras`, `KHR_lights_punctual` lights, cameras and
  per-mesh primitive and triangle counts, read from the file's own JSON.
- One glTF mesh out of a file: `ModelLoader::loadMesh(path, mesh)`, by name or `#<index>`,
  at its own origin, with only its own materials.
- `ModelAsset.mesh`: a scene can place one mesh of a file. Instantiate caches one GPU mesh
  per `(path, mesh)`. An empty `mesh` is the whole-file merge, as before, and is neither
  written nor hashed, so every existing scene file and `world.hash` is unchanged.
- Authored normals are kept. The loader's normal repair fills only vertices a file gives
  no normal; before, it replaced every model's normals with smoothed ones. Eleven golden
  images moved and were re-baselined after looking.

**What step 2 does not deliver:** nothing reads a level into the world yet. The `Level`
component, the `level` sync system, the hint mapping, spawns and sockets are step 3.

## 2. What was built

**`ohao/level/gltf_structure.{hpp,cpp}`** (69 + 347 lines; static library `ohao_level`,
linked by `ohao_scene`):
- `readGltfStructure(std::span<const uint8_t>) -> Result<GltfStructure, GltfError>`:
  - the GLB header and chunk 0, or the whole `.gltf`; simdjson's DOM;
  - `asset.version` must be 2.x;
  - the default scene (`scene`, else 0), its nodes depth first, parents before children;
  - TRS, or a `matrix` decomposed. A perspective row, a zero scale or a shear is
    `E_LEVEL_NODE` naming the node's path; a mirroring matrix becomes a negative X scale;
  - `extras` (per node and the scene's) as compact JSON text, member order and number
    kinds kept;
  - `KHR_lights_punctual` (point, spot, directional; colour, intensity, range with 0 for
    infinite, cone angles) and perspective cameras (`yfov`, `znear`, `zfar` with 0 for
    infinite);
  - per mesh: its primitive count and triangle count from the accessors (triangles
    `n/3`, strips and fans `n-2`, points and lines 0);
  - Draco and meshopt compression are `E_LEVEL_UNSUPPORTED`; skins, animations, morph
    targets, orthographic cameras and `KHR_materials_*` are listed in `unsupported`;
  - malformed files (not JSON, glTF 1, an index out of range, a node cycle or a node with
    two parents) are `E_ASSET_INVALID`.
- `nodePath(structure, node)`: `/Room/WallNorth`, `#<index>` for an unnamed node.

**`ModelLoader::loadMesh(path, mesh) -> Result<std::shared_ptr<Model>>`:**
- glTF only (`.glb`, `.gltf`); `mesh` is a name (an ambiguous name is an error) or
  `#<index>`.
- Reads the structure for the primitive counts, maps the mesh to its range of Assimp
  meshes (Assimp's glTF2 importer makes one per primitive, in file order), and emits that
  range untransformed.
- Drops two of the whole-file path's Assimp steps, which would shift the range:
  `SortByPType` (splits mixed primitive types) and `FindDegenerates` (turns degenerate
  triangles into points and lines it splits off). It refuses a file whose Assimp mesh
  count is not its primitive count.
- Builds only the materials the range uses, renumbered from 0.

**`ModelAsset.mesh`** (`ecs/components.hpp`, `scene_types.cpp`, `scene_writers.cpp`):
- reflected with a doc string and the new `omitDefault` attribute;
- the per-type writer writes it only when non-empty and `samePrim` compares it;
- `AssetResolver::loadModel(ref, mesh)` caches on `file` or `file + "\n" + mesh`;
  instantiate's mesh key and debug name carry the mesh.

**Reflection: `omitDefault`** (`reflect/type_info.hpp`, `registry.{hpp,cpp}`, `schema.cpp`):
a field so marked is skipped by the generic writer and by the world hash while it equals
its default, and the catalogue says `"omitDefault": true`. Without it, adding a field to a
variant alternative changes every saved file and every `world.hash`.

**Normals** (`model_loader.cpp`, `model.cpp`):
- `repairMeshNormals` returns at once when every vertex has a normal, and otherwise writes
  only the vertices without one (zero or not finite), from the smooth normal of the faces
  around their position.
- The Assimp emitter and the OBJ loader write zero for a missing normal, not `(0,1,0)`, so
  the repair can find it.

**Fixtures** (`tests/level/fixtures/`, LF in `.gitattributes`): `nodes.gltf`,
`matrix.gltf`, `lights_cameras.gltf`, `scenes.gltf`, `errors_shear.gltf`,
`errors_draco.gltf`, `errors_bad_index.gltf`, and `no_normals.obj` (`.gitignore` ignores
`*.obj` for MSVC's object files; `!tests/level/fixtures/*.obj` lets the fixture in).

**Tests** (`scene_tests`, 14 new, 80 in all):
- `sp5_t02_structure_reads_nodes_trs_and_hierarchy`, `_extras_kept_verbatim`,
  `_matrix_nodes_decompose`, `_sheared_matrix_is_an_error`, `_khr_lights_and_cameras`,
  `_draco_is_refused`, `_malformed_files_are_invalid`, `_default_scene_is_used`,
  `_blockout_structure` (17 nodes, 13 meshes, step 1's hints);
- `sp5_t03_whole_file_unchanged` (merged models' positions, colours, UVs, tangents,
  indices and materials hash as at `e47bccb`, normals aside), `_load_mesh_matches_accessors`
  (every blockout mesh, plus the pillar at its own origin and the errors),
  `_shared_mesh_uploads_once`, `_model_asset_mesh_round_trips`,
  `_authored_hard_edges_kept` (every blockout vertex normal within 1e-4 of its face's,
  whole file and per mesh; an OBJ without normals still gets them).

**Generated outputs updated:** the engine catalogue and scene schema goldens
(`tests/scene/golden/`), and the schema hash (`99055f4c1dbc9ac8` → `37302179076ab632`) and
the memory one `world.snapshot` reports (3,920 → 4,088 bytes: `MeshSource` is larger in
memory with the new string) in the CLI and serve expectations, and the two snapshot sizes the
CLI and serve guides show (4,160 → 4,352 and 4,000 → 4,288 bytes). Nothing else in them
changed; the guides' `world.hash` values are the same.

**Docs:** AGENTS.md (the levels gotcha: `ModelAsset.mesh`, `ohao/level`, normals kept); the
monograph's `scene/loaders` (the load path and the normal repair) and `path-tracer/hit-miss`
(the loaders no longer write `(0,1,0)`); spec and plan as-built notes.

## 3. Bugs found and fixed

- **Every model's normals were replaced** (found in step 1): `repairMeshNormals` merged
  vertices by position and wrote back area-weighted normals on every load, so hard edges
  rendered rounded. Fixed as above; `sp5_t03_authored_hard_edges_kept` failed before the
  fix (the pillar's worst vertex normal 84.9° from its face's, the whole blockout's
  169.4°) and passes after (1.96e-5).
- **The loaders hid missing normals** behind `(0,1,0)`, which the repair then had to treat
  as real. They write zero now.

## 4. Spec corrections and rulings

- **Dependencies:** the plan had `ohao_level` link `ohao_reflect` and `ohao_scene` and parse
  with reflect's JSON reader. `ohao_scene` needs `ohao_level` (`loadMesh` reads the
  primitive counts), so that would be a cycle, and the scene JSON reader lives in
  `ohao_ecs`, above both. `ohao_level` links `ohao_core`, glm and simdjson only.
- **`Result`, not `std::expected`:** the tree is C++20; `core/result.hpp` is the engine's
  equivalent.
- **Hint errors move to step 3:** test 2's "a bad `col`, an unknown component" are judged by
  the hint mapping (the plan already has them in Task 3); the reader keeps `extras` as text.
- **`omitDefault`** was not in the plan; it is what lets `ModelAsset.mesh` be "written only
  when non-empty" through the generic writer and the hash as well as the per-type writer.
- **Test names** carry the sub-project, `sp5_t02_*`/`sp5_t03_*`, since `scene_tests`
  already has `t02_*`/`t03_*` from sub-project 3.

## 5. Results

| Gate | Result |
|---|---|
| `scene_tests` | 80/80 |
| render goldens | 25/25 after the re-baseline: deferred max_abs 0, path-traced within the 1-LSB floor |
| `tools/check.py --all` | 41/44: every test but the three benchmarks passed (`engine_tests` 85/85, `ecs_tests` 26/26, `scene_tests` 80/80, `render_golden` 25/25, `check_docs` 73/73 under CTest and 106/106 run alone); `scene_bench`, `serve_bench` and `render_bench` were `noisy` on every run and rerun (below) |

**The benchmarks have no verdict.** All three were `noisy` on two full gates and on each of
check.py's two reruns. The machine was never quiet: `wait_quiet.py --deadline 3600` gave up
after an hour with other processes at 1.3–4.6 cores (Defender, other `pwsh` sessions,
`dwm.exe` at ~125%, the Unreal editor, Edge WebView). `scene_bench`'s own calibration loop,
which this step does not touch, ran 2.95× its reference (68.4 ms against 23.2 ms), and
`render_bench`'s CPU calibration 2.70×; every scene op was 2–3× its budget in the same
proportion. Dividing the clean rounds' medians by the calibration's 2.95 puts each bench10k
op inside its budget (save_pretty ≈ 7.6 ms of 10, load ≈ 11.9 of 20, validate ≈ 8.2 of 10,
query_1000_ids ≈ 1.1 of 2), but that is an estimate, not a pass: the budgets need a run on a
quiet machine. The code this step adds to those paths is one flag test per field in the
generic writer and the world hash (`omitDefault`).

**Goldens before the normals fix:** 25/25 unchanged (per-mesh loading and `ModelAsset.mesh`
move nothing). **After it**, 15 manifest scenes over 11 golden images moved, each looked at
(before, after and heatmap in the look-shot folder) and re-baselined in commit `a2b25e0`:

| Golden image (scenes) | max_abs | changed fraction |
|---|---|---|
| `damaged_helmet` (`damaged_helmet`, `cli_damaged_helmet`; path traced) | 179 | 0.681354 |
| `damaged_helmet_deferred` (+ `cli_`) | 216 | 0.181168 |
| `helmet_game_deferred` (`cli_helmet_game_deferred`) | 216 | 0.183030 |
| `helmet_game_reference` (`cli_helmet_game_reference`) | 214 | 0.183650 |
| `khronos_metal_rough_spheres_deferred` (+ `cli_`) | 31 | 0.097474 |
| `khronos_metal_rough_spheres_reference` (`cli_`) | 27 | 0.085903 |
| `khronos_flight_helmet_deferred` (+ `cli_`) | 33 | 0.073268 |
| `khronos_flight_helmet_reference` (`cli_`) | 32 | 0.067122 |
| `khronos_sponza_deferred` (`cli_`) | 201 | 0.638568 |
| `khronos_sponza_reference` (`cli_`) | 199 | 0.629045 |
| `blockout_room_deferred` (`cli_blockout_room_deferred`) | 110 | 0.766931 |

- The helmet changes most along its panel seams, the visor and the vents, where its
  authored creases replace smoothed shading. The flight helmet changes in a speckle over
  the whole model, strongest at the goggle lenses and the base's edges. The spheres are smooth either way: their change is a
  scattered speckle over every sphere and the label strips (max 31), the authored normals
  against the re-averaged ones. Sponza's arches, columns and walls change, its floor does
  not. The blockout's boxes have hard edges. The path-traced helmet's large changed fraction is its noise re-drawn wherever a
  normal moved.
- Unchanged: the Cornell box family (primitives, which carry their own normals) and the
  two area-light scenes.

**Look shots** in `C:\Users\djmax\ohao_refs\look_shots\sp5_step2\`:
- `blockout_before_after_authored_normals_side_by_side.png`: the blockout golden before
  (rounded pillar, crates, ramp) and after (hard edges).
- For each of the 11 images: `golden_<name>_before.png`,
  `golden_<name>_after_authored_normals.png` and `golden_<name>_diff_heatmap.png`.

## 6. Known gaps

- The benchmark budgets were not judged (section 5): rerun
  `python tools/bench/wait_quiet.py` then `python tools/check.py --labels bench` on a quiet
  machine.
- Nothing expands a level into entities yet (step 3); `extras` are read but not mapped.
- `loadMesh` is glTF only; FBX and OBJ load whole files.
- A glTF mesh used by two nodes is still emitted twice by the whole-file merge (unchanged);
  per-mesh loading shares it.
- The `UCX_` hulls of staged props are still drawn by the whole-file path (step 3 hides
  them with `vis: 0`).

## 7. How to run

```
python tools/bench/wait_quiet.py
python tools/check.py --labels bench
build/Release/scene_tests.exe
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_blockout_room_deferred
python tests/golden/render_golden.py tests/golden/manifest.json
```
