---
module: gpu
id: scene-upload
title: Scene upload
standard: v2
---

## Two speeds: load-time registration, per-frame staging

`render::RenderScene` is the only place geometry, materials, instances and lights turn
into GPU state, and it moves at two speeds. *Registration* — a mesh, its textures, its
materials — is load-time work: it records into a private one-shot command buffer,
submits, and drains the queue before it returns.

{{cite ohao/render/scene/mesh_registry.cpp "vkQueueWaitIdle(m_ctx.queue);"}}

*Edits* are frame-time work. `createInstance`, `updateInstance`, `destroyInstance` and
`setLights` touch only host copies and mark entries dirty; nothing reaches the device
until the next frame's command buffer calls `recordUpdates`, which stages the dirty
rows through a host-visible buffer owned by that frame slot and records one copy per
contiguous run.

{{cite ohao/render/scene/render_scene.cpp "void RenderScene::recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot) {"}}

So moving a mesh uploads one 160-byte `InstanceGpu` and refits the TLAS; it stalls
nothing, because the slot's fence has been waited and the device-local table is only
ever written by copies recorded in the same command buffer that will read it. The
raster light table is the one exception: `LightClusters::setLights` copies the light
table's host array straight into that frame slot's persistently mapped storage buffer,
with no staging at all, and only when the table's version differs from the one the slot
holds. One buffer per slot, written after the slot's fence wait, is what makes the
direct write safe.

{{cite ohao/render/deferred/light_clusters.cpp "void LightClusters::setLights(uint32_t slot, std::span<const LightData> lights, uint64_t version) {"}}

The version is the light table's own counter, raised whenever `set` produces a raster
array that differs from the last one byte for byte, so re-sending an unchanged light
list each frame uploads nothing:

{{cite ohao/render/scene/light_table.cpp "++m_rasterVersion;"}}

:::key
Nothing in the render world is torn down and rebuilt. Growth is the only thing that
replaces a buffer, and a replaced buffer waits `MAX_FRAMES_IN_FLIGHT` frames in a
retire queue before it is destroyed, so the frame path never idles the device.
:::

## One arena per stream, mesh-local indices

Every registered mesh is appended to four shared buffers: a vertex arena, an index
arena, a per-triangle material-slot stream and a mesh table of 16-byte `MeshGpu`
entries. Three more take its submeshes' meshlets: the 48-byte `MeshletGpu` records,
their vertices and their packed local triangles (the meshlets unit's subject). A mesh
records its slice as element offsets — `vertexOffset`, `indexOffset`,
`triangleSlotOffset` — not byte offsets. The whole registration travels in one staging
buffer, laid out vertices, indices, slots, entry, then the three meshlet streams:

{{cite ohao/render/scene/mesh_registry.cpp "// vertices | indices | slots | entry | meshlets | meshlet vertices | meshlet triangles."}}

An arena that is full is replaced by one sized at twice what is required, the old
contents are copied across on the same command buffer, and the old buffer is pushed to
the caller's retire list rather than destroyed.

{{cite ohao/render/scene/mesh_registry.cpp "const VkDeviceSize newCapacity = required * 2;"}}
{{cite ohao/render/scene/mesh_registry.cpp "retired.push_back(a.buf);"}}

The indices in the arena stay *mesh-local*: they are not biased by the running vertex
offset as they are appended. The bias is applied by whoever reads them, and there are
three readers that must agree. The rasteriser's draws are indirect: GPU culling copies
the mesh's `vertexOffset` from the draw record into each command's base-vertex field,
beside the index range, with `firstInstance` the draw's index in the draw table. The
G-buffer and the cascaded shadow pass both draw such lists; the shadow pass adds a
64-byte push constant, the cascade's view-projection.

{{cite shaders/cull/instances.slang "commands[at + 3u] = d.draw.z;"}}
{{cite ohao/render/deferred/csm_pass.hpp "static_assert(sizeof(ShadowPushConstant) == 64);"}}

The BLAS build gets the same offset as its `firstVertex`, with the buffer base and a
byte offset for the mesh's slice:

{{cite ohao/render/scene/mesh_registry.cpp "mesh.vertexOffset,       // arena indices are mesh-local"}}

and the closest-hit shader adds it by hand when it fetches a triangle's corners:

{{cite shaders/rt/pt_closesthit.rchit "uint i0 = mesh.vertexOffset + idx[baseIdx + 0u];"}}

The coupling is the point. Three consumers each apply the bias in their own idiom, and
a change to how indices are stored — biasing at upload, say — would be correct for
none of them until all three were edited, and wrong for any one left behind.

## Geometry that is raster input, RT input and a storage buffer at once

The arenas are device-local, created `GpuOnly` and never mapped. Their usage mask is
what lets one buffer serve every reader: it is a vertex or index buffer for the
raster passes, a storage buffer for the hit shaders, and acceleration-structure build
input with a device address for the BLAS builder.

{{cite ohao/render/scene/mesh_registry.cpp "VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;"}}

:::why
A mesh is resident once. The textbook layout keeps a raster vertex buffer and a
separate device-local copy for ray tracing — two allocations per mesh and a
re-upload whenever either changes. A shared arena pays instead in growth: when a
stream fills, the whole arena is copied into a larger one. Built BLASes survive that,
because a BLAS built without the update flag does not reference its inputs after the
build. What it gives up is per-mesh release: arenas only append, so a mesh's range is
not reclaimed while the scene lives.
:::

## Textures are uploaded into a description, not into the scene

`uploadModel` is the single entry point that turns a loaded `Model` into render-world
state. It registers the mesh (one BLAS), pushes each material's maps into the bindless
manager, and registers one `MaterialDesc` per material. The texture indices come back
into the descriptions it is about to register; no scene object is mutated, and the
returned `UploadedModel` is the only record of what was created. Materials come from
the model's `materialColors` — one per entry — with scalar roughness read from the
colour's `w` and metallic from the parallel array, falling back to `0.0f` when that
array is shorter:

{{cite ohao/render/scene/model_upload.cpp "mats[i].metallic = i < model.materialMetallic.size() ? model.materialMetallic[i] : 0.0f;"}}

A model with no `materialColors` gets a single material: the caller's override, or the
default. A material that loaded an emissive map emits at `KHR_materials_emissive_strength`
(1 without it); the strength does not scale with the map.

{{cite ohao/render/scene/model_upload.cpp "mats[i].emissiveStrength = i < model.materialEmissionStrength.size() ? model.materialEmissionStrength[i] : 1.0f;"}}

Submeshes are the contiguous runs of equal `materialPerTriangle` entries, capped at the
triangle count; triangles past the end of that list use material 0. Each run becomes
one draw.

The rough/metal branch carries a comment describing a repack and performs a copy: R
to R, G to G, B to B, alpha forced to 255.

{{cite ohao/render/scene/model_upload.cpp "repacked[p * 4 + 1] = td.pixels[p * 4 + 1];"}}

An identity copy with an opacity fix. Not wrong — the layout it keeps (R unused by
glTF, or occlusion in a packed image; G=roughness, B=metallic) is the one the source
already has — but the loop is the only thing implying a conversion happens.

Occlusion is the last kind uploaded. glTF's `occlusionTexture` is usually the same image
as the metallicRoughness map (packed ORM, as FlightHelmet's); the loader marks that case
(`Model::kOcclusionIsRoughMetal`) and the material's occlusion index is set to its
roughness-metal texture instead of a second upload:

{{cite ohao/render/scene/model_upload.cpp "if (model.materialOcclusionTexIndex[mi] == Model::kOcclusionIsRoughMetal) mats[mi].occlusionTex = mats[mi].roughMetalTex;"}}

Beyond that one case nothing deduplicates textures: each material uploads its own images, so a model
whose materials share a pooled image uploads it once per material. Each upload takes a
slot out of the manager's fixed table of 4096, and none is released while the scene
lives.

## An emissive texture, promoted to a light

A model that declares an emissive texture may have a light synthesised for it, with
the intensity derived by reading the texture on the CPU. Write $R_p, G_p, B_p$ for a texel's
channels in $[0,1]$, $Y(p)$ for Rec. 709 relative luminance, $S$ for the texels bright enough
to count and $P$ for their summed luminance:

$$Y(p) = 0.2126\,R_p + 0.7152\,G_p + 0.0722\,B_p, \qquad S = \{\,p : Y(p) > 0.05\,\}, \qquad P = \sum_{p \in S} Y(p)$$

$$I = \min(0.1\,P,\;20), \qquad c = \frac{1}{|S|}\sum_{p \in S}\big(R_p, G_p, B_p\big)$$

Two gates decide whether the light exists at all, and neither is the per-texel $Y(p) > 0.05$
that defines $S$. The first is $P > 0.1$ on the whole texture: below it the scan runs, costs a
full texture read, and produces nothing.

{{cite ohao/render/scene/model_upload.cpp "if (totalPower > 0.1f) {"}}

The second is a return at the foot of the per-material loop, placed *outside* the power
test, so only the first material carrying a non-negative emissive index is ever
examined. A model whose second material is the real emitter gets nothing.

{{cite ohao/render/scene/model_upload.cpp "return std::nullopt;   // one light per model: only the first emissive material counts"}}

The colour $c$ is a *mean* over $S$ and is independent of texture resolution. The intensity
$I$ is a *sum* and is not: the same emitter authored at 2K instead of 1K feeds roughly four
times the power into the same formula, and shifts the $P > 0.1$ gate by the same factor. The
clamp at 20 hides the intensity half of that for most textures; nothing hides the gate.

{{cite ohao/render/scene/model_upload.cpp "light.intensity = std::min(totalPower * 0.1f, 20.0f);"}}

The light's geometry is half model space and half not. The proxy carries a model-space
centre — the midpoint of the vertex bounding box — and a radius of 0.3 times the
model-space diagonal.

{{cite ohao/render/scene/model_upload.cpp "const float radius = glm::length(bmax - bmin) * 0.3f;"}}

When the render extract resolves it into a world-space light, the centre goes through the
entity's world matrix and the radius does not:

{{cite ohao/ecs/render_extract.cpp "d.position = glm::vec3(wt.world * glm::vec4(p.localCenter, 1.0f));"}}

Scaling an emissive entity moves its light correctly and leaves its size behind. The proxy
is not a GPU upload by itself: it becomes an `EmissiveProxyLight` component, then a
`LightDesc` of a special kind that the light table lowers twice — into the deferred
`LightData` array after the regular lights, as a point light with a range of ten radii,

{{cite ohao/render/scene/light_table.cpp "light.direction = glm::vec4(0, -1, 0, d.radius * 10.0f);"}}

and into the path tracer's `GPULight` buffer as a sphere light.

## The HDRI is its own upload

The environment map's bindless index rides in the light buffer's 16-byte header, alongside
the light count and an intensity scale. The upload does not run inside the light path:
`render()` runs it once per requested path, before choosing a frame body, and drains the
device first when a different panorama is about to join the bindless table. It runs again
when a directional light comes or goes, the one thing about lights the environment depends
on: a directional light stands for the sun, so while there is one the map is uploaded with
its own sun painted out, and the sun is counted once.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!m_envMapPath.empty() && (m_envMapPath != m_envMapLoadedPath || sunless != m_envMapLoadedSunless))"}}

The sun is the brightest texel's connected region over 64 times the median sky, when the
peak is at least $10^4$ times it and the region at most 0.02 sr; its texels take the mean of
the sky bordering it. RT GI's cosine rays met the outdoor HDRI's sun, half the map's power on
0.003% of the sphere, about once in $10^4$ rays and brought back tens of thousands: fireflies.

{{cite ohao/render/rt/env_sun.cpp "if (!(sun.median > 0.0f) || sun.peak < kSunPeakOverMedian * sun.median) return sun;"}}

A scene with no lights and no emissive meshes therefore still gets its environment map:
`env_demo --lighting=none` loads the HDR and creates no light at all, and the HDRI is
still sampled by the path tracer's miss shader and the deferred sky reflections.

{{cite examples/env_demo.cpp "// LightingMode::None → no lights, HDR env IBL only (pure environment)"}}

The loader is guarded by a path cache: when the requested path matches the one already
loaded, the function skips the decode and the CDF build and only re-stamps the index
and the scale into the render scene. Without it every call would re-decode and
re-upload a multi-megabyte HDR, so a caller that changed the path every frame would turn
a scene-load cost into a per-frame one.

{{cite ohao/gpu/vulkan/env_upload.cpp "float* hdrPixels = stbi_loadf(m_envMapPath.c_str(), &ew, &eh, &ec, 4);"}}

The image is `R32G32B32A32_SFLOAT` — 16 bytes per texel, so a 4096×2048 panorama is
128 MB resident plus an equal host staging buffer.

{{cite ohao/gpu/vulkan/env_upload.cpp "imgInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;"}}

The renderer owns the image and its memory and frees them in one place,
`destroyEnvMapImage`, at shutdown and before loading a different panorama; the view
belongs to the bindless table once registered. The replacement path idles the graphics
queue first, because frames still in flight may be sampling the old image.

{{cite ohao/gpu/vulkan/env_upload.cpp "// Replacing an earlier env map: frames in flight may still sample it."}}

The last step hands the render scene the slot to stamp into its light header, or the
no-texture sentinel when nothing loaded:

{{cite ohao/gpu/vulkan/env_upload.cpp "m_renderScene->setEnvMapTextureIndex(envMapLoaded() ? m_envMapTexIdx : render::kNoTexture);"}}

## Contracts

- Mesh indices are arena-local. Every reader applies `MeshInfo::vertexOffset` itself — as the indirect draw command's base vertex, as the BLAS `firstVertex`, and as an addend in the hit shaders. Change how one reads them and the other two silently break.
- Offsets in `MeshInfo` and `MeshGpu` are element counts, not bytes; byte offsets are computed at the point of use.
- `registerMesh` blocks on the queue and belongs to load time. Per-frame changes go through `createInstance`/`updateInstance`/`destroyInstance`/`setLights` and reach the device in `recordUpdates`.
- A replaced arena or table buffer is retired, not destroyed; it is freed `MAX_FRAMES_IN_FLIGHT` frames later by `onFrameRetired`. Calling that tick more or less often than once per submitted frame breaks the lifetime guarantee.
- `uploadModel` consumes slots in a 4096-entry bindless table and releases none; every upload of a model spends its materials' texture slots again.
- The emissive proxy's centre is world-transformed and its radius is not.
- The environment map reaches the GPU independent of lights. Its slot is stamped into the RT light header by `setEnvMapTextureIndex`, so loading a different path changes the header on the next frame.
