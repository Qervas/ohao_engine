---
module: path-tracer
id: bindings
title: Descriptor map 0–35
standard: v2
---

## One set is the entire interface

A ray-tracing dispatch has nowhere to hide per-draw state: raygen, closest-hit,
any-hit and miss all execute inside a single `vkCmdTraceRaysKHR` against whatever
was bound before it. OHAO takes that literally. The RT pipeline layout declares
one descriptor set and one push-constant range, so every buffer, image and
acceleration structure any RT stage can reach must occupy a numbered slot in
set 0.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "layoutInfo.setLayoutCount = 1;"}}

`createDescriptorResources` builds that set once at init as a fixed array whose
last entry is 35, and allocates it from a pool holding exactly one set.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "bindings[35].binding"}}
{{cite ohao/render/rt/path_tracer_descriptors.cpp "poolInfo.maxSets = 1;"}}

There is no per-frame duplication of the set. The same `VkDescriptorSet` handle
is rewritten host-side inside `render()` and rebound before every trace — while
the frame manager cycles three command-buffer slots and waits only on the fence
of the slot it is about to reuse, so up to two earlier submissions can still be
referencing that set at the moment it is overwritten.

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

## Stage flags carry information the binding numbers do not

The binding number says where a resource lives; `stageFlags` says who may ask for
it, and that split encodes the traversal architecture rather than decorating it.
Vertex normals, indices, UVs, per-triangle material IDs and material colours
(4, 5, 8, 9, 10) are invisible to raygen — they exist to be unpacked at a hit.
Four of those additionally carry `ANY_HIT`, because alpha cut-out has to resolve
a texel *during* traversal, before a hit is committed, and therefore needs the
index, UV and material tables from inside the any-hit shader:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "bindings[5].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT"}}

The bindless texture array is the mirror image. It is visible to closest-hit,
any-hit and miss — miss samples the equirectangular environment map out of the
same array — and deliberately not to raygen:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "bindings[12].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR"}}

So none of the path tracer's raygen shaders can sample a texture: not one of
`pt_raygen.rgen`, `pt_raygen_offline.rgen` or `pt_raygen_realtime.rgen` declares
a `sampler2D`, and shaded colour has to arrive through the ray payload. That is a
property of *this* layout, not of the engine — the deferred pipeline's RT shadow
and RT GI passes build their own set 0 and read GBuffer samplers straight from
raygen:

{{cite shaders/rt/rt_shadow.rgen "layout(set = 0, binding = 2) uniform sampler2D gBufferPosition;"}}

Most AOV storage images are the exact opposite, raygen-only, because the raygen
owns the pixel coordinate and a hit shader has no idea which pixel invoked it.
Bindings 27 and 28 are the exception, and they are the first warning that the
flags are not a reliable guide to who writes what: no shader in the tree declares
them at all, and the layout file says why they carry `RAYGEN` anyway.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "Raygen stage flag kept for layout parity."}}

## The bindless array and the flag that never moved

Binding 12 is sized for 1024 descriptors regardless of how many textures the
scene actually has.

{{cite ohao/render/rt/path_tracer.hpp "static constexpr uint32_t m_maxBindlessTextures = 1024;"}}

Three descriptor binding flags apply to it. Two still do work; the third is a
leftover.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "| VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT"}}

`PARTIALLY_BOUND` is what makes the write legal: the per-frame update covers only
`m_bindlessTextureCount` descriptors and leaves the rest of the 1024 undefined,
which would otherwise be illegal the moment the set is bound. What makes it
*safe* is the sentinel in the material encoding — index `0xFFFFFFFF` means "no
texture", and every sampler fetch in the closest-hit shader is guarded by that
test, so the undefined tail is never indexed:

{{cite shaders/rt/pt_closesthit.rchit "if (diffuseTexIdx != 0xFFFFFFFFu) {"}}

`UPDATE_AFTER_BIND` makes the *timing* legal. The single set is rewritten every
frame with earlier submissions still in flight, so without the flag the bindless
array would be updated while bound in a pending command buffer. The flag sits on
binding 12 alone; every other binding written in the same `vkUpdateDescriptorSets`
call has no such cover.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "| VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;"}}

The variable-count flag is the leftover. It currently buys nothing — the
allocation asks for exactly `m_maxBindlessTextures` descriptors, the same number
the layout already declares, so nothing is elided. It is also in the wrong place.
Vulkan requires the binding carrying `VARIABLE_DESCRIPTOR_COUNT` to have the
highest binding number in the set; here it sits on 12,

{{cite ohao/render/rt/path_tracer_descriptors.cpp "bindingFlags[12] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT"}}

under a comment still asserting the invariant that once made it legal:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "// Enable bindless: variable count on the LAST binding only"}}

That was true when the layout ended at 12. Bindings 13–35 arrived in at least ten
separate additions: the unlabelled surface/shading history quad at 13–16 and the
env CDF pair at 17–18, then eight that still carry their sub-plan label in the
layout file — 3.A, 3.B, 3.C, 3.C.6, 4.B, 4.C, ReSTIR GI Phase 1, and DLSS-RR at
the end.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "// Binding 35: DLSS-RR specular hit-distance guide"}}

The flag stayed put through all of them. Two fixes exist and neither has been
applied. Deleting `VARIABLE_DESCRIPTOR_COUNT` from `bindingFlags[12]` is free:
the allocation already requests the full 1024, so the binding's descriptor count
is unchanged, no shader line moves, and nothing observable differs. Renumbering
the bindless array above 35 keeps the flag and puts it where Vulkan wants it, at
the cost of editing the `layout(set = 0, binding = ...)` line in every RT shader
that samples a texture. Only the second has an excuse.

## Nothing is allowed to be unbound — almost

Because only binding 12 is partially bound, every other slot must hold a live
handle before the trace, even when the resource is conceptually absent. Geometry
SSBOs alias onto the material buffer when their real buffer does not exist yet, so
the descriptor is valid even though its contents are meaningless:

{{cite ohao/render/rt/path_tracer_render.cpp "normalBufInfo.buffer = m_normalBuffer != VK_NULL_HANDLE"}}

And with no HDR environment loaded, the light-upload path fabricates a four-byte
CDF buffer holding a single `1.0` rather than leave bindings 17 and 18 empty:

{{cite ohao/gpu/vulkan/light_upload.cpp "if (!m_envMarginalCDFBuffer)    createDummyCDF"}}

Binding 11 gets neither treatment, and it is the counter-example to the paragraph
above. Its write is conditional on the light buffer existing:

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_lightBuffer != VK_NULL_HANDLE) {"}}

That buffer is created only once the scene has yielded at least one
`LightComponent`, and there is no dummy fallback. The CDF fabrication above lives
inside that same `if (!gpuLights.empty())` block, so in a light-less scene it does
not run either:

{{cite ohao/gpu/vulkan/light_upload.cpp "if (!gpuLights.empty()) {"}}

All three path-tracer raygens and the miss shader statically read binding 11. In
the raygens the guard that looks like it protects the access —
`if (lightBuf.lightCount > 0u)` — is itself a load from that buffer; the miss
shader reads `lightBuf.envMapTexIdx` with no guard at all:

{{cite shaders/rt/pt_raygen_offline.rgen "layout(set = 0, binding = 11) readonly buffer LightBuffer {"}}

So a scene with no `LightComponent` binds 11, 17 and 18 unwritten, none of them
carrying `PARTIALLY_BOUND` — the same hazard this page reserves for 29–35 below.
This is not hypothetical: `env_demo --lighting=none` builds exactly that scene,
deliberately creating no light actors and leaning on the HDR environment alone.

{{cite examples/env_demo.cpp "// LightingMode::None → no lights, HDR env IBL only (pure environment)"}}

The writes are packed rather than sparse: a running `writeCount` skips the
optional bindings (11, 12, 17, 18), so after binding 11 the write index and the
binding number diverge. The array is sized to exactly the maximum the function can
emit — eleven unconditional writes for bindings 0–10, the four optional ones, and
fourteen more for 13–16 and 19–28:

{{cite ohao/render/rt/path_tracer_render.cpp "VkWriteDescriptorSet writes[29] = {};"}}

Add one more unconditional binding to that function without raising the 29, and
the last write lands past the end of a stack array.

## Where the invariant stops holding: 29–35

Bindings 29–34 are the ReSTIR GI reservoir ping-pong (three RGBA32F planes read,
three written) and 35 is the DLSS-RR specular hit-distance guide. They are in the
layout, their images are created and destroyed alongside the rest, and the
realtime raygen declares and uses them:

{{cite shaders/rt/pt_raygen_realtime.rgen "binding = 29, rgba32f) uniform readonly  image2D prevGIReservoir0"}}

But `PathTracer` issues exactly one descriptor update, and its final write targets
binding 28:

{{cite ohao/render/rt/path_tracer_render.cpp "vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);"}}

Grepping the engine for `m_giReservoirViews` and `m_specHitDistView` turns up
image creation and teardown, plus two `m_specHitDistView` getters with no callers
— and no `VkDescriptorImageInfo` built from any of them. On the realtime profile those seven descriptors
are therefore bound but never written, and unlike binding 12 they carry no
`PARTIALLY_BOUND` flag. This is a structural reading of the code, not an observed
failure: validation layers were not run for this page, and the shader-side ReSTIR
code (temporal and spatial reuse both) is present and compiled in.

## Format qualifiers are load-bearing; the comments beside them are not

For a storage image the format in the GLSL `layout(...)` qualifier is not
documentation — Vulkan requires it to match the format of the bound image view.
Two comments in the layout file get that wrong. Binding 21 is annotated R8 UNORM:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "// Binding 21: roughness AOV (R8 UNORM storage image)"}}

The image is created `R16_SFLOAT`, and all three raygen variants declare it `r16f`:

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.format = VK_FORMAT_R16_SFLOAT;"}}

Binding 26 is annotated RGBA8 UNORM. The image and its view are
`A2B10G10R10_UNORM_PACK32`, and the shader side declares `rgb10_a2`:

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "binding = 26, rgb10_a2"}}

That format is not a preference. The NRD build is forced to
`NRD_NORMAL_ENCODING = 2`, which is its `NRD_NORMAL_ENCODING_R10G10B10A2_UNORM`
mode, so the packed normal-roughness AOV can be handed to REBLUR without a repack:

{{cite external/cmake/nrd.cmake "set(NRD_NORMAL_ENCODING"}}

Shader and image agree in both cases, so nothing renders wrong today; what is
unsafe is trusting these comments when adding binding 36.

## The pool has no slack either

The pool reserves 25 storage images and 9 storage buffers, which is exactly what
the layout consumes: 18 output, AOV and history images, the six reservoir planes,
and the hit-distance guide.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 25},"}}

Adding a storage-image binding without bumping that count makes
`vkAllocateDescriptorSets` return `VK_ERROR_OUT_OF_POOL_MEMORY` and
`createDescriptorResources` return false. That failure is named, not silent —
`init()` prints it to stderr and aborts:

{{cite ohao/render/rt/path_tracer.cpp "[PathTracer] Failed to create descriptor resources"}}

It is total, though. `ensureRTRenderer` drops the half-built profile when `init`
fails, and the RT dispatch then returns immediately because `getRTRenderer` hands
back null, so no trace is ever recorded:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (!rtRenderer || !m_rtAccel) return;"}}

## Two profiles, one layout

The engine ships two RT profiles and no more — the enum has `Realtime` and
`Offline`:

{{cite ohao/render/rt/rt_settings.hpp "enum class RTRenderProfile {"}}

They are two separate `PathTracer` instances of the same class, and they differ in
more than the raygen: bounce count, sampler, firefly clamp, internal denoise,
external denoiser and denoise mode are all set per profile.

{{cite ohao/render/rt/rt_settings.hpp ".samplerType = SamplerType::PCG,"}}

The descriptor code is the one thing they do not differ in: neither
`createDescriptorResources` nor the per-frame write function branches on the
profile. That is what the 36-slot union buys. A third raygen file,
`pt_raygen.rgen`, survives as the struct default in `PathTracerShaderSet`,

{{cite ohao/render/rt/path_tracer.hpp "raygenSpv{"}}

but both profile classes pass an explicit shader set to the base constructor, so
nothing in a shipping path binds it:

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_realtime.rgen.spv"}}

:::why
One layout for both profiles, rather than one per profile. Per-profile layouts
would keep each set minimal and make "declared but never written" impossible to
express — the realtime set would carry 29–35 and the offline set, whose raygen
does not declare them, would not — at the cost of two descriptor code paths, two
pools, and two per-frame write functions to keep in step. The union buys exactly
one property: the host descriptor code never has to know which profile it is
serving, so adding a profile is a subclass plus a raygen, not a third descriptor
path. Everything this page keeps running into — 1024 texture slots reserved
whatever the scene holds, seven unwritten bindings, a variable-count flag stranded
in the middle of the set — is the bill for that property.
:::

:::key
Set 0 is a union, not a contract. A binding appearing in `createDescriptorResources`
proves only that a slot was reserved. Whether anything is written into it lives in
`path_tracer_render.cpp`, and for 29–35 it currently is not.
:::

## Contracts

- The layout's `UPDATE_AFTER_BIND_POOL` flag and the pool's `UPDATE_AFTER_BIND` flag must be set together, but dropping one is not symmetrical with dropping the other: without the pool flag, `vkAllocateDescriptorSets` is the invalid call; without the layout flag, `vkCreateDescriptorSetLayout` already is, because `bindingFlags[12]` carries `UPDATE_AFTER_BIND_BIT`. Only binding 12 uses the capability.
- Every `layout(set = 0, binding = N)` across the four RT stages must match the number *and* the descriptor type in `createDescriptorResources`, and for storage images the format qualifier must match the view format.
- Bindings 4, 5, 8, 9, 10 are hit-stage only, and 12 excludes raygen. Reading them from the wrong stage is a layout mismatch at pipeline creation, not a compile error in the shader.
- Binding 11 is written only if the scene produced at least one light. A scene with no `LightComponent` leaves 11, 17 and 18 bound but unwritten while the raygens and the miss shader statically read them.
- One descriptor set is shared by every frame while the frame manager rotates three slots (`MAX_FRAMES_IN_FLIGHT = 3`), so the `vkUpdateDescriptorSets` in `render()` mutates a set that earlier submissions may still reference. Most writes rebind the same handles each frame; the history ping-pong at 13–16 does not.
