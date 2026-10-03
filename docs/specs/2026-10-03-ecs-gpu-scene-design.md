# Sub-project 2a — ECS world + persistent GPU scene

Status: design approved in conversation 2026-10-03; this document is the binding spec.
Roadmap: sub-project 2 is split into **2a** (this spec) and **2b** (indirect draws, GPU
frustum culling, two-pass Hi-Z occlusion — its own spec, after 2a).

## Goal

Game code lives in an EnTT registry with systems in an explicit fixed order. The
renderer reads a persistent GPU-side render world fed incrementally from that registry,
so per-frame CPU work scales with what changed, not with scene size, and moving objects
are correct in raster and ray tracing alike.

## Why (what the current code does)

- `Scene`/`Actor`/`Component`: `shared_ptr` components, one per type, raw-pointer
  hierarchy; ~15 files outside `ohao/scene` walk `getAllActors()` every frame.
- The G-buffer pass re-scans every vertex of every model per frame for culling,
  resolves textures by string per draw, and pushes a 240-byte constant per draw.
- All GPU scene data is rebuilt in one blocking pass, only on `setScene`. Meshes are
  duplicated per actor, one BLAS each. **A transform change never reaches the TLAS.**
- RT shading indexes materials by a global triangle offset (`instanceCustomIndex`),
  which diverges when a mesh is hidden.
- Two texture systems: deferred uses native-resolution bindless textures; the path
  tracer uses one array image with every texture bilinear-resized to a common size.

## Decisions

| Decision | Choice |
|---|---|
| Old scene code | Deleted outright — no adapter (`Scene`, `Actor`, `Component`, `SceneObject`, legacy `Transform`, component factory/packs, `DefaultSceneFactory`) |
| Architecture | Render-world extraction: game registry → `RenderScene`; the renderer never includes EnTT |
| Physics | Decoupled now (handle-based, no Component/Transform pointers); ECS physics sync is sub-project 4 |
| Hierarchy | In 2a: `Parent`/`Children` + parent-before-child world-transform propagation |
| Textures | One bindless texture table (`TextureManager`, native resolution) for deferred, path tracer and RT GI |
| Proof | Deferred goldens captured on master before any change; RT goldens gate the RT migration |

## Architecture

### `ohao/ecs` (new library `ohao_ecs`)

- EnTT via FetchContent, pinned release tag `v3.13.2` (or the newest stable tag at
  implementation time; the pin is recorded in `external/cmake/entt.cmake`).
- `World`: owns the `entt::registry` and a fixed, explicitly ordered system list.
  Systems are plain functions `void(World&, float dt)` registered in order; there is
  no automatic discovery or dependency sorting.
- Components:
  - `Name { std::string }`
  - `Transform { vec3 position; quat rotation; vec3 scale; }` — local
  - `WorldTransform { mat4 world; mat4 prevWorld; }`
  - `Parent { entt::entity }`, `Children { std::vector<entt::entity> }`
  - `MeshRenderer { MeshHandle mesh; std::vector<MaterialHandle> materials; bool visible; }`
    (one material per submesh)
  - `Light { type, color, intensity, range, radius, inner/outer cone }` — direction and
    position come from `WorldTransform`
  - Tags: `TransformDirty`, `RenderDirty`
- Write rule: gameplay changes `Transform` through `registry.patch<Transform>` or
  `World::setTransform`; EnTT's update signal tags `TransformDirty`. A direct
  `get<Transform>()` write is not tracked; a debug-build check detects world/local
  mismatch on untagged entities.
- Systems in 2a, in order: `TransformSystem` (recomputes world matrices for dirty
  subtrees, parents first; tags changed renderables `RenderDirty`), then
  `RenderExtractSystem`.

### `ohao/render/scene` (new): `RenderScene`

No EnTT dependency. Owned by `VulkanRenderer`.

- **`MeshRegistry`**
  - A mesh is registered once from `MeshData { vertices, indices (mesh-local),
    submeshes {firstIndex, indexCount, materialSlot}, triangleMaterialSlot[] }`.
  - The local AABB is computed once at registration.
  - Device-local vertex and index arenas, suballocated; growth by reallocate + copy.
    Draws use `vertexOffset`; BLAS geometry uses the same offsets.
  - One BLAS per mesh, built at registration, shared by every instance.
  - `MeshHandle {index, generation}`.
  - The vertex format stays today's 100-byte `Vertex` in 2a.
- **`MaterialTable`**: a storage buffer of `MaterialGpuPack` (48 bytes, 3 × vec4),
  shared by raster and RT. Texture indices are indices into the one bindless texture
  table, resolved at registration. `MaterialHandle {index, generation}`.
- **`InstanceTable`**: device-local storage buffer, std430 entry per slot:
  `mat4 world; mat4 prevWorld; uint mesh; uint firstMaterial; uint flags; uint pad;`.
  Slots come from a free list; slot 0..N reused after release.
- **`LightTable`**: storage buffer of lights packed from `Light` + `WorldTransform`.
  Raster keeps its 8-light limit in 2a; RT keeps `GPULight` (64 max).
- **TLAS**: built from the instance table. `instanceCustomIndex` = instance slot.

### Renderer consumption

- G-buffer and CSM: one `vkCmdDrawIndexed` per visible instance × submesh with
  `firstInstance = slot`; shaders read the instance via `gl_InstanceIndex` and the
  material via `firstMaterial + submesh.materialSlot`. Camera matrices move to a
  uniform buffer; the 240-byte per-draw push constant is removed. CPU AABB culling
  uses the registered AABB (no vertex scan). Recording stays on the CPU in 2a.
- Path tracer, RT shadows, RT GI: hit shaders resolve instance slot → mesh →
  `indexOffset + 3·gl_PrimitiveID` → vertices; material = `instance.firstMaterial +
  mesh.triangleMaterialSlot[gl_PrimitiveID]`. The global-triangle-offset scheme and
  its separate normal/UV/material-ID global buffers are removed.
- Per-object motion vectors: `prevWorld` replaces today's "previous MVP from the current
  model matrix".

### Assets

`ModelLoader` stays and returns `MeshData` + material descriptions (+ decoded
textures). A spawn helper registers meshes/materials/textures with `RenderScene` and
creates entities. Multi-material models become one mesh with submeshes (today's
"split into one actor per material" in `model_viewer` goes away).

### Physics decoupling

`PhysicsWorld`/`RigidBody` lose their `PhysicsComponent`/`TransformComponent`
pointers. Bodies are created and addressed by `BodyHandle`; poses are read and written
through `PhysicsWorld` calls. `PhysicsComponent` is deleted. Physics and force-generator
test suites stay green. No ECS physics system in 2a.

## Frame flow

1. Game systems (none beyond `ohao_game`'s camera and a spinning object in 2a).
2. `TransformSystem`.
3. `RenderExtractSystem`:
   - queued `MeshRenderer` construct/destroy → slot allocate/release;
   - `RenderDirty` instances → this frame slot's staging buffer; instances that moved
     last frame are written once more so `prevWorld` catches up (motion vectors return
     to zero);
   - lights → staging;
   - TLAS decision: rebuild if instances were added/removed or a mesh changed, refit if
     only transforms moved, nothing otherwise.
4. Command buffer start: `vkCmdCopyBuffer` staging → device-local instance / light /
   TLAS-instance buffers; barrier transfer-write → all shader reads and AS build input.
   Single device-local buffers are safe because the copy is ordered after the previous
   frame's commands on the same queue. (This replaces the host-mapped light buffer
   rewritten while frames are in flight.)
5. TLAS build or refit recorded in the frame (no blocking single-time submit); barrier
   → RT passes.
6. Passes.

## Error handling

- Stale handle (generation mismatch): log once per handle, skip the entity.
- Arena growth failure: log, refuse the spawn; the frame renders without it.
- Render scene empty: passes run (G-buffer still clears), RT passes skip (no TLAS),
  lighting falls back as today.
- Single-threaded in 2a.

## Build order

Each step ends with the engine running and every suite green.

1. Capture deferred goldens (cornell_box, damaged_helmet; deferred mode) on master;
   add to `tests/golden/manifest.json`.
2. EnTT + `ohao/ecs` (`World`, components, `TransformSystem`) with CPU unit tests.
3. Physics decoupling.
4. `RenderScene` (`MeshRegistry`, `MaterialTable`, `InstanceTable`, `LightTable`,
   TLAS) with headless GPU tests.
5. Deferred G-buffer + CSM on `RenderScene`; bindless texture unification.
6. Path tracer + RT shadows + RT GI on instance indexing and the shared texture table;
   in-frame TLAS refit/rebuild.
7. Port examples (cornell_box, model_viewer, turntable, env_demo, ohao_game,
   `SceneFramer`) to entities; ohao_game gains a spinning object.
8. Delete the old scene code; port tests; re-derive monograph units; change document
   (`docs/changes/`, written by a Haiku agent, reviewed before commit).

## Done criteria

All verified by running, none estimated.

- Deferred goldens (step 1) pass within their tolerance after the migration.
- `cornell_box` RT golden bit-identical.
- `damaged_helmet` RT golden: may change only because textures are no longer
  downsampled. Verified at 1024 spp old vs new: pixels outside the helmet's textured
  surfaces stay within the golden tolerance; the golden is then updated.
- Moving objects in RT: an engine test moves an instance and asserts (a) the frame took
  the refit path, (b) the TLAS instance transform equals the new world matrix.
  ohao_game's spinning object casts correct RT shadows (visual check by the user).
- Incremental upload: engine test — 10k static instances upload 0 instance bytes per
  frame after the first; with 1% moving, bytes uploaded ∝ moving count.
- CPU cost: 10k instances, 1% moving: `TransformSystem` + extract + staging < 0.5 ms
  (measured, Release).
- Zero validation messages: `tests/validation/check_validation.py` and
  `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` (teardown included).
- `ohao_game` ≥ 60 fps at 1080p.
- All suites green; monograph builds with every citation resolving.

## Out of scope (2b and later)

Indirect draws, GPU frustum culling, Hi-Z occlusion, `drawIndirectCount`; meshlets;
vertex compression; render thread; ECS physics sync (sub-project 4); glTF level loading
(sub-project 3); BLAS compaction; skinned meshes.

## Review focus

Inputs no step's tests exercise by default, most likely to bite first:

1. Destroying a parent entity with live children: children must be destroyed or
   detached, never left with a dangling `Parent`.
2. A slot released and reused in the same frame: the TLAS must rebuild, and no draw may
   use the old mesh/material with the new slot.
3. An entity whose `MeshRenderer.materials` has fewer entries than the mesh has
   submeshes: fall back to a default material, never read past the table.
4. `Transform` written via `get<>` instead of `patch`: debug check must fire.
5. Arena growth while instances are in flight: old buffers must stay alive until the
   frames using them retire.
