---
module: reflection
id: instantiate
title: Instantiate & the scene view
standard: v2
---

## Meshes as data

A scene file cannot hold a `MeshHandle`. A handle is an index into one `RenderScene` on one GPU, and it means nothing in the next process. So the saved form of "this entity has a mesh" is a description of where the mesh comes from:

{{cite ohao/ecs/components.hpp "std::variant<ModelAsset, SpherePrim, CubePrim, PlatformPrim, CylinderPrim, ConePrim, QuadPrim, BoxPrim> source;"}}

`Material` (OpenPBR's base: base colour, metalness, specular roughness, IOR, emission, ...) sits beside it. The `instantiate` system turns the pair into the runtime components the render extract reads: `MeshRenderer` and, when the model's first emissive-textured material is bright enough to make an auto-light, `EmissiveProxyLight`. None of these is saved. An entity whose `MeshRenderer` was built in code from an in-memory model has nothing a file can describe, and `saveScene` reports it as `W_UNSAVEABLE_MESH` instead of dropping the mesh silently.

## The backend seam

Instantiate does not include the renderer's upload code directly. It talks to a four-method interface:

{{cite ohao/ecs/instantiate.hpp "class InstantiateBackend {"}}

`uploadMesh` and `releaseMesh` handle geometry. `modelMaterials` registers a model's own materials with their textures. `material` registers one descriptor. The production `RenderSceneBackend` forwards these to `render::uploadModelMesh`, `RenderScene::releaseMesh` and `registerMaterial`, the same path `spawnModel` uses. `material` registers the descriptor as given, adding no texture to it.

The seam is what makes the logic testable without a GPU. In `scene_tests`, a `FakeBackend` hands out sequential handles and counts every upload, release and registration, and the t11 tests assert those counts directly. The GPU half is `instantiate_gpu_test`. It renders the Cornell box twice with fresh renderers, once built through `spawnModel` and once from `MeshSource`/`Material` entities through instantiate on a real `RenderScene`. The two images must be bit-identical, and with validation layers on there must be no messages. Then it destroys the spheres and checks that their shared mesh is released.

## Triggers and one run

Constructing or updating `MeshSource` or `Material` tags the entity `InstantiateDirty`. Destroy signals only queue the entity, because it may be halfway through being destroyed:

{{cite ohao/ecs/instantiate.cpp "void onSourceDestroy(entt::registry&, entt::entity e) { m_sourceGone.push_back(e); }"}}

Each `run()` does three things in order. First, every entity whose `MeshSource` went away gives up its use of the mesh; if the entity still lives, what instantiate made is removed from it. Second, an entity that lost only its `Material` is re-dirtied, so it falls back to the default material. Third, the dirty entities are sorted by `CreationOrder` and instantiated, and the tag is cleared from every entity. The sort makes mesh registration order, and so mesh indices, independent of EnTT pool order. The write rule is the same as for the render extract: a change through `get<>()` fires no signal and is never seen; use `patch` or `replace`.

## One mesh per distinct value

Meshes are shared by value, not by entity. The cache key is the variant's alternative index followed by the raw bytes of the primitive's parameters, or, for a model, its path and GUID:

{{cite ohao/ecs/instantiate.cpp "static_assert(sizeof(SpherePrim) == 12 && sizeof(CubePrim) == 4 && sizeof(PlatformPrim) == 12 &&"}}

The `static_assert` is the guard: keying on bytes is only sound if the primitive structs have no padding, since padding bytes are indeterminate. Two consequences follow from a byte key:

- Two values that compare equal but differ in bits get two meshes. `-0.0` and `0.0` are an example.
- A model is keyed by its path exactly as written. `FileAssetResolver` caches loaded models by *normalized* path, so `props/a.glb` and `props/./a.glb` share one loaded `Model` but are uploaded as two meshes.

Each cache entry counts its users. An entity switched to a source that already has a mesh reuses it, with no upload and no release. The last user to leave releases the mesh:

{{cite ohao/ecs/instantiate.cpp "if (it == m_meshes.end() || --it->second.users > 0) return;"}}

`RenderScene::releaseMesh` marks the mesh released and retires its BLAS after `MAX_FRAMES_IN_FLIGHT` frames, because frames still in flight may trace it. Per the step-7 record, the mesh index is never reused and arena space is not reclaimed yet. When the world is destroyed, the system's destructor releases every mesh it still holds.

## Materials by value, for a reason

`MaterialTable` has a fixed capacity of 4096 entries and no way to free one. When it is full it logs once and returns the default material. Registering a material per entity would fill it in a 10k-entity scene, and every edit would use up another slot. So scene materials are deduplicated on the bit patterns of every value they set (OpenPBR's base, emission, subsurface, alpha mode and opacity: 19 words; until 2026-10-29 seven, so two materials differing only in alpha shared an entry), in a map that lives as long as the system:

{{cite ohao/ecs/instantiate.cpp "for (std::size_t i = 0; i < key.size(); ++i) key[i] = std::bit_cast<uint32_t>(values[i]);"}}

A model that has its own materials always uses them; a `Material` beside it is ignored (t11 checks it). Model materials are cached in the mesh entry, not in that map. If a model's last user goes and the model is instantiated again later, its materials are registered a second time and take more table slots.

## Model files through the resolver

`AssetResolver` has three methods. `loadModel` returns a shared `Model` or an `AssetError`. `exists` never loads anything; `checkAssets` validation uses it. `absolutePath` returns an empty path for a reference it rejects. `FileAssetResolver` normalizes each path lexically against an absolute project root. It rejects empty, absolute, drive-rooted and root-relative paths, and any path whose first component after normalization is `..`, so `assets/../../x.glb` is rejected as well:

{{cite ohao/ecs/asset_resolver.cpp "return {};   // leaves the project"}}

A rejected or missing file is `E_ASSET_NOT_FOUND`. An extension `ModelLoader` does not accept, or a loader failure, is `E_ASSET_INVALID`. The loader accepts obj, fbx, glb, gltf, dae, blend, 3ds and ply, and the error message names all eight. Instantiate adds `E_MESH_INVALID` for a model with no triangles or a failed upload. A failure clears everything instantiate put on the entity and leaves `InstantiateError {code, message}` on it. Each distinct entity/code/message triple is logged once, and `instantiateErrors(world)` lists the failing entities in creation order. A failure is not retried on its own, because the dirty tag is cleared. Once the file exists, patching the entity's `MeshSource` re-instantiates it and clears the error.

## Its place in the update

`World`'s constructor registers `spin` then `transform`. Instantiate inserts itself before `transform`, and the render extract inserts itself after it. The order of one update is therefore spin, instantiate, transform, render_extract:

{{cite ohao/ecs/world.cpp "// Structural: transforms are propagated before every host system (instantiate is"}}

The order is what makes a new mesh appear in the frame its `MeshSource` appears. Emplacing a `MeshRenderer` fires the extract's construct signal, which tags the entity `RenderDirty`, and the extract runs later in the same update and gives it an instance slot. If instantiate ran after the extract, every new or edited mesh would be one frame late. A second `installInstantiate` on the same world logs and is ignored; a flag in the registry context guards it.

The system also works on a world that already has entities. Its constructor tags every existing `MeshSource`, and the render extract's constructor does the same for existing `MeshRenderer`s:

{{cite ohao/ecs/instantiate.cpp "for (const entt::entity e : m_reg.view<MeshSource>()) m_reg.emplace_or_replace<InstantiateDirty>(e);"}}

`ohao run` depends on this. It loads the scene with no GPU, and installs the extract and instantiate only on the first `render.capture`.

## The scene view

`Camera` and `Environment` are one per world. Validation and commands reject a second one with `E_SINGLETON`, even an inactive one. The registry itself does not enforce this, so if a world built in code holds two, `resolveSceneView` takes the first in creation order. It returns plain values; `ohao_ecs` never sees the renderer's camera class.

An inactive camera resolves to nothing, and the host keeps its own camera. An active one takes its pose from the entity's world matrix $W$. Its position is the translation column, and its view direction is the entity's $-Z$ axis. The renderer's camera is parameterized by pitch and yaw only, so the direction is converted to those two angles:

$$\mathbf d = -W_{\cdot,2}, \qquad \text{pitch} = \arcsin\frac{d_y}{\lVert\mathbf d\rVert}, \qquad \text{yaw} = \operatorname{atan2}(d_z,\, d_x)$$

Here $W_{\cdot,2}$ is the third column of $W$. Looking down $-Z$ gives exactly (0°, −90°). Roll cannot be expressed in pitch and yaw and is dropped. The length of $\mathbf d$ does not matter, because it is normalized. The angles are computed in double and rounded once, so axis-aligned directions give exact angles. `-0` is turned into `+0`, because a negative-zero pitch would reach the renderer as a negative-zero `front.y`:

{{cite ohao/ecs/scene_view.cpp "const glm::vec2 py = pitchYawDegrees(-glm::vec3(w[2]));"}}

$W$ is not read from `WorldTransform`. It is rebuilt from `Transform` and `Parent`, root first, with the transform system's own arithmetic, because a host may apply the view before the world has ever been updated. `t20_camera_pose_comes_from_the_world_transform` checks with `memcmp` that the position matches `WorldTransform` bit for bit, both before and after an update.

## Applying it: `applySceneView`

The host half is `runtime::applySceneView`, header-only in `ohao/runtime/scene_view_apply.hpp`, and `cornell_box`, `model_viewer`, the shared capture procedure (`runtime::captureFrame`, behind the CLI's render host) and the window's `LiveHost` all call it; the live host does so before every frame it renders. It sets the renderer camera's position, its perspective (the field of view and planes come from the component, and the aspect ratio stays the render target's) and its pitch and yaw. For the environment it resolves the HDRI through the same resolver and sets the intensity scale:

{{cite ohao/runtime/scene_view_apply.hpp "renderer.setEnvironmentMap(path.string());"}}

An HDRI path the resolver rejects becomes a warning, not an error. Removing the HDRI does not unload a map the renderer already holds, because the renderer has no unload. `applySceneView` also does not reset path-tracer accumulation. A host that changes the view between frames has to do that itself; the CLI's render host restarts accumulation on every capture after the first, and the live host on every lockstep capture.

## Contracts

- The `RenderScene`, the texture manager and the resolver must outlive the `World`. The system's destructor releases meshes through the backend.
- `MeshSource` and `Material` changes reach instantiate only through `patch`, `replace`, `emplace` or `remove`.
- A failed entity stays failed until its `MeshSource` is touched again.
- A rolled `Camera` entity renders unrolled.
