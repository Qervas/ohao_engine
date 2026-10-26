# Level pipeline, step 3: levels in the world

**Spec:** `docs/specs/2026-10-26-level-pipeline-design.md` ("glTF → world mapping", "Extras: the
hint schema", "Spawn points", "Determinism"; "Build order" step 3; tests 4, 5 and 9); plan
`docs/plans/2026-10-26-level-pipeline.md`, Task 3.

**Status:** built and tested; the three benchmarks were noisy and wait for a quiet-machine
rerun (section 5, Frank's ruling of 2026-10-26).

## 1. Summary

**What step 3 delivers:**
- A `Level` component: one line in a scene file places a glTF level, and the `level` sync
  system expands it into one entity per node of the file's default scene, with ids that
  are the same in every process and on every load, and that are never saved.
- Nodes become what their file and `extras` say: meshes (one GPU mesh per glTF mesh, through
  step 2's per-mesh loading), `KHR_lights_punctual` lights, cameras, spawn points, sockets,
  and any saved component by name. A `vis: 0` node or a `UCX_` hull gets no mesh.
- Strict hints: an unknown lower-case key warns, an unknown capitalised key or a bad value is
  an error, every message naming the node's path and the key; the rest of the level loads.
- The blockout scene now uses `Level`, and its golden is unchanged (max_abs 0).
- Frank's staged Flat42 props, placed as levels, render without their collision hulls.

**What step 3 does not deliver:** colliders and bodies (step 4: `col`, `body`, `mass`,
`fric` and `rest` are parsed and checked, not applied), `level.info`/`level.reload` and
`ohao level` (step 5), the spawns' grounding check (test 9's ray, step 4).

## 2. What was built

**Components** (`ecs/components.hpp`, registered in `scene_types.cpp`, with per-type writers):
- `Level{asset, lightScale = 1, collision = true}` (saved);
- `SpawnPoint{tag}` (saved, tag ≤ 32 characters) and `Socket{name}` (saved, ≤ 64);
- `LevelNode{level, index}` (runtime, never saved) and the `LevelDirty` tag;
- the `ColliderShape` and `Motion` enums the hints use (step 4's components register them).

**`ohao/level/level_hints.{hpp,cpp}`:** `parseHints(structure, node, types, diagnostics)`:
- the keys `col`, `body`, `mass` (> 0), `fric`, `rest` (0–1), `vis` (0 or 1), `spawn` (1–32
  characters), `socket` (1–64), and the default scene's `col` and `body` for nodes without them;
- component passthrough: a capitalised key naming a saved component is decoded by the scene
  reader (`reflect::readStruct`), its own diagnostics kept (`E_RANGE`, …) and prefixed with the
  node and key; runtime components, `Name`/`Transform`/`Parent` (the node's own) and `Level`
  are refused, and entity references too;
- `UCX_<target>` mesh nodes read as `{"col":"convex","vis":0}` and `SOCKET_<name>` empties as
  `{"socket":"<name>"}`, explicit extras winning;
- `extrasJsonSchema()`: the hint keys as JSON Schema, which `ohao schema level-extras` writes
  and which is now checked in as `tests/level/extras_schema.json` (same content as step 1's
  hand-written file, laid out by the writer).

**`ohao/level/level_system.{hpp,cpp}`:**
- `installLevels(world, assets, types)` adds the `level` sync system before `instantiate`;
  `levelNodeId(levelId, index)` (splitmix64 of the pair, never 0); `levelDiagnostics(world, e)`.
- Expansion, depth first: `Parent` (the level entity for roots), `Name`, `Transform` (TRS),
  `LevelNode`; `MeshSource{ModelAsset{asset, "#<mesh index>"}}` unless the node is collision
  only; `Light` from KHR (intensity × `lightScale`, the file's range or OHAO's 10 m, cone
  half-angles in degrees); `Camera` from a perspective camera (yfov in degrees, far 200 m when
  infinite), active only when no other camera is in the world; `SpawnPoint`, `Socket`, then
  passed-through components.
- Re-expansion in place on a changed `Level`: the same entities, given back the file's name,
  transform, parent and saved components (others removed); destroyed nodes recreated, nodes
  no longer in the file destroyed. Removing a `Level` destroys its nodes.
- Problems: the file's error, or `E_LEVEL_EXTRAS` with the count and the first message, on
  the level entity's `InstantiateError`; each diagnostic logged (`asset` channel).
- A new library, `ohao_level_world` (links `ohao_ecs`); `ohao_level` keeps only the structure
  reader. `ohao_runtime` links it; the CLI context and `LiveHost` install it.

**Elsewhere:**
- `scene_io`: saving skips `LevelNode` entities (both writers); `--check-assets` checks a
  `Level`'s file (both decoders).
- `World::runSync(name)`; `scene.load` runs `level` at once, so a headless query sees the nodes.
- Reflection: `maxLength` for string fields (code points, `E_RANGE`, both decoders, the
  schemas' and catalogue's `maxLength`).
- Error codes `E_LEVEL_NODE`, `E_LEVEL_UNSUPPORTED`, `E_LEVEL_EXTRAS` (exit class 2),
  `W_LEVEL_EXTRAS`, `W_LEVEL_UNSUPPORTED` in the catalogue.
- `assets/scenes/blockout_room.scene.json`: the level is a `Level`; `check_level_assets.py`
  checks that, and that nothing loads the file as a merged mesh.
- Docs: AGENTS.md (levels gotcha), the CLI guide (components, `ohao schema level-extras`, error
  codes), the serve guide (the fly example's hash), the monograph's module map, spec and plan
  as built.

**Tests:**
- `ecs_tests` (11 new, 37 in all): `sp5_t04_one_entity_per_node`, `_hierarchy_matches_file`,
  `_ids_stable_across_processes` (a child `ecs_tests --level-ids` prints the same ids),
  `_save_writes_only_the_level` (and reloading the save expands once, to the same ids and
  bytes), `_reload_keeps_world_hash` (unchanged file; a moved node and an added component are
  put back to the same hash), `_collision_only_nodes_have_no_renderer` (only the drawn meshes
  `#0 #1 #3` are loaded and uploaded), `_passthrough_equals_scene_json`,
  `_ucx_and_socket_conventions`, `_khr_light_becomes_light`,
  `_level_camera_active_only_without_another`, `sp5_t09_spawns_in_node_order_with_facing`
  (the fixture and the blockout's three spawns, positions and facing).
- `scene_tests` (4 new, 84 in all): `sp5_t04_hints_read_keys_defaults_and_conventions`,
  `sp5_t02_extras_errors_name_the_node` (9 diagnostics over `level_errors.gltf`: 7 errors, 2
  warnings, each naming the node path and key; the level still loads, with `E_LEVEL_EXTRAS`
  on its entity), `sp5_t04_string_limits_are_checked`,
  `sp5_t04_extras_schema_is_the_checked_in_file`.
- Fixtures: `tests/level/fixtures/level_small.gltf` (12 nodes: meshes, a point and a spot
  light, a camera, two spawns, `UCX_` and `SOCKET_` nodes, a passed-through `Spin`) and
  `level_errors.gltf`.

**Goldens regenerated** (looked at; only what the step adds): the engine catalogue and scene
schema (the new components, `maxLength`, the error codes), reflect's fixture catalogue (the
error codes), the CLI and serve expectations (the schema hash `37302179076ab632` →
`8ee429bddf1493f4`, and the serve session's `world.hash`, since the hash walks every saved
component type and three were added).

## 3. Bugs found and fixed

None in existing code. While building:
- `std::vector<entt::entity>(n, entt::null)` picks the allocator constructor (`entt::null`
  converts to anything); written `entt::entity{entt::null}`.

## 4. Spec corrections and rulings

- **Where the system lives:** the plan put registration in `world.cpp`; the system needs the
  host's asset resolver and reflection, and `ohao_ecs` cannot link a library above it, so it
  is `installLevels` in `ohao_level_world`, placed before `instantiate` either way round.
- **Expansion at load:** `scene.load` expands levels at once (`runSync`), not at the next
  step, so an agent's first query sees the level.
- **Reload in place**, not destroy and recreate: recreating would give new CreationOrder
  values and move `world.hash`, which test 4 forbids.
- **Mesh names:** nodes use `#<mesh index>`, since glTF mesh names may repeat or be empty.
- **`maxLength`** added to reflection for the plan's string limits.
- **Test names** carry `sp5_` (both suites already have `t02_`/`t04_` tests).

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | 41/44: every functional suite passed; `scene_bench`, `serve_bench`, `render_bench` noisy on the run and both reruns |
| `ecs_tests` | 37/37 (11 new) |
| `scene_tests` | 84/84 (4 new) |
| `reflect_tests` | 42/42 |
| `engine_tests` | 85/85 |
| `api_tests` / `rpc_tests` | 44/44 / 18/18 |
| render goldens | 25/25, every one unchanged |
| `check_docs` | 73/73 under CTest, 106/106 run alone |

**Benchmarks: noisy, pending a quiet rerun.** Other processes used 1.0–2.5 cores while they
ran (Grok Bot, Edge WebView, a python process at 92%, the terminal). `scene_bench`'s clean
rounds (44 of 50) were inside every budget (bench10k save_pretty 9.56 ms of 10, load 13.67
of 20, validate 8.91 of 10, snapshot 0.545 of 1, restore_unchanged 0.644 of 1,
apply_100_field_set 0.182 of 0.2, query_1000_ids 1.29 of 2); the two budget lines it reported
(save_pretty 10.07 ms, apply_100_field_set 0.207 ms) are the medians over all rounds,
contended ones included. `serve_bench`'s and `render_bench`'s calibrations ran 1.04–1.17x and
1.09x (CPU), 1.08x (GPU) their references beside that load. No budget was changed.

**Test 5:** `cli_blockout_room_deferred` through `Level`: max_abs 0, changed fraction 0. The
per-node meshes, materials and draws render exactly what the merged file did. No re-baseline.

**The staged Flat42 props** (`assets/local/blockout_flat42_{close,wide}_level.scene.json`,
local and git-ignored, the props placed as `Level`s): the four props load only their visual
meshes (the table 1,838 vertices against 1,862 merged, the chair 4,260 against 4,308, the
books 1,364 against 1,388, the kettle 3,400 against 3,424) and render textured; the white
hull boxes and their shadows are gone. Deferred, 1920×1080, 30 frames: 2.96 ms GPU (close)
and 2.93 ms (wide) a frame, against 3.18 and 2.52 ms through the merged files.

**Look shots** in `C:\Users\djmax\ohao_refs\look_shots\sp5_step3\`:
- `flat42_close_before_after_side_by_side.png`, `flat42_wide_before_after_side_by_side.png`;
- `flat42_{close,wide}_before_merged_files_hulls_drawn.png` and
  `flat42_{close,wide}_after_level_hulls_hidden.png`;
- `blockout_via_level_golden_exact.png`.

## 6. Known gaps

- **Loading cost:** `loadMesh` re-imports the whole file through Assimp for each mesh (the
  blockout: 13 imports for 13 meshes). Step 6's `kit2k` budget will measure it; one import
  per file is the obvious fix.
- **Structure cache:** a level file edited on disk is not re-read until step 5's
  `level.reload` clears the cache.
- **Snapshots:** `world.restore` of a snapshot taken before a node was destroyed recreates it
  as a plain entity (its `LevelNode` is runtime and not in the snapshot), which then saves.
- **An entity parented under a level node** saves with a `Parent` that names an unsaved
  node, and fails to load (`E_PARENT_MISSING`).
- **Collision hints** are parsed, not applied (step 4). Spawns are not yet checked to stand
  on ground (test 9's ray, step 4).
- **Benchmarks** need a rerun on a quiet machine (`python tools/check.py --labels bench`),
  for this step and step 2.

## 7. How to run

```
build/Release/ecs_tests.exe
build/Release/scene_tests.exe
build/Release/ohao schema level-extras -o build/level_extras.schema.json
build/Release/ohao query assets/scenes/blockout_room.scene.json --has SpawnPoint --fields SpawnPoint/tag
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_blockout_room_deferred
python tests/level/check_level_assets.py
```
