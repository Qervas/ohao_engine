---
module: gpu
id: scene-upload
title: Scene upload
standard: v2
---

## One walk, and everything downstream

`updateSceneBuffers` is the only place a `Scene`'s *geometry* turns into GPU state, and it is
not incremental. It stalls the device — the buffers it is about to free may still be
referenced by in-flight command buffers — destroys the vertex and index buffers, rebuilds
them from every visible actor, then rebuilds acceleration structures, RT material rows,
bindless textures and the RT light SSBO on top, and finally re-derives the host-side emissive
light cache the deferred pipeline reads.

{{cite ohao/gpu/vulkan/scene_upload.cpp "vkDeviceWaitIdle(m_device);"}}

Lights are outside that deal. `updateLightBuffer` re-walks the actor map once per rendered
frame, reading each `LightComponent`'s position, direction, colour, range and cone angles
straight into the deferred light UBO; the RT SSBO is refreshed by
`updateRTLightParams`, which never enters this function. Move a light and the deferred
pipeline needs no upload at all. Move a mesh and all of the above runs.

{{cite ohao/gpu/vulkan/buffer_setup.cpp "void VulkanRenderer::updateLightBuffer(uint32_t frameIndex) {"}}

Every GPU-side step past the geometry copy hangs off `buildAccelerationStructures`, which
returns before doing any of it when the device has no ray-tracing support. There the RT
material rows, the BLAS/TLAS build, the bindless texture upload and the light SSBO never
happen — and since `uploadDeferredTextures` is the only path model textures take into the
bindless manager, the deferred pipeline gets none of them either. Only `cacheEmissiveLights`,
which is called separately and touches no GPU object, survives that early return.

{{cite ohao/gpu/vulkan/scene_upload.cpp "if (!m_rtAccel || !m_rtAccel->isSupported()) return;"}}

Two passes build the combined buffers. The first walks the actor map, counts vertices and
indices so the combined vectors reserve exactly once, and collects each visible actor into a
`std::vector`; the second concatenates from that vector, not from the map. Each actor's slice
is recorded as a `MeshBufferInfo` — four `uint32_t`s whose offsets count *elements*, not
bytes — keyed by actor ID.

{{cite ohao/gpu/vulkan/scene_upload.cpp "m_meshBufferMap[actor->getID()] = MeshBufferInfo{"}}

The container behind the walk is a `std::unordered_map<uint64_t, Actor::Ptr>`, so
concatenation order is hash order, not authoring order — the raster draw loop, the shadow
loop and the BLAS builder each re-find their slice by actor ID rather than recomputing
offsets.

:::key
This is a teardown, not an update. Most of the sharp edges below follow from that — the
environment map needs a load cache only because the function is re-entered.
:::

## The rebase that decides both draw calls

As each model's indices are appended they are biased by the running vertex offset, so the
combined index buffer holds global vertex ids rather than mesh-local ones.

{{cite ohao/gpu/vulkan/scene_upload.cpp "combinedIndices.push_back(index + vertexOffset);"}}

That `+ vertexOffset` is why both draw loops in this file issue
`vkCmdDrawIndexed(cmd, indexCount, 1, indexOffset, 0, 0)`: `firstIndex` picks the slice and
the `vertexOffset` argument is zero because the bias is already in the data. Those two draw
calls are the only byte-identical thing about the loops. Everything around them differs —
pipeline, render pass, viewport and scissor, descriptor set, pipeline layout, and the
push-constant payload: the main loop fills all six members of `ObjectPushConstants`, the
shadow loop sets `pc.model` on a zero-initialised struct and pushes the same 240 bytes.

{{cite ohao/gpu/vulkan/scene_upload.cpp "vkCmdPushConstants(cmd, m_pipelineLayout,"}}
{{cite ohao/gpu/vulkan/scene_upload.cpp "vkCmdPushConstants(cmd, m_shadowPipelineLayout,"}}

The ray tracer leans on the same bias from the other side: each BLAS is handed the buffer
*base* with `firstVertex = 0`, which the acceleration-structure unit covers. The coupling is
the point — moving the bias out of upload and into the draw call's `vertexOffset` argument
leaves the rasterizer correct and silently breaks every BLAS.

## Geometry that is its own staging buffer

The raster vertex buffer is created with `TRANSFER_SRC` next to `VERTEX_BUFFER`, and its
memory is requested `HOST_VISIBLE | HOST_COHERENT` with no `DEVICE_LOCAL` bit — filled by a
plain `memcpy` through a temporary mapping, never staged.

{{cite ohao/gpu/vulkan/scene_upload.cpp "bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;"}}

The ray tracer then copies that same buffer into a device-local one carrying the
acceleration-structure build-input, device-address and storage usages.

{{cite ohao/gpu/vulkan/rt_build.cpp "VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |"}}

:::why
The textbook alternative is a throwaway staging buffer, a device-local vertex buffer for
raster and a separate device-local copy for RT — three allocations, two copies. OHAO keeps
the host-visible buffer permanently and lets it double as the RT staging source, so a
re-upload is a `memcpy` rather than a command submission. The price is that the
rasterizer's per-frame vertex fetch reads memory nobody asked to be device-local:
`findMemoryType` returns the first heap satisfying the requested bits, so which physical
memory that is depends on the device's type list, not on intent.
:::

## Two hundred forty bytes the forward shaders do not agree about

The per-draw material path is push-constant only: `renderSceneObjects` reads a
`MaterialComponent`, resolves up to four bindless textures by *string name*, and bit-punts
each `uint32_t` slot index into a float so it can travel in a `vec4`.

{{cite ohao/gpu/vulkan/scene_upload.cpp "pc.materialParams = glm::vec4(metallic, roughness, packIdx(roughMetalTexIdx), packIdx(albedoTexIdx));"}}

`ObjectPushConstants` is three `mat4`s then three `vec4`s — 240 bytes, well above the
128-byte `maxPushConstantsSize` the Vulkan spec guarantees, and the pipeline layout declares
the whole range.

{{cite ohao/gpu/layout_meta.hpp "3 * sizeof(glm::mat4) + 3 * sizeof(glm::vec4); // 240"}}

The shaders that layout serves declare something else. `createPipeline` builds this pipeline
from `core/forward.vert` and `core/forward.frag`, and both open with `mat4 model` followed by
loose scalars — so their `baseColor` lands at byte 64, `metallic` at 76, `roughness` at 80
and `ao` at 84, with the fragment shader's two texture indices at 88 and 92. The CPU writes
`viewProj` across bytes 64–127.

{{cite ohao/gpu/vulkan/pipeline.cpp "// Load shaders (new AAA directory structure: core/forward.vert/frag)"}}
{{cite shaders/core/forward.frag "float albedoTexIdx;   // uint32 packed as float (0xFFFFFFFF = no texture)"}}

The forward fragment shader's albedo, metallic, roughness and both bindless indices are
therefore columns of the view-projection matrix reinterpreted as material data, and the
index guard rejects only the exact bit pattern `0xFFFFFFFF` before indexing the descriptor
array:

{{cite shaders/core/forward.frag "if (albedoIdx != 0xFFFFFFFFu) {"}}

The three `vec4`s the upload code packs at bytes 192–239 sit past the end of every block
those shaders declare. Only this path is affected: the deferred GBuffer pass has its own draw
loop and matching block, and the shadow pass writes only `model`, the one field both sides
agree on.

How live that is cuts both ways. `RenderMode::Forward` is the *default-initialised* value of
`m_renderMode` — what any caller that never calls `setRenderMode` gets, and what a caller is
left holding when `setRenderMode` refuses, which it does for Deferred without a deferred
renderer and for an RT profile without RT support.

{{cite ohao/gpu/vulkan/renderer.hpp "RenderMode m_renderMode{RenderMode::Forward};"}}

Yet nothing in the tree asks for it: the shared example CLI's `rtMode` starts at `RTOffline`
and is only ever assigned an RT profile, `resolveMode` returns that or `Deferred`, and the
smoke test selects `RTOffline`. No in-tree call site passes `RenderMode::Forward`. The
mismatch is reachable by any embedder and currently driven by nobody.

{{cite examples/example_cli.hpp "RenderMode rtMode{RenderMode::RTOffline};"}}

Verified here: both shader declarations, the C++ struct, the pipeline's shader paths, the push
site and every `setRenderMode` call site in the tree. Not verified here: the resulting image.

## Textures are uploaded by writing back into the scene

`uploadDeferredTextures` pushes each model's albedo, normal, rough/metal and emissive images
into the bindless manager, then writes the generated name back into the actor's
`MaterialComponent`.

{{cite ohao/gpu/vulkan/light_upload.cpp "matComp->getMaterial().albedoTexture = texName;"}}

That write-back is what makes the draw loop's string lookup resolvable at all, but it turns
upload into a mutation of the scene rather than a read of it. The assignment sits inside the
per-material loop, so a model with several albedo textures leaves only the last one named —
the raster path is one material per actor, while the RT path keeps per-triangle material ids.
Scalar roughness and metallic collapse the same way, to material 0, but only when the model
carries any `materialColors` at all; metallic falls back to `0.0f` when the per-material
metallic array is empty rather than reading material 0.

{{cite ohao/gpu/vulkan/light_upload.cpp "float metallic = !model->materialMetallic.empty() ? model->materialMetallic[0] : 0.0f;"}}

Nothing deduplicates either: `loadTextureFromMemory` allocates a fresh slot per call and never
releases the previous handle, so each re-entry into `updateSceneBuffers` spends another N
slots out of the manager's 4096.

The rough/metal branch also advertises a repack it does not perform. Its comment describes a
swizzle from Assimp's raw GLTF channel order into the engine's AO/roughness/metallic layout;
the loop copies R→R, G→G, B→B and forces alpha to 255.

{{cite ohao/gpu/vulkan/light_upload.cpp "repacked[p*4+1] = g;   // G = Roughness"}}

An identity copy with an opacity fix. Not wrong — the two orderings coincide — but the
comment is the only thing implying a conversion happens.

## An emissive texture, promoted to a light

An actor whose model declares an emissive texture may have a light synthesised for it, with
the intensity derived by reading the texture on the CPU. Write $R_p, G_p, B_p$ for a texel's
channels in $[0,1]$, $Y(p)$ for Rec. 709 relative luminance, $S$ for the texels bright enough
to count and $P$ for their summed luminance:

$$Y(p) = 0.2126\,R_p + 0.7152\,G_p + 0.0722\,B_p, \qquad S = \{\,p : Y(p) > 0.05\,\}, \qquad P = \sum_{p \in S} Y(p)$$

$$I = \min(0.1\,P,\;20), \qquad c = \frac{1}{|S|}\sum_{p \in S}\big(R_p, G_p, B_p\big)$$

Two gates decide whether the light exists at all, and neither is the per-texel $Y(p) > 0.05$
that defines $S$. The first is $P > 0.1$ on the whole texture: below it the scan runs, costs a
full texture read, and produces nothing.

{{cite ohao/gpu/vulkan/light_upload.cpp "if (totalPower > 0.1f) {"}}

The second is a `break` at the foot of the per-material loop, placed *outside* the power test,
so only the first material carrying a non-negative emissive index is ever examined. A model
whose second material is the real emitter gets nothing.

{{cite ohao/gpu/vulkan/light_upload.cpp "break;  // one light per actor"}}

The colour $c$ is a *mean* over $S$ and is independent of texture resolution. The intensity
$I$ is a *sum* and is not: the same emitter authored at 2K instead of 1K feeds roughly four
times the power into the same formula, and shifts the $P > 0.1$ gate by the same factor. The
clamp at 20 hides the intensity half of that for most textures; nothing hides the gate.

{{cite ohao/gpu/vulkan/light_upload.cpp "float intensity = std::min(totalPower * 0.1f, 20.0f);  // scale power to reasonable intensity"}}

The light's geometry is half in world space and half not: its centre is the model-space
bounding-box midpoint pushed through the actor's world matrix, its radius 0.3 times the
model-space diagonal, never scaled by that matrix.

{{cite ohao/gpu/vulkan/light_upload.cpp "float radius = glm::length(bmax - bmin) * 0.3f;"}}

Scaling an emissive actor moves its light correctly and leaves its size behind. The whole
block exists twice — here as a `GPULight` written into the RT light SSBO, and again in
`cacheEmissiveLights` as a deferred `LightData` point light with a range of ten radii — with
the same two gates, the same clamp and the same mismatch. The second copy is not a GPU
upload: it fills a host-side `std::vector<LightData>` that the per-frame `updateLightBuffer`
appends to the deferred light UBO after the scene's own `LightComponent`s.

## Why the HDRI loads from inside the light path

The environment map has nothing to do with lights except that its bindless index rides in
the light buffer's 16-byte header. The code takes that literally: the HDR decode, the
importance-sampling CDF build and the CDF upload all sit inside the branch that runs only
when at least one light was collected.

{{cite ohao/gpu/vulkan/light_upload.cpp "if (!gpuLights.empty()) {"}}

A scene with no `LightComponent`s and no emissive meshes therefore gets no light buffer, no
environment map, no CDFs, and binding 11 is never handed anything. That shape is not
hypothetical: `env_demo --lighting=none` loads the HDR path and then creates no
`LightComponent` at all, so the HDRI reaches the GPU only if some mesh clears the $P > 0.1$
gate above.

{{cite examples/env_demo.cpp "// LightingMode::None → no lights, HDR env IBL only (pure environment)"}}

The loader is guarded by a path cache, and the comment explaining why is the most useful
line in the file: re-decoding the HDR on every `updateSceneBuffers` leaked whole images until
it crashed. The re-entrant caller that makes that matter is the differentiable-rendering
forward pass: `forwardStudioDeferred` calls `updateSceneBuffers` on every single render.

{{cite ohao/render/diff/diff_vk_forward.hpp "(void)renderer.updateSceneBuffers();"}}

It compounds twice over: one loss evaluation renders `kAvg * nViews` times — 2, with the
fit's `nViews = 2`, `kAvg = 1` — and one finite-difference coordinate probe is two loss
evaluations. Probing a single tile channel is therefore four full teardown-and-rebuild cycles,
and a sweep does that for every channel of every tile.

{{cite ohao/inverse/diff_fit.hpp "const int nViews = 2;"}}
{{cite ohao/inverse/diff_fit.hpp "const double Lp = lossAt(trialP);"}}

The RT-side inverse loops avoid the cost deliberately: `RenderSession` re-uploads materials and
light/env scale in place and reaches `updateSceneBuffers` only when one of those in-place
updates fails.

{{cite ohao/inverse/render_session.hpp "// Material + light/env-scale edits only — never rebuild BLAS / reload HDR."}}
{{cite ohao/gpu/vulkan/light_upload.cpp "float* hdrPixels = stbi_loadf(m_envMapPath.c_str(), &ew, &eh, &ec, 4);"}}

Two details survive that fix. The image is `R32G32B32A32_SFLOAT` — 16 bytes per texel, so a
4096×2048 panorama is 128 MB resident plus an equal host staging buffer — created directly
beneath a comment claiming RGBA16F. And only the image *view* is retained; the `VkImage` and
its `VkDeviceMemory` are function locals nothing destroys, so switching environments at
runtime keeps the previous one alive.

{{cite ohao/gpu/vulkan/light_upload.cpp "imgInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;"}}
{{cite ohao/gpu/vulkan/light_upload.cpp "VkImage envImage = VK_NULL_HANDLE;"}}

## Contracts

- Indices are global: the raster draw must pass `vertexOffset = 0` and the BLAS build `firstVertex = 0`. Rebasing without changing both corrupts one pipeline in silence.
- `MeshBufferInfo` offsets are element counts, and actor order is `unordered_map` order — look slices up by actor ID, never by position.
- `updateSceneBuffers` owns geometry, not lights: `updateLightBuffer` re-reads every `LightComponent` per frame, and `updateRTLightParams` rebuilds the RT SSBO on its own. Editing a light and then calling this function buys nothing but a device stall.
- On an RT-capable device `updateSceneBuffers` is not idempotent: it re-uploads every texture into fresh bindless slots and frees none, so repeated calls drain the 4096-slot table. Without RT support none of that runs — including the bindless upload the deferred pipeline depends on.
- `uploadDeferredTextures` writes texture names, roughness and metallic back into `MaterialComponent`. Code treating the scene as const across an upload is wrong.
- The environment map reaches the GPU only when RT is supported and at least one light was collected. A scene lit purely by an HDRI still needs one `LightComponent` or one emissive mesh past the summed-luminance gate.
- `ObjectPushConstants` and the `core/forward.*` push blocks diverge past the first `mat4`. Only `model` is safe to rely on through those pipeline layouts.
