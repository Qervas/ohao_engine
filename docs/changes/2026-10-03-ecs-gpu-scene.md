# Sub-project 2a: ECS world and persistent GPU scene

**Branch:** `ecs-gpu-scene` (16 implementation commits `c4fb186..9519b29` on `master`, plus one documentation commit), not merged  
**Spec:** `docs/specs/2026-10-03-ecs-gpu-scene-design.md`  
**Plan:** `docs/plans/2026-10-03-ecs-gpu-scene.md`  
**Decision ledger:** `.superpowers/sdd/2026-10-03-ecs-gpu-scene/progress.md`  
**Status:** Sub-project 2a implemented; 2b (indirect draws, GPU culling, Hi-Z) is the next spec.

## 1. Summary

Game code now lives in an EnTT registry (`ohao/ecs`) with systems that run in a fixed, explicit order. The renderer no longer includes EnTT or the old `Scene`/`Actor`/`Component` system; it reads `ohao::render::RenderScene`, a persistent GPU-side world with mesh arenas (one BLAS per mesh), material, instance and light tables, and an in-frame TLAS that is built or refit from the instance table each frame. The deferred G-buffer and cascaded shadow maps draw one instance per `vkCmdDrawIndexed` (the G-buffer's only per-draw state is a 16-byte material-slot push constant; CSM pushes its `viewProj` once per cascade), and the path tracer, RT shadows and RT GI trace the same TLAS and share one bindless texture table at native resolution. The old scene code, the temporary scene bridge, the legacy RT build and the texture array are deleted. The deferred goldens were captured on master before the migration and pass; the RT goldens were kept, with `cornell_box` bit-identical and the `damaged_helmet` RT reference re-captured for the environment-map fix.

## 2. New modules and APIs

### 2.1 `ohao/ecs` (library `ohao_ecs`)

EnTT is fetched through `external/cmake/entt.cmake` at tag `v3.13.2`. `ohao/ecs/CMakeLists.txt` builds `ohao_ecs` from every `.cpp` and `.hpp` in the directory and links `ohao_core`, `ohao_renderer`, `ohao_scene`, `glm` and `EnTT::EnTT`. `ohao_ecs` is the only library that includes EnTT on the rendering side of the boundary.

**`ohao/ecs/world.hpp` / `world.cpp`: `ecs::World`**
- Owns an `entt::registry` (`registry()` accessor) and an ordered list of systems. Systems are `void(World&, float dt)`. There is no discovery and no dependency sorting.
- `create(name, local)` returns an entity and stamps a `CreationOrder`. `destroy(e)` destroys the entity's subtree.
- `setParent(child, parent)` returns `bool` and rejects cycles. `setTransform(e, local)` is the tracked write path.
- `addSystem(name, fn)` appends; `addSystemAfter(after, name, fn)` inserts after a named system (used to place `render_extract` after `transform`).
- `update(dt)` runs the systems in order. A signal on `Transform` tags `TransformDirty` when the component is patched.

**`ohao/ecs/components.hpp`**
- `Name { std::string value }`
- `Transform { position, rotation, scale }`: local transform.
- `WorldTransform { world, prevWorld }`: written by `TransformSystem`. `prevWorld` feeds motion vectors.
- `Parent { entt::entity entity }`, `Children { std::vector<entt::entity> entities }`.
- `MeshRenderer { MeshHandle mesh; std::vector<MaterialHandle> materials; bool visible; }`: one material per submesh.
- `Light` with `LightType { Point, Directional, Spot, AreaRect }`, and `EmissiveProxyLight`. Direction and position come from `WorldTransform`.
- `CreationOrder { seq }`: sets the order in which instances and lights are created, which is the order the RT path tracer and the goldens depend on.
- `GiMaterialLegacy { packed }`: the legacy RT GI word (defaults 0.73, 0.73, 0.73, 0.95).
- Tags: `TransformDirty`, `RenderDirty`, `MovedLastFrame`.

**`ohao/ecs/transform_system.hpp` / `.cpp`**
- `updateTransforms(entt::registry&)` recomputes world matrices for dirty subtrees, parents first, and tags moved renderables `RenderDirty` and `MovedLastFrame`.
- `verifyTransforms` detects a world matrix that disagrees with the local transform (the "unpatched write" check). It exists and is tested; no automatic debug-build caller is wired in yet (see section 7).

**`ohao/ecs/render_extract.hpp` / `.cpp`**
- `installRenderExtract(World&, render::RenderScene&)` adds the `render_extract` system after `transform`. Each update it:
  1. releases the slots of entities whose `MeshRenderer` or `RenderSlot` went away;
  2. creates an instance for every renderable without a slot, in creation order;
  3. re-uploads every `RenderDirty` renderable;
  4. clears `RenderDirty`;
  5. rebuilds the light list when a `Light` or `EmissiveProxyLight` changed, appeared or was destroyed.
- `RenderSlot { uint32_t slot }` is owned by the extract system only.
- Renderables whose mesh handle the scene does not know are skipped, and the skip is logged once per distinct handle. A failed `createInstance` is treated as a skip.
- The `RenderScene` must outlive the `World`. Destroying the `World` releases the slots and clears the lights it set.

**`ohao/ecs/spawn.hpp` / `.cpp`**
- `uploadModel(...)` takes the `RenderScene`, a `BindlessTextureManager*` and a model, and returns `SpawnedModel`. It is a thin wrapper over `render::uploadModel`.
- `spawnModel(World&, RenderScene&, textures, ...)` creates an entity with a `MeshRenderer` for each uploaded model.
- `spawnLight(World&, name, Light, Transform)` creates a light entity.
- `lookRotation(...)` returns a rotation whose -Z axis points along a direction (used by lights and the turntable panel).

**`ohao/ecs/framing.hpp` / `.cpp`**
- `spawnFramedLights(World&, const FrameResult&)` replaces `SceneFramer::applyLights`. The renderer-side `scene_framer.hpp` no longer names any ECS type.

### 2.2 `ohao/render/scene`: `RenderScene`

Owned by `VulkanRenderer` (`renderScene()` accessor). It has no EnTT dependency. Files:

- `render_scene.hpp` / `.cpp`: the `RenderScene` class.
- `render_scene_types.hpp`: `MeshData`, `MaterialDesc`, `InstanceDesc`, `LightDesc`, `LightData` (moved here from the renderer headers, still in namespace `ohao`), `RenderSceneStats`, `MAX_LIGHTS = 8` (raster limit), `kMaxMaterialRefsPerInstance = 64`, `kNoTexture`, `kInstanceVisible`.
- `handles.hpp`: `MeshHandle` and `MaterialHandle`, each `{index, generation}`.
- `mesh_registry.hpp` / `.cpp`: `MeshRegistry`. Mesh data is registered once. The AABB is computed at registration. Vertex and index arenas are device-local and suballocated; growth reallocates and copies. Each mesh has one BLAS, built at registration and shared by every instance. Vertex format is the existing 100-byte `Vertex`.
- `material_table.hpp` / `.cpp`: `MaterialTable`, a 4096-entry material storage buffer shared by raster and RT. Texture indices refer to the one bindless texture table.
- `instance_table.hpp` / `.cpp`: `InstanceTable`, a device-local storage buffer with one entry per slot (`world`, `prevWorld`, mesh, first material, flags). Slots come from a lowest-first free list. Each slot has a list of material references. Growth mid-frame defers destruction of the old buffers.
- `light_table.hpp` / `.cpp`: `LightTable`. Produces both the raster `LightData` array (at most 8) and the RT `GPULight` buffer (header `{count, envTexIdx}` followed by `GPULight[]`). Lights sort in creation order. The raster shadow map goes to the first Point or Spot light (the legacy rule).
- `model_upload.hpp` / `.cpp`: `render::uploadModel(RenderScene&, BindlessTextureManager*, Model, ...)` returns `UploadedModel`. It turns a model into submeshes and materials, registers its textures, and applies the legacy rules: per-kind texture formats, the emissive auto-light (`emissiveProxy`), and the RT GI legacy word (`giMaterialLegacyPacked`). It moved here from `gpu/vulkan` so the renderer stays ECS-free.

**`RenderScene` public surface (`render_scene.hpp`)**
- `InitInfo { instance, physical, device, queue, queueFamily, commandPool, rayTracing, rayQuery }`. `rayTracing` gives the scene its own `RTAccelerationStructure`. `rayQuery` changes which stages the TLAS barrier reaches.
- `initialize(InitInfo)`, `shutdown()`.
- `registerMesh(MeshData)` (blocking, load time), `registerMaterial(MaterialDesc)`.
- `createInstance(InstanceDesc)` returns a slot; `updateInstance(slot, desc)`; `destroyInstance(slot)`.
- `setLights(span<LightDesc>)`, `setEnvMapTextureIndex(idx)`, `setEnvIntensity(scale)`.
- `recordUpdates(cmd, frameSlot)`: called at the start of the frame command buffer after the slot's fence wait. Copies dirty runs from the slot's staging buffer into the device-local tables, then records the TLAS build or refit.
- `onFrameRetired()`: ticks the retire queue after the fence wait.
- Accessors: `vertexArena()`, `indexArena()`, `triangleSlotBuffer()`, `meshTable()`, `materialBuffer()`, `materialRefBuffer()`, `instanceBuffer()`, `rtLightBuffer()`, `rtLightCount()`, `lights()`, `rasterLights()`, `draws()`, `meshInfo(h)`, `materialIndexFor(slot, materialSlot)`, `rtSupported()`, `rtAccel()`, `tlas()`, `tlasTransform(slot)`, `stats()`, `empty()`.
- `draws()` returns `Draw { slot, firstIndex, indexCount, vertexOffset, materialSlot, aabbMin, aabbMax, world }` for every visible instance and submesh. It is rebuilt only when instances change.
- `RenderSceneStats` includes `recordCpuMs` (CPU time of the last `recordUpdates`), `instanceTableGrowths` (cumulative mid-frame growths), and `lastTlasOp` (`None`, `Build` or `Refit`).

**Retire queue.** Replaced device buffers, and replaced TLAS storage and scratch, wait `MAX_FRAMES_IN_FLIGHT` frames in a retire queue before destruction. A frame still reading an old buffer never sees it freed.

### 2.3 `RTAccelerationStructure` additions

File: `ohao/render/rt/rt_acceleration_structure.hpp` / `.cpp`.

- `createBLAS(...)` takes a `firstVertex` argument (default 0), because the arenas store mesh-local indices.
- `buildTLASFromDevice(cmd, instanceAddress, count, rebuild)` builds or refits a TLAS from a device-address instance buffer, with its own scratch. It returns `bool`; a failed allocation is not recorded as a build. Replaced TLAS storage and scratch go to the owning `RenderScene`'s retire queue.
- `InitInfo` plumbs the `rayQuery` flag through `renderer` and `device_setup` (`ohao/gpu/vulkan/device_setup.cpp`).

### 2.4 Other changed or new files

- `ohao/gpu/vulkan/env_upload.cpp` (new, 298 lines): environment map upload. It runs at the start of `render()` when the env map path changes.
- `ohao/gpu/vulkan/bindless_texture_manager.hpp` / `.cpp`: the texture table is sized to a capacity of 4096 (see section 4).
- `ohao/gpu/vulkan/CMakeLists.txt`: adds `/INCLUDE:vmaCreateAllocator` to the `ohao_gpu_vulkan` link (see section 6).
- `ohao/physics/dynamics/rigid_body.hpp` / `.cpp`, `ohao/physics/world/physics_world.hpp` / `.cpp`: bodies are created by name with `createRigidBody(name)`, addressed by `BodyHandle`, and named back with `resolveHandleName`. No scene component or transform pointer is involved.
- `ohao/runtime/application.hpp` / `.cpp`: `runtime::Application` owns an `ecs::World` and runs update hook, then `world.update`, then render.
- `ohao/scene/asset/`: `ModelLoader` and `PrimitiveMeshGenerator` remain. `generateSphere` is now the 32x16 sphere the Cornell box always rendered.
- `examples/ohao_game.cpp`: spins a cube through `World::setTransform` next to the DamagedHelmet, so the spin is a tracked transform write in raster and RT alike.

## 3. Removed

**Scene system (`ohao/scene`):** `actor/actor.*`, `component/*` (component, component factory, component pack, light, material, mesh and transform components), `default_scene_factory.*`, `scene.*`, `scene_module.hpp`, `scene_object.*`, `transform.*`. `ohao/scene` now contains only `asset/` and `CMakeLists.txt`.

**Core:** `ohao/core/common_types.hpp` (26 lines) and the `ComponentType` concept in `ohao/core/concepts.hpp`.

**Physics components:** `ohao/physics/components/physics_component.*`. `PhysicsWorld` and `RigidBody` no longer include scene headers; the physics-to-scene include was removed.

**Renderer, legacy RT build and uploads:**
- `ohao/gpu/vulkan/rt_build.cpp`: the legacy RT scene build (global vertex, normal, UV and index buffers, per-triangle material IDs, the texture array, per-actor BLAS and TLAS).
- `ohao/gpu/vulkan/scene_upload.cpp`: the old scene upload.
- `ohao/gpu/vulkan/light_upload.cpp` (566 lines): the light-buffer scene walk.
- `ohao/gpu/vulkan/scene_bridge.cpp`: the temporary bridge that fed the old `Scene` into `RenderScene` between sub-project steps. It was added and deleted on this branch, so it never appears on master.
- `VulkanRenderer::setScene`, `getScene`, `updateSceneBuffers`, `updateRTLightParams`, `hasSceneMeshes`; `DeferredRenderer::setScene`; `ObjectPushConstants` and `kObjectPushConstantsBytes`; `MeshBufferInfo`; the renderer's `m_rtAccel` (the scene now owns its acceleration structures).
- `ohao/render/rt/path_tracer_images.cpp`: its 60 lines are removed with the old image path (the file is still present).
- The per-draw push constant (240 bytes) and the combined raster vertex and index buffers.
- The texture array and the per-material triangle-offset scheme (`instanceCustomIndex` as a global triangle offset).
- `uploadDeferredTextures`.

**Not deleted:** `ohao/gpu/vulkan/buffer_setup.cpp` shrank (207 lines changed) but remains.

**Ports:** `examples/cornell_box.cpp`, `env_demo.cpp`, `model_viewer.cpp`, `turntable.cpp` and `ohao_game.cpp` now build ECS worlds. Multi-material models are one mesh with submeshes; the old "one actor per material" split is gone.

## 4. Rendering changes

**Deferred G-buffer.** `GBufferPass` binds set 1 "scene": instances, materials, material references, and a camera UBO. It writes one set and one camera UBO per frame slot in `execute`. It issues one indexed draw per visible instance and submesh (`Draw` from `RenderScene::draws()`), with `firstInstance = slot`. The only push constant is a 16-byte material-slot value. Camera matrices, including `prevViewProj`, are in the UBO. Shaders: `shaders/core/gbuffer.vert`, `shaders/core/gbuffer.frag`, `shaders/includes/scene/scene_data.glsl` (new). Motion vectors are computed from `inst.prevWorld` and `cam.prevViewProj`, not from the previous frame's model-view-projection.

**CSM.** `CSMPass` uses set 0 as the instance table, one set per frame slot, and a 64-byte `viewProj` push constant. Shader: `shaders/shadow/shadow_csm.vert`.

**Lights.** The raster light UBO is filled from `RenderScene::rasterLights()` (at most 8), one buffer per frame slot (`VulkanRenderer::updateLightBuffer(slot)`). The default-sun fallback is kept when no light is present.

**RT accumulation.** After `recordUpdates`, a frame whose TLAS was built or refit, or that uploaded instance data or lights, calls the active RT profile's `notifyViewChanged`: the offline profile restarts accumulation, the realtime profile treats the frame as a camera cut (NRD history reset, previous view matrices overwritten), so while anything moves the realtime denoiser runs spatial-only. Keeping temporal history across object motion needs per-instance motion vectors in the RT path, which do not exist yet. A still scene uploads nothing and keeps accumulating, so the Cornell RT golden is unchanged.

**Path tracer, RT shadows, RT GI.** Hit shaders resolve instance slot, then mesh, then `indexOffset + 3 * gl_PrimitiveID` for vertices, and material as `firstMaterial + mesh.triangleMaterialSlot[gl_PrimitiveID]`. Bindings 4, 5, 8, 9 and 10 are repurposed for the instance, mesh and material data; the instance table is binding 36 and the material references are binding 37. RT GI binds the instance table at its own binding 6 (`InstanceGpu instances[]` in `rt_gi.rchit`). Shaders: `shaders/rt/pt_closesthit.rchit`, `shaders/rt/pt_anyhit.rahit`, `shaders/rt/pt_miss.rmiss`, `shaders/rt/rt_gi.rchit`. RT GI reads `InstanceGpu::giAlbedoPacked.rgb`; every hit counts as a hit (the old alpha was always 1.0). RT GI's bounce light is the brightest light in `RenderScene::lights()`.

**One bindless texture table.** `BindlessTextureManager` is the only texture system. Textures are kept at native resolution; the sampler decodes sRGB. The HDRI is registered in the same table, and its index is stamped into the RT light header through `setEnvMapTextureIndex`. Binding 12 is sized to the manager's capacity (4096, clamped to the device's update-after-bind limits with a log line). The hit and miss shaders treat an index at or past the count as untextured, using the specialization constant `OHAO_BINDLESS_COUNT` (`constant_id = 1`). Materials index this table's slots directly; before the fix, slots past 1024 read unwritten descriptors.

**Legacy quirk kept for RT goldens.** Untextured materials get a 1x1 UNORM tint texture, multiplied as `pow(texel, 2.2)`. This reproduces the old texture array's solid-colour layers, so the base colour is applied twice (see section 7).

**One path-tracer descriptor set per frame slot.** The path tracer allocates `MAX_FRAMES_IN_FLIGHT` descriptor sets and writes only the set of the slot whose fence was just waited (`render` takes the slot; `renderRTPipeline` passes `m_currentFrame`).

**In-frame TLAS.** `RenderScene` keeps a device-local buffer of `VkAccelerationStructureInstanceKHR`, one per slot. Custom index is the slot; released and invisible slots have mask 0. `recordUpdates` records the TLAS on the frame's command buffer:
- create, destroy, slot reuse, mesh change or visibility change: `Build`;
- transform only: `Refit`;
- otherwise: `None`.

The result is in `stats().lastTlasOp`. The TLAS is built from a device address, not from a blocking single-time submit. The post-build barrier reaches compute and fragment stages only when `rayQuery` is enabled.

**Staging and ordering.** Host edits mark entries dirty. `recordUpdates` copies only dirty runs from the per-slot staging buffer into the device-local tables, after a pre-copy barrier. Device-local tables are therefore never written by the host while a frame reads them. The one host-written per-frame buffer is the raster light UBO: there is one per frame slot, the host writes only the current slot's after that slot's fence wait, and the lighting pass binds it in the same slot's descriptor set.

**Goldens.** The deferred goldens were captured on master before the migration (`tests/golden/cornell_box_deferred.png`, `tests/golden/damaged_helmet_deferred.png`). The RT goldens were kept:
- `cornell_box` RT is bit-identical.
- `damaged_helmet` RT was re-captured. The old golden was not lit by the HDRI: the legacy build bound bindless index 0 (a wall's 1x1 solid colour) as the environment map. The new path registers the HDRI. Verified at 1024 spp against master with the old index emulated: differences are max_abs 9 inside the helmet box and 8 outside. The `_recaptured` note is in `tests/golden/manifest.json`.
- Textures are sRGB-decoded by the sampler now, instead of `pow(2.2)` on UNORM. The helmet textures were already sampled at native 2048^2.

**Environment and emissive.** The env map loads at the start of `render()` when its path changes. Emissive materials produce an automatic proxy light (`EmissiveProxyLight`), ported from the old deferred rules.

## 5. Tools, gates and tests

**New test suites**
- `tests/ecs/ecs_tests.cpp` (new, target `ecs_tests`, ctest `EcsTests`). Links `ohao_ecs` and `ohao_gpu_vulkan`; `/FORCE:MULTIPLE /IGNORE:4088` on MSVC. 26 tests: hierarchy and transforms (root and child composition, moving a parent, patch marks dirty, unpatched write detected, the debug update reports it, destroying a parent destroys its subtree, destroying a child detaches it, `registry().destroy` of a parent detaches its children and of a child updates its parent, `registry().clear()` with a hierarchy, reparenting keeps local transform, cyclic parenting rejected, `lookRotation`), system order, creation order, extract (create, update and release slots; destroyed subtree releases slots; unknown mesh skipped), lights (kept without a scene; follow transforms in creation order), spawning (model materials become submeshes and materials; emissive texture yields a proxy light), a patched `GiMaterialLegacy` re-uploads its instance, a World released after an explicit renderer shutdown, and the 10k-instance timing gate (Release only).
- `tests/engine/engine_tests.cpp`: 58 tests. The new `RenderScene`, RT and physics tests include: a rigid body exists without a scene component; a registered mesh gets an AABB, a table entry and a BLAS; static instances upload once and moving ones upload in proportion; missing material refs fall back to the default material; instance-table growth mid-frame defers destroying the old buffers under validation; mesh arena growth keeps meshes intact; the raster shadow map goes to the first Point or Spot light; a material edit reaches the material table pack; legacy RT tint textures are 1x1 UNORM and shared per colour; the path tracer traces the `RenderScene`; texture slots past 1024 reach the path tracer and out-of-range reads as untextured; moving an instance refits the TLAS with the new transform; a slot released and reused in one frame forces a TLAS rebuild; the raster light UBO is one buffer per frame slot; RT accumulation restarts when an instance moves or a light changes.
- `tests/renderer/renderer_test.cpp` and `tests/renderer/CMakeLists.txt`: updated for the new scene path.
- `tests/physics/physics_backend_tests.cpp`: changed for handle- and name-based bodies.

**Timing gate.** `ten_thousand_instances_one_percent_moving_under_half_a_millisecond` builds 10,000 instances, moves 1% per sample for 20 samples, and asserts the median of `TransformSystem` plus extract plus `recordCpuMs` is below 0.5 ms. It prints the median and the worst sample. A worst-case assertion flaked (about 1 in 25) once `render()` recorded 10k draws and four CSM cascades between samples; the gate is now the median.

**Golden script.** `tests/golden/render_golden.py` takes `--only <name>` to run one scene, before or after the mode flag; an unknown name, a missing value or an unexpected argument exits 1 with the usage line. `tests/golden/manifest.json` has four scenes: `cornell_box` and `damaged_helmet` (RT, 16 spp, denoise off) and `cornell_box_deferred` and `damaged_helmet_deferred` (deferred, 4 spp). The deferred helmet entry has `max_abs_diff` 6 and `max_diff_frac` 0.05; the others use `max_abs_diff` 4 and `max_diff_frac` 0.01 (see section 6). A fifth scene, `cornell_box_rt_realtime` (16 spp, denoise off, `max_abs_diff` 10, `max_diff_frac` 0.05), was added with the `rt_realtime` device-loss fix (section 7.1).

**Validation.** `OHAO_VALIDATION=1 build/Release/cornell_box.exe` exits 3 on master and on this branch (see section 7). `tests/validation/check_validation.py` and `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` are the gates named in the spec. Measured on 2026-10-10 on the documentation commit: `check_validation.py` reports `cornell_box deferred: clean` and `damaged_helmet deferred: clean`; the windowed game under validation with a resize at frame 200 exits 0 with `validation including teardown: 0 messages`. Without validation, `ohao_game --frames 600` ran at 177.9 fps (5.62 ms per frame), against the 60 fps target. Suites on the same build: `engine_tests` 56/56, `ecs_tests` 20/20, `runtime_tests` 21/21; all four golden scenes pass (`cornell_box` max_abs 0, `damaged_helmet` 1, `cornell_box_deferred` 0, `damaged_helmet_deferred` 3 of a limit of 6). After the final review fixes (section 7.3), on 2026-10-10: `engine_tests` 58/58, `ecs_tests` 26/26, `runtime_tests` 21/21, the same validation results, `ohao_game --frames 600` at 176.4 fps, goldens `cornell_box` 0, `damaged_helmet` 1, `cornell_box_deferred` 0, `damaged_helmet_deferred` 2.

**Build.** The shaders target is still manual and has no `ALL` dependency (see the runtime-shell change doc). Shaders rebuild with `cmake --build build --target shaders`.

**Example flags.** `model_viewer` gained `--print-bounds`, needed for the converged comparison of the helmet reference.

## 6. Design decisions

From the decision ledger (`progress.md`, "Ruling" lines), one line each with the reason:

1. Deferred helmet tolerance is `max_abs_diff` 6 and `max_diff_frac` 0.05, because the helmet deferred render is not bit-exact run to run (fresh-vs-fresh max_abs 3, measured tail 5), while `cornell_box_deferred` is exact. Migration regressions move pixels by tens of levels, so the gate still catches them.
2. RT light order is creation order (`CreationOrder`), matching `examples/cornell_box.cpp`; this keeps the Cornell RT golden bit-exact.
3. The shadow-caster rule is the legacy one (the first Point or Spot light gets shadow index 0), not the plan's prose; the goldens require it.
4. Implementer adaptations accepted: `createBLAS` `firstVertex`, `LightData` stays in namespace `ohao`, `onFrameRetired` after acquire, a pre-copy barrier in `recordUpdates`, `setEnvMapTextureIndex`. Each was needed for correctness or by a later task.
5. `RenderScene` owns its own `RTAccelerationStructure`, so the legacy `destroy()`/`init()` path cannot wipe or alias its BLASes during the migration window.
6. No BLAS rebuild after arena growth: a built BLAS does not reference its input buffers (FAST_TRACE, no ALLOW_UPDATE), so the rebuild was wasted work. Arena growth also no longer forces a TLAS rebuild.
7. The plan's arena-growth test was replaced by a mid-frame instance-table growth test under validation, because `registerMesh` idles the queue and the plan's test could not fail.
8. `buildTLASFromDevice` returns `bool`, so a failed allocation is not recorded as a Build; the `rayQuery` flag is plumbed so consumer stages are named only when rayQuery is enabled (VUID 06257).
9. `/INCLUDE:vmaCreateAllocator` on `ohao_gpu_vulkan`: NRD's `NRI_VK.lib` carries its own VMA build, and `/FORCE:MULTIPLE` kept the wrong copy (a crash in `vmaCreateAllocator` in `ecs_tests`).
10. `World::addSystemAfter` is needed to place `render_extract` after `transform`.
11. Callers pass the override material wherever the old examples set a `MaterialComponent`. The GI legacy word is `0.73/0.95` only when no override is given, reproducing the old `uploadDeferredTextures` overwrite.
12. Temporary per-frame bridge sync (transforms, visibility, lights) was kept until the bridge was deleted, so `model_viewer`'s `flip` and live old-`Scene` edits kept working during the migration.
13. The 10k timing gate asserts the median, not the worst sample, because a worst-sample assertion flaked about 1 in 25.
14. The helmet RT golden was updated, because master bound bindless index 0 (a wall's tint) as the environment map; the golden was never lit by the HDRI.
15. The legacy RT tint quirk (base colour applied twice through a 1x1 tint texture times `pow(2.2)`) is kept so the Cornell RT golden stays bit-exact; it is a known wrong result (section 7).
16. RT GI reads only `giAlbedoPacked.rgb`. The plan's `.a < 0.5` miss rule would have dropped the metal sphere from GI, because the old buffer always held alpha 1.0.
17. The path tracer's single shared descriptor set was re-graded from Minor to Important and fixed with one set per frame slot, because it was the same VUID 03047 hazard removed from every deferred pass.
18. `SceneFramer::applyLights` moved to `ecs::spawnFramedLights` in `ohao/ecs/framing.*`, so the renderer header names no ECS type (the cross-library split was a link hazard).
19. `model_viewer` uses a multi-material model's own material 0 as the GI override; it feeds only the legacy GI word, and no golden covers it.
20. The turntable ceiling panel is spawned with `lookRotation(down)` and local edges set to the inverse rotation times the old world edges, so the RT edges are unchanged. The old light direction was `(0,-1,0)`, and the raster pass reads that direction.

## 7. Known issues and deferred items

Ledger findings are marked (finding). Minor items are marked (minor). The `minor` list is as recorded in the ledger at commit `d80eb35`, with items addressed by commit `9519b29` noted.

### 7.1 Correctness findings (open)

- **Helmet deferred nondeterminism (finding, high).** The deferred helmet render varies by up to 5 LSB (about 3% of pixels) between runs. Candidate cause: the RT GI frame index seeded from accumulated wall-clock `m_totalTime` in `deferred_renderer.cpp`. Not investigated in 2a.
- **RT double-applies base colour on untextured materials (finding, high).** The tint texture (1x1 texel multiplied as `pow(texel, 2.2)`) and the material colour are both applied. Kept deliberately so `cornell_box` RT stays bit-exact. Fixing it changes the Cornell RT output, so the RT golden must be re-captured after a converged comparison.
- **Validation exit 3 on `cornell_box` (finding).** `OHAO_VALIDATION=1 build/Release/cornell_box.exe` reports VUID 03004 and then NRD aborts on `vkCmdPushDescriptorSet` under the validation layer. Present on master too. The path-tracer engine test runs without validation, so it does not show.
- **Shader rebuild dependency (finding).** The shaders target may not rebuild when `shaders/includes/scene/scene_data.glsl` changes; include dependency tracking is not in place.
- **`rt_realtime` lost the device (fixed on this branch 2026-10-10; pre-existing on master).** `cornell_box.exe out.png 16 rt_realtime` logged `QueueWaitIdle(): failed, result = ... (-4)` (VK_ERROR_DEVICE_LOST) with every denoise setting and wrote an all-black image. Master at `62c1b1c` fails the same way, so 2a did not introduce it. Cause: the C++20 RT refactor on master dropped the descriptor writes for bindings 29-34 (ReSTIR GI reservoir ping-pong) and 35 (specular hit-distance guide), their layout barriers, and the reservoir ping-pong advance. `pt_raygen_realtime.rgen` loads and stores those images every frame; the offline raygen does not declare them, which is why `rt_offline` worked. `PathTracer::render` (`path_tracer_render.cpp`) now writes 29-35 into each frame slot's set, moves the reservoirs and the guide to `GENERAL` (the source stage is the previous frame's ray tracing), and flips the reservoir index each frame. Regression gate: the golden scene `cornell_box_rt_realtime`; with the fix reverted it fails on the uniform-frame check.
- **`rt_realtime` is not deterministic run-to-run (finding).** `pt_raygen_realtime.rgen` reads `accumBuffer` and `normalAOV` at neighbour pixels (temporal reprojection and the in-shader spatial filter) in the same dispatch that writes them at the centre pixel. That is a race inside the dispatch. Two runs differ by up to 67 LSB on about 11% of full-resolution pixels at 4 frames, and by max_abs 3-5 on about 1.5% of pixels at the golden's compare resolution after 19 frames. The `cornell_box_rt_realtime` tolerance (10 / 0.05) is sized for this race. A fix needs separate passes or a ping-ponged accumulation image.
- **NRD and VMA link order (finding).** `/INCLUDE:vmaCreateAllocator` is correct only because of CMake link order. NRI's `ImplVK.obj` binds to the engine's VMA build; a configuration mismatch on the NRD path is possible. Pre-existing in `engine_tests`.

### 7.2 Minor items, grouped

**ECS world and transforms**
- `World::update` iterates the system list by reference; `addSystem` during an update would invalidate the iterator.
- `verifyTransforms` uses an absolute threshold of 1e-4.
- `setParent`, `setTransform` and the recompute path assume the entity has a `Transform`; entities not created by `World::create` are not supported.
- `RenderDirty` and `MovedLastFrame` tag every moved entity, while the header describes renderables only.
- Reparenting gives a one-frame motion-vector jump (`prevWorld` comes from the old parent). It is undocumented and untested.
- `World` move operations are implicitly deleted; the signals hold `this`.
- `setParent` returns `bool` without `[[nodiscard]]`. A non-null invalid parent silently detaches the child and returns true.

**Render extract and spawning**
- The "only the moved one uploads" test is vacuous: it has a single instance.
- The light list is rebuilt twice per move (once for the move, once for the `prevWorld` settle).
- An entity patched from a valid mesh to an unknown one keeps its old slot and mesh until fixed or destroyed. The log-once behaviour has no assertion.

**RenderScene and tables**
- The header's word 8 is fixed at 1.0f instead of `m_envIntensityScale` (disclosed).
- `LightTable::m_shadowsEnabled` is hard-coded to true; the old code read the renderer flag.
- `updateInstance` on a dead slot logs on every call; the spec asks for once per handle. Slots carry no generation; stale detection belongs to the extract.
- Index count is not checked to be a multiple of 3.
- `StagingBatch::upload` guards overflow only with an assert.
- A partial arena growth failure bumps counters and retires the old vertex buffer before refusing.
- `RenderScene`'s acceleration structure is destroyed twice (explicit and in the destructor), causing an extra `vkDeviceWaitIdle`.
- The light test could also cover Directional-first ordering. The legacy-BLAS test declares its scene after the renderer.
- More than 64 material references per instance (`kMaxMaterialRefsPerInstance`) renders the slot with the default material and logs no warning.
- The path tracer writes all 4096 texture descriptors every frame (it could write only on change). `bindRTTextures` rewrites up to 1024 descriptors per frame.
- An empty texture table would leave binding 12 unwritten. VUID 03004 (variable-count binding not highest) remains.
- The new texture-slot test's skip `return`s cut later tests short.
- `path_tracer.hpp:19` has a stale usage comment.
- The legacy tint textures are never freed; they will go away with the tint.
- The RT readback copies a never-rendered `ptOutput` when there is no TLAS (pre-existing).

**Acceleration structures**
- The TLAS and BLAS scratch addresses are not checked against `minAccelerationStructureScratchOffsetAlignment` (safe on the known drivers; validation clean).
- The slot-reuse test does not isolate the structural flag: mask and mesh changes also force a Build.
- There is no periodic TLAS rebuild after long refit chains (trace quality).

**Deferred shading and examples**
- `turntable` writes no PNGs on Windows (`mkdir -p` fails). Pre-existing.

**Golden script (`tests/golden/render_golden.py`)**
- The deferred helmet margin is 1 LSB above the observed tail (6 against 5).

**Resolved on this branch (recorded so they are not re-reported)**
- Stale comments referring to `LightComponent`s in `pt_raygen*.rgen`, `tests/physics/CMakeLists.txt`, `README.md`, and the dead `class Model;` and `<unordered_map>` in `renderer.hpp`: fixed in `9519b29`.
- `ObjectPushConstants` and `kObjectPushConstantsBytes`: deleted in `988be58`.
- Bridge cache, fallback-material registration and snapshotted `MaterialComponent`: died with the bridge in `988be58`.
- Test name `rendering_without_a_scene_keeps_ecs_lights`: renamed in `9519b29`.

**Still stale after `9519b29`**
- `ohao/ecs/spawn.hpp:13` and `ohao/render/scene/model_upload.hpp:10` forward-declare `class Model;` in a namespace block. Not addressed by `9519b29`; harmless.
- README LOC rows for `gpu`, `physics`, `core`, `audio`, `shaders` and the total are not recounted (ledger, `9519b29`).

### 7.3 Final review fixes

The whole-branch review's findings, fixed on this branch after the documentation commit:

- **Raster light UBO per frame slot (important).** It was one host-mapped buffer rewritten every frame while two earlier frames could read it. Now `MAX_FRAMES_IN_FLIGHT` buffers; `renderDeferred` writes the current slot's after its fence wait and binds it in that slot's lighting descriptor set. The RT path no longer writes the raster UBO (nothing in RT reads it). Test: `the raster light UBO is one buffer per frame slot` (engine_tests, under validation, a light moving every frame).
- **Debug transform check (important).** `World::update` runs `verifyTransforms` before the systems in debug builds and logs the first mismatch once per World; `World::transformMismatches()` exposes it. Compiled out in Release. Test: `update_reports_an_unpatched_write_in_debug` (asserts the report in Debug, its absence in Release).
- **RT accumulation on scene change (important).** See section 4, "RT accumulation". Test: `RT accumulation restarts when an instance moves or a light changes`.
- **TLAS publish order.** `buildTLASFromDevice` ensures scratch first and publishes new storage only after the build is recorded; an allocation failure leaves the current TLAS in place.
- **RenderScene lifetime.** `VulkanRenderer::shutdown()` shuts the scene down but keeps the object until the renderer is destroyed, so a World released after an explicit shutdown is safe. Test: `world_outlives_an_explicit_renderer_shutdown`.
- **Raw registry destroys.** `World` connects `on_destroy<Parent>` (the parent forgets the child) and `on_destroy<Children>` (children are detached, not destroyed). `World::destroy` is still the preferred path and takes the subtree. Tests: `registry_destroy_of_a_parent_detaches_its_children`, `registry_destroy_of_a_child_leaves_the_parent_consistent`, `registry_clear_with_a_hierarchy_is_safe`.
- **`GiMaterialLegacy` is tracked.** Its construct and update signals tag `RenderDirty`. Test: `patched_gi_material_reuploads_the_instance`.
- **Golden script arguments** (section 5) and stale comments in `model_upload.cpp`, `render_scene_types.hpp`, `rigid_body.hpp` and `physics_world.hpp`.

## 8. Wiki pages affected

The monograph was refreshed in the documentation commit that follows the implementation commits. The `scene` module now holds `world`, `transform-system`, `render-extract`, `render-scene` and `loaders`; the old `actors` and component units are deleted, and the hand-written hub `site/m/scene.html` describes the ECS world, render extract and `RenderScene`. Units re-derived against the new tree:

- **GPU:** `scene-upload` (RenderScene registration and staged edits), `rt-build`, `bindless`, `buffers-alloc`, `dispatch`, `init`, `layout-meta`, `renderer-facade`.
- **Deferred and graph:** `gbuffer`, `csm`, `lighting`, `culling`, `render-graph`.
- **Path tracer, hybrid, materials, sampling, denoise:** `as`, `bindings` (descriptor map 0-37), `hit-miss`, `host`, `profiles`, `restir`, the hybrid techniques, `pack`, `lights`, `mis`, `env-cdf`.
- **Architecture:** `module-map` (eight archives, one include cycle between GPU and render), `invariants`, `request-path`.
- **Physics:** `backend`, `bodies`, `world`, `debug`, `materials-phys`, `math-constants` (bodies are `shared_ptr<RigidBody>` identified by name; no `PhysicsComponent`; the ECS physics boundary comes in sub-project 4).
- **Testing and status:** goldens, `ecs_tests` and the engine suites, the validation exit 3 and the shaders target.
