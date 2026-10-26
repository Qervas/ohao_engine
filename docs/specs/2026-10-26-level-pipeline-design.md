# Sub-project 5 — Level pipeline: Blender levels that load, render and collide

Status: written 2026-10-26 on Frank's call ("go on SP5, and we may use existing 3D models
from Somnabulate"). Frank delegated the technical choices; this document makes them. It is
the binding spec once Frank accepts it. What only Frank can do is listed under "Frank
actions"; the questions that need him are under "Open decisions".

Roadmap: row 5 of `docs/ROADMAP.md`. Before it: 2b (renderer upgrade, built 2026-10-25)
and the GT polish that followed it. After it: 6 (player: Jolt at a fixed step in the ECS,
physics ↔ transform sync, the first-person controller).

Builds on:
- **Sub-project 3** (`2026-10-22-reflection-scene-serialization-design.md`): reflection,
  the scene codec, `.scene.json`, the `ohao` CLI, `MeshSource`/`ModelAsset`, instantiate
  and its mesh cache.
- **Sub-project 4** (`2026-10-23-agent-control-surface-design.md`): `ohao serve`, MCP, the
  live host, `tools/check.py`, the golden and bench machinery.
- **2b** (`2026-10-24-renderer-upgrade-design.md`): the Assimp loader that applies glTF
  node transforms (its "glTF node transforms" section hands `extras` → components to this
  sub-project), and the GPU-driven `RenderScene`, where one mesh drawn by many instances
  costs one upload and one BLAS.

## Thesis: a level is data an agent can read

Levels are authored in Blender and exported as glTF 2.0; node `extras` map to components
(the roadmap's foundation decision of 2026-10-02). This sub-project makes that true and
cheap for agents:
- **One line references a level.** The scene file holds `Level{asset}`, not thousands of
  copied entities. The `.glb` stays the source of truth, so a Blender re-export is the
  whole edit loop.
- **The level's world is queryable.** Every glTF node becomes an entity with its name,
  transform and components. Agents find spawn points with the existing `query`, ask
  `level.info` for a summary of a few hundred bytes, and ask `physics.raycast` whether
  something collides.
- **Hints are terse.** A node says `{"col":"box"}` or `{"spawn":"player"}`. No fat JSON
  in the level and none in the scene.

## Goal

A level exported from Blender loads into the world as one entity per node, renders through
the GPU-driven `RenderScene`, and builds Jolt collision from its hints, so queries and
bodies collide with it.

**Done when:**
- `assets/scenes/blockout_room.scene.json`, a single `Level` entity over
  `assets/levels/blockout_room.glb`, loads into one entity per glTF node:
  - names, transforms and hierarchy are the file's;
  - spawn points are `SpawnPoint` components;
  - colliders are built from `extras`.
- It renders: an exact deferred golden.
- It collides, tested:
  - a ray down from each spawn hits the floor or step at its authored height (±1 mm);
  - a ray through the doorway passes and one beside it hits the wall;
  - the two dynamic crates, stepped in the test at 60 Hz for 2 s, come to rest on the floor
    and on each other.
- Agents reach it over MCP: `level.info`, `level.reload`, `physics.raycast`, `physics.info`,
  and spawn points through `query --has SpawnPoint`.
- Frank's Flat42 props (Somnabulate) load as a prop kit:
  - textured;
  - their `UCX_` hulls as convex colliders, not drawn;
  - their sockets as `Socket` components.
- The budgets under "Performance budgets" pass five consecutive clean runs, and every
  existing gate stays green.

## Agents as users, in 5

- **Authoring a level is two edits:** export from Blender to `assets/levels/<name>.glb`,
  and add one entity to a scene: `{"Name":{"value":"Level"},"Level":{"asset":{"path":
  "assets/levels/<name>.glb"}}}`.
- **Reading a level costs little.** `level.info` returns counts, spawns and warnings, not
  nodes. `ohao level <glb>` returns the same without a GPU or a scene, so an agent can
  inspect a file before using it.
- **Errors name the node.** Every level diagnostic carries the node's path in the file
  (`/Room/WallNorth`) and the `extras` key, so a fix is one Blender property away.
- **The loop is short.** After a Blender re-export, `level.reload` re-reads the file in a
  live session. Agents on the CLI rerun `ohao run`.

## Scope: in 5 vs later

| In 5 | Later (and where) |
|---|---|
| glTF 2.0 / GLB from Blender's exporter: the default scene's node tree, TRS and matrix transforms, mesh nodes, empties | FBX or USD levels (FBX props go through staging, below; USD is sub-project 8) |
| Node `extras` → components: the hint keys and component passthrough below | Per-node overrides authored in OHAO and saved over the `.glb` (a later editor concern) |
| One render mesh per glTF mesh, shared by every node that uses it (instancing through `RenderScene`) | `EXT_mesh_gpu_instancing`, LODs (Flat42's `_LOD1`), streaming and sub-levels |
| `KHR_lights_punctual` → `Light`, glTF cameras → `Camera` | Area lights through glTF (none in the extension; `extras` passthrough covers them now) |
| Collision: box, sphere, capsule, convex, mesh or none per node, static/dynamic/kinematic declared, Unreal `UCX_` hulls | Stepping dynamic bodies in the world and transform sync (6); triggers and their events (6/7); navmesh |
| `SpawnPoint` and `Socket` components; spawn queries | Using sockets (attachments) and spawning actors at spawns (6, 7) |
| `Level` component, `level.info`, `level.reload`, `physics.raycast`, `physics.info`, `ohao level` | Hot reload by file watching (a live host may poll later; `level.reload` is the primitive) |
| Somnabulate Flat42 props staged as GLB (local), a checked-in generated blockout | An asset database, GUIDs and cooked formats (roadmap "Next") |
| Skins, animations, morph targets, `KHR_materials_*`: ignored with one warning per kind | Their own sub-projects when the slice needs them |
| Draco and meshopt compression: refused with an error | Supported only if a real asset needs it |

## Why (what the current code does)

Verified against the tree at `a19d437`:
- **The glTF path merges a file into one mesh.** `ModelLoader` (Assimp,
  `ohao/scene/asset/model_loader.cpp:276-499`) walks the node tree depth first and bakes
  each node's transform into one vertex and index array (`ohao::Model`). Node names,
  `extras`, cameras and `KHR_lights_punctual` are never read. A mesh used by two nodes is
  emitted twice.
- **A scene references a whole file:** `MeshSource{ModelAsset{asset}}`. Instantiate caches
  one GPU mesh per path (`ecs/instantiate.cpp`).
- **Jolt is linked but not in the world.**
  - `ohao/physics` has a backend interface (`physics/backend/physics_backend.hpp`) with
    box, sphere, capsule, convex hull, triangle mesh and heightfield shapes, ray, sphere and
    box casts, and overlaps. Its Jolt implementation builds `MeshShape` and
    `ConvexHullShape` (`jolt_backend.cpp:1472-1574`).
  - Nothing in `ohao/ecs`, `ohao/api` or the hosts creates a body. The `PhysicsWorld`
    wrapper turns triangle meshes into boxes ("For now, fall back to box",
    `physics_world.cpp:582-588`).
- **There is no spawn point, collider or level component,** and no CLI command that reads
  a level.
- **Every loaded model's normals are replaced by smoothed ones.**
  - `repairMeshNormals` (`model_loader.cpp:56-134`) runs after every load, not only on the
    "triangle soup" it was written for.
  - It merges vertices by position and writes back area-weighted normals, so a file's
    authored hard edges are lost. The blockout's boxes render rounded (step 1: 306 vertices
    → 83 positions, 263 normals changed).
  - Step 2 keeps the file's normals whenever the file has them and repairs only meshes
    without normals. That moves goldens whose models have authored hard edges, which are
    re-baselined after looking, in their own commit.
  - *As built (step 2):* the repair now fills only vertices without a normal (zero or not
    finite; the loaders write zero there instead of `(0,1,0)`) and leaves every other
    vertex as the file has it. The blockout's `Pillar` keeps its face normals (worst
    vertex normal within 1e-4 of its face's, where the smoothed one was up to 169.4° off). 11 golden images (15 manifest
    scenes) moved: the helmet, the flight helmet, the metal-rough spheres, Sponza and the
    blockout; Cornell and the area-light scenes did not.

## Assets and staging

Two sources, kept apart by license:

1. **The checked-in blockout is generated, so it is ours (MIT, as the repo).**
   - `tools/level/blockout_room.py`, run by Blender headless, writes
     `assets/levels/blockout_room.glb` (16.8 KB).
   - The room: an 8 × 6 m floor slab, four 3 m walls, a doorway with a lintel, a 0.3 m
     step, a 0.6 m platform, a ramp, a pillar, two crates, three spawn empties and a
     camera.
   - Every hint is a custom property on the Blender object, exported as `extras`.
   - Blender 5.2.2 writes the same bytes on every run (checked at step 1). The script is
     the file's provenance and its rebuild recipe.
2. **Somnabulate assets are Frank's** (his game, read and copy only; nothing under
   `C:\Users\djmax\Desktop\games` or `D:\Unreal Projects\somnabulate` is ever written).
   - **Olivier's Flat42 props** (`dream_01_held_second/flat42_o33/Assets/Olivier/Props/Flat42/`):
     - 78 LOD0 `SM_<Prop>.fbx` files (plus `_LOD1`) with `T_<Prop>_BC/N/ORM.png`
       textures, a `.blend` and a `_sockets.json` each;
     - they follow Unreal's conventions: Z up, metres, `UCX_` convex collision meshes,
       `SOCKET_` empties, DirectX normal maps;
     - only the `flat42_o33` folder exists, with no sibling `flat42_o3x`.
   - `tools/level/stage_flat42.py` (Blender headless) converts chosen props to GLB:
     - the material is rebuilt from the three textures (ORM packed as glTF reads it);
     - the normal map's green channel is flipped to OpenGL's convention;
     - `UCX_` hulls are kept with `{"col":"convex","vis":0}`;
     - sockets are kept as `{"socket":…}`.
   - **Ready GLBs** in `games/generated/` (bedside clock, dining chair, desk set,
     kitchen): 2–20 MB each with baked 4K textures, so they are staged on demand only.
   - **The blockout folders** (`.delilah-preview/d1_blockout`,
     `bench04/pass11/flat_blockout`) hold reference renders (JPG/PNG), no geometry: layout
     references for later levels, nothing to import.

**Staging rule:**
- **Location:** staged Somnabulate GLBs go to `assets/local/` (git-ignored), with
  `assets/local/README.md`. The asset resolver reads only project-relative paths, so a
  staging tree outside the repo could not be loaded.
- **Scenes:** a scene that uses them lives in `assets/local/` too. Checked-in scenes,
  goldens and `ohao doctor` keep the rule that they load only tracked assets.
- **Tests:** a test that needs staged props reports `skip` when they are missing. It never
  passes silently.
- **Checking them in** waits for Frank's ruling (Open decision 1): the repository is MIT
  and has a GitHub origin, so committed art would be published under MIT by the next push.

## glTF → world mapping

**The `Level` component** (saved): `Level{asset: AssetRef, lightScale: float = 1,
collision: bool = true}`.

**The `level` sync system** runs before `instantiate`. On a new or changed `Level` it reads
the file's structure (cached per path) and creates one entity per node of the default
scene, depth first in file order:
- `Parent`: the level entity, or the node's parent node's entity.
- `Name`: the node name.
- `Transform`: the node's TRS. A `matrix` is decomposed; a sheared matrix is
  `E_LEVEL_NODE`.
- `LevelNode{level, index}`: runtime only, not saved.
- `EntityId`: a hash of the level entity's id and the node index. The same file gives the
  same ids on every load, so `world.hash`, recordings and an agent's references survive
  `level.reload`.

**Mesh nodes** get `MeshSource{ModelAsset{asset, mesh}}`:
- `ModelAsset` gains a `mesh` field: a glTF mesh name, or `#<index>`.
- Empty `mesh` (the default) keeps today's whole-file merge, so every existing scene is
  unchanged.
- Instantiate's cache keys on `(path, mesh)`, so every node that uses one glTF mesh shares
  one GPU mesh and one BLAS. The same field lets a scene place one prop out of a kit file:
  `{"ModelAsset":{"asset":{"path":"assets/local/flat42/kit.glb"},"mesh":"SM_Kettle"}}`.

**Where the data comes from:**
- **Geometry and materials** come from the existing Assimp path, one glTF mesh at a time,
  untransformed: `ModelLoader::loadMesh(path, mesh)`.
  - Assimp's glTF2 importer makes one `aiMesh` per primitive, in glTF mesh order. The loader
    maps a glTF mesh to its primitive range by the primitive counts it reads from the JSON.
  - Test 3 checks each mesh's triangle count against its accessors.
  - Materials stay as the Khronos goldens validate them.
  - *As built (step 2):* per-mesh loading drops two of the whole-file path's Assimp steps.
    `SortByPType` splits a mesh of mixed primitive types, and `FindDegenerates` turns
    degenerate triangles into lines and points that it would split off; either would shift
    the primitive ranges. `loadMesh` refuses a file whose Assimp mesh count is not its
    primitive count. Only the materials a mesh's primitives use are built (renumbered from
    0), so one prop of a kit loads only its own textures.
- **Structure** (nodes, `extras`, `KHR_lights_punctual`, cameras) is read from the glTF
  JSON itself: chunk 0 of a `.glb`, or the `.gltf`.
  - This gives exact `extras` and light semantics without depending on how Assimp
    translates metadata.
  - *As built (step 2):* `ohao/level` (`ohao_level`) parses it with simdjson's DOM, not the
    ECS's `parseJson`. `ohao_scene` links `ohao_level` (`loadMesh` needs the primitive
    counts), and `ohao_ecs` links `ohao_scene`, so `ohao_level` cannot depend on the ECS.
    It depends on `ohao_core`, glm and simdjson only.
  - `extras` are kept as compact JSON text (member order and integer/float kinds kept). The
    passthrough in step 3 parses that text with the scene reader.

**Lights:** `KHR_lights_punctual` becomes a `Light`.
- Point, spot and directional map to their types.
- Intensity is the file's (candela, or lux for directional) × `lightScale`. Blender exports
  with lighting mode "Standard" (`export_import_convert_lighting_mode='SPEC'`), whose
  units these are.
- Range is the file's, or 10 m (OHAO's default) when the extension leaves it infinite.
- Spot cones come from `innerConeAngle`/`outerConeAngle`.
- Area lights do not exist in the extension. A node carries one through passthrough
  (`"Light":{"type":"AreaRect",…}`).

**Cameras:** a perspective camera becomes a `Camera`:
- `fovDeg` from `yfov`; `near` and `far` (far 200 m when the file leaves it infinite).
- A level camera is active only if no other camera in the world is after the load. The
  scene's camera wins.

**Other nodes:**
- **Empties** are entities with a transform and whatever their `extras` give.
- **Coordinates:** glTF is Y up, right-handed and in metres, as OHAO is, so nothing is
  converted. A node's forward is its local −Z (Blender's +Y, after the exporter's Y-up
  conversion), as for `Camera`.
- **Unsupported content:**
  - skins, animations, morph targets and `KHR_materials_*` are ignored, with one
    `W_LEVEL_UNSUPPORTED` per kind;
  - `KHR_draco_mesh_compression` and `EXT_meshopt_compression` are refused with
    `E_LEVEL_UNSUPPORTED`;
  - files with several scenes use `scene` (or scene 0).

**Saving and editing:**
- Saving writes the `Level` entity and never its nodes, which are the file's.
- A command may change a level node at run time (move a crate, hide a wall); the change is
  not saved, and the command's result carries `W_LEVEL_NODE_UNSAVED`.
- `level.reload` discards such changes. Saved overrides of level nodes are later work.
- *As built (step 3):*
  - The level system lives in `ohao_level_world` (`ohao/level/level_hints.*`,
    `level_system.*`), above the ECS: it reads files through the host's `AssetResolver` and
    decodes passed-through components with reflection, so neither `ohao_level` (below the
    scene library) nor `world.cpp` (in `ohao_ecs`) can hold it. A host installs it with
    `level::installLevels(world, assets, types)`, which places `level` before `instantiate`
    whichever is installed first; the CLI context and `LiveHost` do. `scene.load` runs it at
    once (`World::runSync("level")`), so a headless `query` sees the nodes.
  - `LevelNode.index` (and the id hash) is the node's position in the default scene, depth
    first; a mesh node's `MeshSource` names its mesh `#<glTF mesh index>` (names may repeat).
  - A changed `Level` (and so a reload) re-expands in place: an existing node entity is
    kept and given back the file's name, transform, parent and saved components (anything
    else saved is removed); missing nodes are recreated, gone ones destroyed. An unchanged
    file therefore keeps `world.hash`, CreationOrder included. A file's structure is read
    once per path per World (step 5's `level.reload` will clear it).
  - A level's errors (file, or hints: `E_LEVEL_EXTRAS` with the count and the first
    message) go on the level entity's `InstantiateError`; every diagnostic is logged and
    kept for `level.info` (`level::levelDiagnostics`).
  - `col`, `body`, `mass`, `fric`, `rest` are parsed and checked now; step 4 applies them.

## Extras: the hint schema

Keys on a node's `extras`, all optional, short so a Blender custom property is quick to
type:

| Key | Values | Default | Becomes |
|---|---|---|---|
| `col` | `box`, `sphere`, `capsule`, `convex`, `mesh`, `none` | `mesh` on mesh nodes, `none` elsewhere | `Collider{shape}` fitted to the node's mesh |
| `body` | `static`, `dynamic`, `kinematic` | `static` | `RigidBody{motion}` |
| `mass` | kg, > 0 | 10 (dynamic only) | `RigidBody.mass` |
| `fric` | 0–1 | 0.6 | `RigidBody.friction` |
| `rest` | 0–1 | 0 | `RigidBody.restitution` |
| `vis` | `0` or `1` | `1` | `0`: collision only, no `MeshRenderer` |
| `spawn` | a tag, ≤ 32 chars (`player`, `enemy`, …) | — | `SpawnPoint{tag}` |
| `socket` | a name, ≤ 64 chars | — | `Socket{name}` |
| `trig` | `0` or `1` | `0` | `Collider.trigger` (added by sub-project 6, step 1) |
| `surf` | a surface, ≤ 32 chars (`concrete`, `metal`, `wood`, …) | — | `Collider.surface`, the footsteps' surface (sub-project 6, step 4) |

- **Component passthrough.** A key that names a registered, saved component (`Light`,
  `Spin`, `MaterialOverride`, …), with an object value, becomes that component.
  - It is read by the scene reader exactly as in a `.scene.json`: same schema, same
    diagnostics.
  - This lets a Blender node carry any component agents can write, without a new hint per
    feature.
- **File defaults.** The default scene's own `extras` may hold `col` and `body`, applied to
  every node that does not set them. A prop kit sets `"col":"none"` there; a blockout
  leaves the defaults.
- **Unreal naming, for Frank's assets.** Explicit `extras` win over both rules.
  - A mesh node named `UCX_<target>[_NN]` reads as `{"col":"convex","vis":0}`, a hull of
    the body of the node named `<target>`.
  - An empty named `SOCKET_<name>` reads as `{"socket":"<name>"}`.
  - Flat42's FBX use both conventions; `stage_flat42.py` also writes them as `extras`.
- **Strictness.** Errors and warnings name the node's path and the key.
  - An unknown lower-case key is `W_LEVEL_EXTRAS`. Blender add-ons write their own
    properties, and a level should not fail on them.
  - An unknown capitalised key is `E_LEVEL_EXTRAS`: probably a misspelt component.
  - A bad value is `E_LEVEL_EXTRAS`.
  - A node with an error loads without the bad component; the rest of the level loads. The
    level's errors are on the level entity (`InstantiateError`) and in `level.info`.
- **Schema.** The hint keys are published as a JSON Schema (`ohao schema level-extras`),
  and `check_scene_schema.py` validates every checked-in level's `extras` against it.
- **Size.** The blockout's 16 hinted nodes carry 13–42 bytes of `extras` each, 279 bytes in
  all (compact JSON, measured at step 1).
- **Authoring.** In Blender: Object Properties → Custom Properties, then export with
  Include → Custom Properties (`export_extras=True`).

## Collision generation

**Components** (saved, so scene files can author them too, on primitives as well as level
nodes):
- `Collider{shape, size, offset}`.
  - `size` is zero to fit from the mesh: half extents for a box, `x` the radius for a
    sphere, `x`/`y` the radius and half height for a capsule.
  - A fitted size is written back into the runtime body only, never into the component.
- `RigidBody{motion, mass, friction, restitution}`. An entity with a `Collider` and no
  `RigidBody` is static.

**Fitting from the node's mesh** (local space, then the node's world scale):
- **box:** the mesh's AABB (centre as offset).
- **sphere:** the AABB centre and the farthest vertex from it.
- **capsule:** along the AABB's longest axis, with the radius the larger of the other two
  half extents.
- **convex:** Jolt's hull of the vertices, at most 256 hull points. A larger hull is
  simplified by Jolt, and `physics.info` counts it.
- **mesh:** Jolt `MeshShape` over the triangles. It is allowed on static and kinematic
  bodies only; on a dynamic body it becomes `convex` with `W_LEVEL_DYNAMIC_MESH`, since
  Jolt does not simulate dynamic triangle meshes.
- **Hulls:** several `UCX_` hulls of one node become one body with a compound shape
  (`ShapeInfo` gains `COMPOUND`), so a dynamic prop is one body.
- **Scale:** non-uniform scale is baked into mesh and hull vertices and box extents; a
  sphere or capsule takes the largest axis, with a warning when the axes differ by more
  than 1%.

**The physics scene** (`ohao/ecs/physics_scene.{hpp,cpp}`, the sync system `physics`
after `transform`):
- It owns a `PhysicsBackend` (Jolt). It builds, updates and removes bodies as `Collider`,
  `RigidBody` and `WorldTransform` change, through the dirty tags, so a still world costs
  nothing.
- It uses the backend directly, not the `PhysicsWorld` wrapper with its box fallback.
- Bodies are created in entity creation order, and shapes are cached per `(mesh, shape,
  scale)`.
- Static bodies go on the `STATIC` layer, dynamic ones on `DYNAMIC`.
- **Dynamic bodies are built but not stepped.** No simulate system steps the physics world
  in 5. Stepping, sleeping and transform sync are 6's.
  - Test 7 steps the backend itself to prove the bodies collide.
  - `physics.info` reports dynamic bodies as `unstepped` until 6.
- `Level.collision: false` builds none of a level's bodies (a render-only level).

**As built (step 4):**
- The physics system is installed by `ecs::installPhysics(world, assets)`, beside
  `level::installLevels` in the CLI context and `LiveHost`, not by `World` itself: it needs
  the asset resolver to load meshes, as instantiate does. `ohao_ecs` links `ohao_physics`
  (core, glm and Jolt only, so no cycle).
- The backend interface is `physics::backend::IPhysicsBackend`; `ShapeInfo::COMPOUND`
  holds `children` (shape, position, rotation in the body's frame), built as a Jolt
  `StaticCompoundShape`. A single shape off the body's origin (a fitted box's offset, a
  capsule's turn) is a one-child compound.
- Dirty state is EnTT signals (`Collider`, `RigidBody`, `ColliderPart`, `CollisionMesh`,
  `MeshSource`, a destroyed body) plus the transform system's `PhysicsMoved` tag: a moved
  body is moved, a body whose scale changed (or whose hulls moved) is rebuilt.
- Runtime components: `ColliderPart{body}` (a `UCX_` hull, the target found by name, with or
  without its `_NN` suffix), `CollisionMesh{source}` (the mesh of an undrawn node),
  `PhysicsBody{handle}`.
- A level gives a node a `RigidBody` only when one of `body`, `mass`, `fric`, `rest` is set.
- The backend is created at the first body, single-threaded, with room for 65,536 bodies;
  its temp allocator falls back to the heap past 10 MB (Jolt's per-step contact buffer is
  sized by the body cap).
- Shapes are not yet cached per `(mesh, shape, scale)`: each body fits its own (the
  resolver caches the meshes). Left for step 6's kit2k budgets.
- Warnings `W_LEVEL_DYNAMIC_MESH`, `W_COLLIDER_NO_MESH`, `W_COLLIDER_SCALE` are in the
  catalogue and on `PhysicsScene::warnings()`; step 5 surfaces them in `physics.info`.

**Queries:**
- `physics.raycast {from, dir, max = 1000}` →
  `{hit, entity, name, point, normal, dist}`, or `{hit: false}`.
- `physics.info` → `{bodies: {static, dynamic, kinematic}, shapes: {box, sphere, capsule,
  convex, mesh, compound}, buildMs, unstepped}`.
- Both work in every host, headless included, and need no renderer.

## Spawn points

- `SpawnPoint{tag}` (saved). Its position and facing come from `WorldTransform`; it faces
  its local −Z.
- Agents find spawns with `query --has SpawnPoint`.
  - `level.info` lists them compactly: `{tag, entity, pos, yawDeg}`, in node order, so "the
    first `player` spawn" is well defined. 6 places the player there.
- A level without a `player` spawn gets `W_LEVEL_NO_PLAYER_SPAWN` in `level.info`. It is
  not an error, since a prop kit has none.
- Test 9 checks that every blockout spawn stands on ground: a ray down from 0.5 m above it
  hits within 0.6 m.

## The agent-facing surface

**API methods**, all with params structs, schemas and limits, as in 4:

| Method | Params | Result |
|---|---|---|
| `level.info` | `entity?` (default: the only level) | `{path, nodes, meshes, uniqueMeshes, triangles, colliders: {…}, bodies, spawns: […], lights, cameras, warnings: […], loadMs}` |
| `level.reload` | `entity?` | the same as `level.info`, after re-reading the file and rebuilding its entities and bodies |
| `physics.raycast` | `from`, `dir`, `max?` | as above |
| `physics.info` | — | as above |

**CLI:**
- `ohao level <glb>`: `level.info` for a file, without a GPU or a scene. One JSON line.
- `ohao run`, `ohao serve` and the window host handle `Level` like any component.

**Token budget:**
- `level.info` on the blockout ≤ 1 KB.
- `physics.raycast` ≤ 200 bytes.
- `tools/list` stays ≤ 24 KB.

**As built (step 5):**
- The methods live in `ohao/api/level.cpp`. `ohao_api` links `ohao_level_world` (level, ecs,
  reflect: no cycle) and reaches the level system through `level::levelSummary`,
  `levelDiagnostics` and `reloadLevel`, and the physics scene through `ecs::physicsScene(world)`,
  the accessors the install hooks leave on the World; no `Host` change was needed. A World
  without them answers `E_STATE`.
- Each method runs the `level`, `transform` and `physics` sync systems first (never
  instantiate or the render extract), so a query right after `scene.load` sees the nodes and
  bodies.
- `level.info` result: `{entity, path, nodes, meshes, uniqueMeshes, triangles, colliders:
  {by shape}, bodies: {by motion}, spawns: [{tag, entity, pos, yawDeg}] (the first 8, then
  moreSpawns), lights, cameras, warnings: [{code, message}] (the first 4, then moreWarnings),
  errors?, loadMs}`; counts are of the file's default scene (mesh nodes, the glTF meshes they
  use, their triangles, light and camera nodes); `loadMs` is the last expansion (bodies are
  physics.info's `buildMs`). Warnings: the level's hint diagnostics, `W_LEVEL_NO_PLAYER_SPAWN`
  and the physics warnings of its nodes. A level whose file did not load answers with its error.
- `physics.raycast`: point and normal to 1e-4, `name` cut at 40 bytes. Errors as the plan
  says: `E_PARAMS` (new; JSON-RPC -32602) for a zero `dir` or `max` outside (0, 1e5], so
  `max` has no field limits in its schema; `E_NOT_FOUND` (new) for no level or an entity that
  is not one; several levels without `entity` is `E_COMMAND`, naming them.
- `level.reload` forgets the file's structure and the resolver's meshes of it
  (`AssetResolver::forget`), re-expands in place and touches the nodes' colliders so their
  bodies are rebuilt; meshes already uploaded to the GPU are kept (instantiate's cache).
- `ohao level <file>` loads the file as the only `Level` (entity `0000000000000001`) of an
  empty world and prints `level.info`.
- The protocol and CLI goldens hold the timings (`loadMs`, `buildMs`) as 0.
- Measured: `level.info` on the blockout 482 bytes, a ray 103, `tools/list` 19,756 bytes for
  28 tools.

## Determinism

- The same file and scene give the same entities, ids, components, bodies and image:
  - level node ids are hashed (above);
  - expansion and body creation follow file order;
  - goldens over levels are exact (deferred, max_abs 0).
- `level.reload` of an unchanged file leaves `world.hash` unchanged (test 4).
- Jolt is deterministic for the same inputs in the same order on one build. 6 decides
  whether cross-platform determinism (`JPH_CROSS_PLATFORM_DETERMINISTIC`) is worth its
  cost.

## Performance budgets

Targets, gated from the step that delivers them. They change only by Frank's ruling.
`level_bench` (new, CPU, headless, with the noisy-run rules of `scene_bench`) measures
them, with results in `build/level_bench.json` and budgets in
`tests/bench/level_budgets.json`:

| Budget | Target | Gated from |
|---|---|---|
| `blockout_load_ms`: structure, 13 meshes, expansion, colliders (no GPU upload) | ≤ 20 ms | step 4 |
| `kit2k_load_ms`: a generated 2,000-node level over 40 unique meshes (~1 M triangles) | ≤ 400 ms | step 6 |
| `kit2k_collision_build_ms` | ≤ 150 ms | step 6 |
| `kit2k_raycast_us`: median of 10,000 random rays | ≤ 5 µs | step 6 |
| `idle_sync_us`: the `level` and `physics` systems on a still world with kit2k loaded | ≤ 10 µs | step 6 |
| `level_info_bytes` on the blockout | ≤ 1,024 | step 5 |

- The 2,000-node level is generated by `tools/level/kit_level.py` (Blender, checked in)
  from the blockout's primitives. It is never a golden.
- `render_bench` adds `blockout_1080p` for the frame cost of a level: reported, not gated,
  until 6.

## Rules for the step chain

The 2b rules apply unchanged (`2026-10-24-renderer-upgrade-design.md`, "Rules for the step
chain"):
- one plain command per tool call;
- foreground work;
- noisy benches are invalid, not failures;
- `-j6`, no parallel heavy builds, GPU tests one at a time;
- files are deleted one by one, never throwaway code under `ohao/` or `shaders/`;
- Python rewrites read before they write;
- goldens are looked at before they are updated;
- a wedged GPU stops the step;
- every step ends with build, tests, `check.py --all`, a change record and local commits.

One rule is new: **Blender runs only in tools and harnesses.**
- `tools/level/*.py` scripts are run by hand or by a harness when Blender is present.
- No gate requires Blender. A check that would need it reports `skip` and says so.

## Test plan

Numbered as the build steps reference them.

1. **Level assets** (`tests/level/check_level_assets.py`, CPU, step 1):
   - The blockout's `extras` validate against the hint schema.
   - The checked-in hint values are what `blockout_room.py` writes.
   - With Blender present, a rebuild is byte-identical to the checked-in `.glb`; without
     Blender this check is `skip`.
   - The blockout renders through today's `ModelAsset` path: an exact golden,
     `cli_blockout_room_deferred`.
2. **Structure reader** (`scene_tests`, CPU). Fixtures in `tests/level/fixtures/`,
   hand-written `.gltf` pinned to LF:
   - nodes, TRS and matrix transforms, hierarchy, `extras`, KHR lights and cameras;
   - error corpus: a bad `col`, an unknown component, a sheared matrix, Draco refused,
     several scenes.
   - *As built (step 2):* `sp5_t02_*` in `scene_tests` (9 tests). The hint errors (a bad
     `col`, an unknown component) belong to the hint mapping and move to step 3
     (`t02_extras_errors_name_the_node` in the plan's Task 3); the reader keeps `extras`
     as text and judges none of them. Malformed files (a child index out of range, text that
     is not JSON, a glTF 1 binary, a node cycle, a node with two parents) are
     `E_ASSET_INVALID`; orthographic cameras, skins,
     animations, morph targets and `KHR_materials_*` are listed in `unsupported`.
3. **Per-mesh geometry** (`scene_tests`, CPU):
   - `loadMesh` triangle counts equal the accessors for every blockout mesh;
   - a mesh used by two nodes is uploaded once (instantiate's cache counts);
   - an empty `mesh` field keeps today's merged output byte for byte.
   - *As built (step 2):* `sp5_t03_*` (5 tests). The whole-file check compares positions,
     colours, UVs, tangents, indices and materials against hashes taken at the step's base commit, normals
     aside, since the normals change on purpose (`sp5_t03_authored_hard_edges_kept`).
4. **Expansion** (`ecs_tests`):
   - one entity per node, parented as in the file;
   - stable ids;
   - a save writes only the `Level` entity;
   - `level.reload` of an unchanged file keeps `world.hash`;
   - `vis: 0` nodes have no `MeshRenderer`;
   - passthrough components equal the same JSON read from a scene file.
   - *As built (step 3):* `sp5_t04_*` and `sp5_t09_*` in `ecs_tests` (11 tests, on the CPU,
     over `tests/level/fixtures/level_small.gltf` and the blockout; ids across processes by
     running `ecs_tests --level-ids` as a child), and in `scene_tests` the hint reader
     (`sp5_t04_hints_read_keys_defaults_and_conventions`), its error corpus
     (`sp5_t02_extras_errors_name_the_node` over `level_errors.gltf`), the string limits and
     the checked-in extras schema. Test 9's grounding ray is step 4's (it needs collision).
5. **Render parity** (golden): the blockout through `Level` against step 1's merged golden.
   - Exact equality is expected; per-mesh BLAS and draw order may move RT GI noise.
   - If it is not exact, the golden is re-baselined after looking, in its own commit with
     its statistics.
6. **Shape fitting** (`physics_backend_tests` or a new `level_physics_tests`, CPU):
   - box, sphere, capsule, convex and mesh fits over fixtures;
   - `UCX_` hulls become one compound body;
   - a dynamic mesh becomes convex, with the warning.
7. **Collides** (the done criterion, CPU), on the blockout:
   - a ray down at each spawn hits at its height ±1 mm;
   - a ray through the doorway at 1 m passes, and one 0.3 m beside it hits `WallSouthA` at
     its face;
   - a ray onto the ramp returns its slope's normal;
   - with the backend stepped 120 times at 1/60 s, the crates rest at their authored
     heights ±2 cm and no static body moves.
8. **Agent surface:**
   - `api_tests` for the four methods (params, limits, errors, `E_STATE` where a host has
     no world);
   - serve goldens; `jsonschema` in `check_serve.py`;
   - `e2e_mcp.py` loads the blockout, reads `level.info` and casts a ray;
   - `tools/list` ≤ 24 KB.
9. **Spawns:** order, tags, facing and grounding (above).
10. **Flat42 kit** (local; `skip` without `assets/local/flat42`):
    - the staged props load textured;
    - hulls are convex colliders and not drawn;
    - sockets are present;
    - look shots go to `C:\Users\djmax\ohao_refs\look_shots\sp5_level\`.
11. **Budgets:** `level_bench --check`, five consecutive clean runs.
12. **Docs:**
    - `check_docs.py` runs the new commands in AGENTS.md, ARCHITECTURE.md ("add a level"
      recipe) and the CLI and serve guides;
    - a monograph unit for the level pipeline.

## Done criteria

All verified by running, none estimated.
- Tests 1–12 pass, and every existing suite, golden and gate stays green.
- `python tools/check.py --all` exits 0.
- Every budget passes five consecutive clean runs, with the results in the change records.
- The roadmap row is marked done, and each step has a change record in `docs/changes/`.

## Build order

Each step ends as in "Rules for the step chain": build, its tests, `check.py --all`,
`docs/changes/YYYY-MM-DD-level-pipeline-stepN.md`, local commits. A step that misses a
budget it gates, sees a golden change it cannot explain, or needs a dependency not named
here stops and reports.

1. **Assets and staging.**
   - `tools/level/blockout_room.py` and `assets/levels/blockout_room.glb` (with
     `assets/levels/README.md`: provenance, license, rebuild).
   - `assets/scenes/blockout_room.scene.json` through today's `ModelAsset` path, and its
     golden.
   - `tools/level/stage_flat42.py`, and `assets/local/` git-ignored with its README.
   - The hint schema as data the check reads (`tests/level/extras_schema.json`, moved
     behind `ohao schema level-extras` in step 3).
   - Test 1.
   - Look shots of the blockout and of the staged props through today's path. The props
     show their hulls, which is the gap step 3 closes.
2. **Structure and per-mesh geometry.**
   - The glTF structure reader (`ohao/level/gltf_structure.*`).
   - `ModelLoader::loadMesh`, `ModelAsset.mesh` and instantiate's `(path, mesh)` cache.
   - Normals: the loader keeps a file's normals and repairs only meshes without them (see
     "Why"); the goldens this moves are re-baselined after looking, in their own commit.
   - Tests 2 and 3.
   - *As built:* done 2026-10-26 (`docs/changes/2026-10-26-level-pipeline-step2.md`). Two
     additions the step needed: a reflection attribute, `omitDefault`, so `ModelAsset.mesh`
     is written (and hashed) only when set and every existing scene file and `world.hash`
     keeps its bytes; and `Result` (`core/result.hpp`) in place of `std::expected`, which
     C++20 lacks.
3. **Levels in the world.**
   - The `Level` and `LevelNode` components and the `level` sync system: expansion, ids,
     saving, `vis`, hints → components, passthrough, the `UCX_`/`SOCKET_` conventions, KHR
     lights, cameras.
   - `SpawnPoint` and `Socket`; `ohao schema level-extras`.
   - The blockout scene moves to `Level`, with its golden (test 5, own commit).
   - Tests 4 and 9.
   - *As built:* done 2026-10-26 (`docs/changes/2026-10-26-level-pipeline-step3.md`). Test 5
     was exact (max_abs 0): no re-baseline. Additions the step needed: reflection's
     `maxLength` for string fields (`SpawnPoint.tag` ≤ 32, `Socket.name` ≤ 64, checked by both
     decoders, in the schemas and the catalogue); `World::runSync`; five error codes in the
     catalogue (`E_LEVEL_NODE`, `E_LEVEL_UNSUPPORTED`, `E_LEVEL_EXTRAS`, `W_LEVEL_EXTRAS`,
     `W_LEVEL_UNSUPPORTED`), the `E_LEVEL_*` ones in exit class 2.
4. **Collision.**
   - `Collider` and `RigidBody`; `ShapeInfo::COMPOUND`; shape fitting.
   - The `physics` sync system over `PhysicsBackend`.
   - Tests 6 and 7; `blockout_load_ms`.
5. **Agent surface.**
   - `level.info`, `level.reload`, `physics.raycast`, `physics.info`; `ohao level`.
   - The serve and CLI guides, the AGENTS.md recipe.
   - Test 8; `level_info_bytes`.
6. **Scale and the kit.**
   - `tools/level/kit_level.py` and `level_bench` with the kit2k budgets.
   - `render_bench`'s `blockout_1080p` (reported).
   - Test 10 on the staged Flat42 props: a local scene of the blockout furnished with them.
   - Tests 11 and 12; the monograph unit; the roadmap row marked done.

## Risks

- **Assimp's per-primitive mapping.** If Assimp reorders or merges primitives, the
  `(mesh → primitive range)` mapping breaks. Test 3 catches it. The fallback is to read the
  geometry from the glTF buffers directly in the structure reader (positions, normals, UVs,
  tangents, indices) and keep Assimp for materials.
- **Jolt build cost of mesh shapes** on large levels. The kit2k budgets measure it. Mesh
  shapes cache per mesh and scale.
- **Exporter drift.** The Blender exporter's output (names, the order of `extras`,
  generator string) can change between Blender versions. The byte-identity check is
  pinned to the Blender version that made the file and is a `skip` on any other.
- **The 6 boundary.** 5 builds bodies; 6 steps them. A world with dynamic bodies and no
  stepping looks inert, so `physics.info` says `unstepped` explicitly.
- **Licensing of Frank's assets** (Open decision 1).

## Review focus

- **Hints that fail quietly.** A misspelt key must never change a level without a
  diagnostic: unknown lower-case keys warn, unknown capitalised keys fail.
- **Level nodes and saving.** A save must never write a level's node entities, or a scene
  would duplicate its level on the next load.
- **Determinism of ids.** A reload, a recording replay and a fresh process give the same
  level node ids.
- **Collision fitting under scale**, especially non-uniform scale and mirrored nodes
  (negative determinant: the triangle winding Jolt sees).
- **The `vis: 0` path:** a collision-only node must cost no draw, no BLAS and no upload.

## Decisions record

- **A level is referenced, not imported** (`Level{asset}` expands at load). An import into
  `.scene.json` was rejected:
  - it copies every node into the scene (fat JSON, against Frank's token rule);
  - it makes a Blender re-export a merge problem.
- **Structure is read from the glTF JSON, geometry from Assimp.** This gives exact
  `extras` and KHR semantics while keeping the material path the Khronos goldens validate.
  A new glTF library (cgltf, fastgltf) was rejected for now: a second geometry and material
  path for one file format.
- **The hint keys are flat and short** (`col`, `body`, `spawn`), with component passthrough
  for everything else. Flat keys are what Blender's custom-property panel types quickly.
  Passthrough reuses the scene schema instead of growing a second one.
- **Collision is built in 5, simulated in 6.** "Collides" is proven by queries and by test
  stepping, so the world's fixed-step physics and its transform sync stay one design,
  in 6.
- **Somnabulate assets stay out of git until Frank rules.** The generated blockout is the
  checked-in test level.

## Frank actions

- Rule on Open decision 1 (whether Somnabulate assets may be committed).
- Optionally, author a hand-made blockout in Blender with the hints. The generated one is
  enough for every gate, but a designer's level shows what the hints miss.
- Choose which Somnabulate space becomes the first real level after the blockout (the
  Delilah d1 or flat reference renders are candidates).

## Open decisions

1. **May Somnabulate assets be committed to this MIT repository?** If yes, under what
   notice (for example an `assets/somnabulate/LICENSE.md` reserving all rights, as the
   Khronos pack carries its own licenses)? Until then they are staged in git-ignored
   `assets/local/`.
2. **Do `UCX_` / `SOCKET_` naming conventions stay** once Frank's assets are re-exported
   with `extras`? They cost little and match Unreal, so this spec keeps them.
