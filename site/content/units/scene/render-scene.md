---
module: scene
id: render-scene
title: RenderScene
standard: v2
---

## A persistent render world

`render::RenderScene` is the GPU-side copy of the scene that every pass reads: deferred G-buffer and shadows, the path tracer, RT shadows and RT GI. It is owned by `VulkanRenderer`, has no EnTT dependency, and is persistent — nothing in it is rebuilt per frame. It holds four tables behind one facade:

| Table | Holds | Lifetime of an entry |
|---|---|---|
| `MeshRegistry` | vertex/index arenas, triangle material slots, mesh table, one BLAS per mesh | forever (append-only) |
| `MaterialTable` | 48-byte material packs | forever (append-only) |
| `InstanceTable` | one `InstanceGpu` + material reference list per slot | create → destroy, slot reused |
| `LightTable` | the light list in raster and RT layouts | replaced wholesale per `setLights` |

{{cite ohao/render/scene/render_scene.hpp "// The persistent GPU render world: mesh arenas (one BLAS per mesh), a material"}}

Host-side edits only mark entries dirty. Device-local tables are written exclusively by copies that `recordUpdates` records at the start of a frame's command buffer, from a staging buffer that belongs to that frame slot.

## Mesh arenas, one BLAS per mesh

Every registered mesh is appended to four shared device-local arenas: vertices (the 100-byte `Vertex`), mesh-local `uint32` indices, one material slot per triangle, and a 16-byte `MeshGpu` entry. Draws address a mesh through `vertexOffset`/`firstIndex`; ray-tracing hit shaders through the mesh table. The arenas start large — one million vertices, four million indices — so the vertex arena alone reserves 100 MiB of device memory before the first mesh arrives:

{{cite ohao/render/scene/mesh_registry.cpp "constexpr VkDeviceSize kInitialVertices      = 1u << 20;   // 1 Mi"}}

Registration is load-time work and is synchronous. It validates the mesh (every index below the vertex count, every submesh range inside the index buffer, a triangle-slot list of the right length — padded with slot 0 and logged if not), uploads it through its own staging buffer on a one-shot command buffer, and waits for the queue. An arena that must grow is replaced by one sized to twice the *required* element count, $c' = 2(u + n)$ for $u$ used and $n$ incoming; the old contents are copied on the same command buffer and the old buffer goes to the retire queue, because earlier frames may still be reading it.

The BLAS is built after the copy, against the arena at the mesh's offsets, and is shared by every instance of the mesh. It survives later arena growth untouched:

{{cite ohao/render/scene/mesh_registry.cpp "// A built BLAS (FAST_TRACE, no ALLOW_UPDATE) does not reference its inputs, so"}}

Meshes are never unregistered. Every handle is issued at generation 0, and `info()` rejects any other generation, so a `MeshHandle` is valid for the life of the `RenderScene` or never.

## Material table

A fixed array of 4096 packs, each three `vec4`s: base color and albedo texture index; roughness, metallic, normal and emissive texture indices; rough-metal texture index, emissive strength and two unused lanes. Texture indices are bit-cast into float lanes and point into the one bindless texture table. Index 0 is registered at initialisation as the default material, and is what every unknown or missing material resolves to — including every material registered after the table is full:

{{cite ohao/render/scene/material_table.cpp "return MaterialHandle{0, 0};"}}

The table is append-only and never grows. Uploading a model registers one material per `materialColors` row whether or not an identical pack already exists, so 4096 is a budget on materials *loaded*, not materials distinct.

Beside each pack the table keeps one host-only byte, the material's sidedness, which never reaches the GPU pack: `MaterialDesc::doubleSided` defaults to true, and only glTF materials can be one-sided — the loader reads Assimp's `TWOSIDED` key, which its glTF importer always sets from glTF's `doubleSided`, into `Model::materialDoubleSided`. Scene-file, OBJ and FBX materials stay double-sided. An index the table does not hold reads as double-sided:

{{cite ohao/render/scene/material_table.hpp "return index >= m_doubleSided.size() || m_doubleSided[index] != 0;"}}

## Instance table: slots, free list, material references

An instance is one 160-byte `InstanceGpu` — `world`, `prevWorld`, mesh index, material-reference offset and count, a visibility flag, and the legacy RT GI albedo word — plus a fixed block of 64 material indices at `slot * 64` in a second buffer. Submesh $k$ of instance $s$ shades with

$$m(s,k) = \begin{cases} \text{refs}[\,64s + k\,] & k < \text{count}_s \\ 0 & \text{otherwise} \end{cases}$$

which is how a `MeshRenderer` with fewer materials than submeshes falls back to the default material without ever reading past its block. The same rule is the shader macro every consumer uses:

{{cite shaders/includes/scene/scene_data.glsl "((slot) < (inst).materialRefCount ? (refs)[(inst).materialRefOffset + (slot)] : 0u)"}}

Material handles are bounds-checked when written: an index at or past the table's count becomes 0. Lists longer than 64 are truncated with a one-time log.

Slots come from a free list that always returns the lowest released slot, and the table grows by doubling from 1024. Every write builds the new `InstanceGpu` and compares it with the current bytes; only a difference marks the slot dirty, and the material block is compared and marked separately. Uploads coalesce dirty slots into contiguous runs, one copy per run. Destroying an instance clears its flags, so the GPU copy reads as invisible until the slot is reused.

## The draw table

`draws()` — the raster draw list, one entry per visible instance × submesh, in slot order — is rebuilt only when the table reports a change that alters it: a create, a destroy, a visibility or mesh change, or host storage growth. The last one is in the list because each `Draw` holds a pointer into the host instance array, which growth reallocates. Each `Draw` also records whether its submesh's material is one-sided, resolved through the instance's material list the way the shader resolves it, and the submesh's meshlets in the meshlet arena (first and count, from `MeshInfo::submeshMeshlets`).

Every rebuild bumps `drawsVersion()`, and `recordUpdates` uploads the whole list, when its version differs from the uploaded one, as the draw table: one 64-byte `gpu::shared::DrawRecord` per draw (index range, vertex offset, instance slot; material slot, the one-sided and emitter flags, first meshlet and meshlet count; the mesh's local box), in a device-local buffer that grows by doubling and retires the old one:

{{cite ohao/render/scene/render_scene.cpp "(d.oneSided ? gpu::shared::kDrawOneSided : 0u) | (d.emitter ? gpu::shared::kDrawEmitter : 0u),"}}

An emitter draw is a light's mesh, and the cascade lists leave it out: a light's sphere would
otherwise cast the sun's shadow.

{{cite shaders/cull/instances.slang "member = inFrustum && (d.info.y & kDrawEmitter) == 0u;"}}

The box's spare `w` carries one more number, as a uint's bits: the draw's first *meshlet entry*, the meshlets of every draw before it. `meshletEntryCount()` is the total, what the mesh-shader fallback culls one entry a thread:

{{cite ohao/render/scene/render_scene.cpp "r.aabbMin = {d.aabbMin.x, d.aabbMin.y, d.aabbMin.z, std::bit_cast<float>(entries)};"}}

It is what GPU culling reads, and what the G-buffer and CSM vertex shaders index with `firstInstance`. `drawCount()` reports the uploaded list's length, and zero while the current version has not been uploaded yet, so no pass draws from a table that does not match the list:

{{cite ohao/render/scene/render_scene.cpp "return m->uploadedVersion == m->drawsVersion ? static_cast<uint32_t>(m->draws.size()) : 0u;"}}

## Lights: one list, two layouts

`setLights` stable-sorts the list by `order` and resolves it once into both consumers' formats. The raster array keeps up to 65,536 `LightData` entries in a host copy, which the deferred renderer's `LightClusters` uploads into each frame slot's light table, and a version (`rasterLightsVersion()`) that changes only when the array's bytes do, so an unchanged list is not uploaded again. The first eight shadow-casting raster lights get RT shadow channels; casters past them render unshadowed and are counted (`shadowCastersWithoutChannel()`). The RT buffer holds a 16-byte header — count, environment-map texture index, environment intensity — and up to 64 `GPULight`s. In both, emissive proxies are placed after all regular lights, so emissive models cannot crowd authored lights out of either cap. The RT bytes are compared with the previous upload and only re-uploaded when they differ.

{{cite ohao/render/scene/render_scene.hpp "// Changes whenever rasterLights() does (clustered lighting uploads only then)."}}

`setLights` also gives every point, spot and area light a body: an *emitter instance*, a
unit sphere scaled to the light's radius or a unit quad mapped onto its rectangle, with the
light's radiance in the instance's `giAlbedoPacked.rgb` and the `kInstanceEmitter` flag. A
point or spot light's sphere shines with `I / (pi r^2)`, so from afar it is the source of
intensity `I` that lights the scene; an area light's rectangle with the path tracer's own
radiance for it:

{{cite ohao/render/scene/light_table.cpp "e.radiance = d.color * (d.intensity / (kPi * r * r));"}}

The instances are ordinary slots, reused in light order, so re-sending an unchanged list
rewrites identical bytes and uploads nothing; `setLightEmitters(false)` removes them. The
TLAS gives them their own mask, which only the path tracer's camera ray passes
(hybrid/visibility): a mesh around a light would otherwise shadow it, and a bounce that
reached it would count the light twice.

## Staging per frame slot, and the retire queue

`recordUpdates` sums every table's pending bytes, makes sure the current frame slot's host-visible staging buffer is at least that large, and records the copies between two global barriers: one ordering them after earlier frames' shader reads and copies, one ordering every shader stage — and the acceleration-structure build — after them.

{{cite ohao/render/scene/render_scene.cpp "// One host-visible staging buffer per frame slot: the slot's fence guarantees"}}

One staging buffer per frame slot, not one shared, because the renderer waits on the slot's fence before calling `recordUpdates`: the buffer it is about to overwrite is the one the GPU finished with `MAX_FRAMES_IN_FLIGHT` frames ago. A buffer replaced mid-flight — a grown arena, a grown instance table, TLAS storage or scratch — is not destroyed but stamped with a retire frame:

{{cite ohao/render/scene/render_scene.cpp "retireQueue.push_back(Retired{frameCounter + MAX_FRAMES_IN_FLIGHT, b, {}});"}}

`onFrameRetired` advances the counter once per frame, after the slot's fence wait, and destroys every entry whose frame has come. With three frames in flight, a replaced buffer outlives every command buffer that could have referenced it.

## TLAS: build versus refit

The TLAS spans every slot below the instance table's high-water mark. Each slot's `VkAccelerationStructureInstanceKHR` is kept in a host mirror and in a device buffer; the instance custom index is the slot, and a released or invisible slot gets mask 0 so rays skip it. Converting the column-major world matrix to Vulkan's row-major 3×4 is a transpose and a 48-byte copy.

{{cite ohao/render/scene/render_scene.cpp "vi.instanceCustomIndex = slot & 0xFFFFFFu;   // 24 bits"}}

Every instance change restages that slot and classifies it. A create, a destroy, a changed mask or a changed BLAS reference is *structural*; a changed transform alone is not. After the frame's copies, `recordTlas` builds if anything structural happened or no TLAS exists yet, refits if only transforms changed, and records nothing for a still scene:

{{cite ohao/render/scene/render_scene.cpp "const bool build = tlasStructural || rtAccel->getTLAS() == VK_NULL_HANDLE;"}}

Both are recorded into the frame's command buffer; nothing is submitted separately. A refit keeps the tree topology of the last build and only re-fits its bounds, so it is cheap but its quality decays as instances travel away from where the build placed them. Nothing here schedules a periodic rebuild: an instance that moves every frame and nothing else — `ohao_game`'s spinning cube — is refit indefinitely until some structural change forces a build. A failed allocation leaves the instance data uploaded and sets the structural flag, so the next frame's build covers what this frame missed. `stats().lastTlasOp` records which of build, refit or nothing the last frame took, and the engine tests assert each case.

:::key
The RenderScene's correctness rests on one discipline: no device-local table is ever written from the host. Every write is a recorded copy from staging owned by the current frame slot, and every buffer a previous frame could still read is retired, not destroyed. That is what lets instances move every frame while up to three frames are in flight, without per-frame descriptor rewrites or a blocking upload.
:::

## Contracts

- Call `recordUpdates` at the start of the frame's command buffer, after the slot's fence wait and after the ECS update; bind the scene's buffers *after* it, because table growth and TLAS reallocation replace buffer handles.
- `registerMesh` is blocking and idles the queue. It belongs at load time; calling it per frame stalls the GPU.
- The custom index keeps 24 bits of the slot; slots at or above $2^{24}$ alias. Nothing enforces that bound.
- `materialIndexFor(slot, k)` mirrors the shader's fallback on the host, so CPU-side code resolves a submesh's material exactly as the GPU will.
- Draw $i$ of one draw-table version is not draw $i$ of another. Anything that keys per-draw state by index — GPU culling's visibility history — must drop it when `drawsVersion()` changes.
