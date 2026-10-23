---
module: path-tracer
id: bindings
title: Descriptor map 0–37
standard: v2
---

## One set is the entire interface

A ray-tracing dispatch has nowhere to hide per-draw state: raygen, closest-hit,
any-hit and miss all execute inside a single `vkCmdTraceRaysKHR` against whatever
was bound before it. OHAO takes that literally. The RT pipeline layout declares
one descriptor set and one push-constant range, so every buffer, image and
acceleration structure any RT stage can reach must occupy a numbered slot in
set 0. The set runs from binding 0 to binding 37: the path tracer's own images
and history at 0–35, and the two `RenderScene` tables the hit shaders add at 36
and 37.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "layoutInfo.setLayoutCount = 1;"}}

`createDescriptorResources` builds the layout once at init, collecting bindings
and their flags in two parallel vectors, and ends the list with the instance
tables:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(37, BUFFER, HIT);   // instance material refs"}}

The set is duplicated per frame in flight. The pool holds `MAX_FRAMES_IN_FLIGHT`
sets, and `render()` takes the one for the frame slot it was given — a slot whose
fence the frame manager has already waited on, so no earlier submission is still
reading the set when it is rewritten.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "poolInfo.maxSets = kSets;"}}
{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

The three sets are written independently; nothing is shared between slots but the
buffers and images they point at.

## Stage flags carry information the binding numbers do not

The binding number says where a resource lives; `stageFlags` says who may ask for
it, and that split encodes the traversal architecture rather than decorating it.
The scene tables — vertex arena, index arena, mesh table, triangle material slots,
material table (4, 5, 8, 9, 10) and the instance table and its material refs (36,
37) — are invisible to raygen; they exist to be unpacked at a hit. All of them
carry `ANY_HIT` as well as `CLOSEST_HIT`, because alpha cut-out has to resolve a
texel *during* traversal, before a hit is committed, and therefore needs the same
instance, mesh, slot and material chain from inside the any-hit shader:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "constexpr VkShaderStageFlags HIT = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;"}}

The bindless texture array is visible to closest-hit, any-hit and miss — miss
samples the equirectangular environment map out of the same array — and
deliberately not to raygen:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, HIT | MISS, m_bindlessCapacity,"}}

So none of the path tracer's raygen shaders can sample a texture: not one of
`pt_raygen.rgen`, `pt_raygen_offline.rgen` or `pt_raygen_realtime.rgen` declares
a `sampler2D`, and shaded colour has to arrive through the ray payload. That is a
property of *this* layout, not of the engine — the deferred pipeline's RT shadow
and RT GI passes build their own set 0 and read GBuffer samplers straight from
raygen:

{{cite shaders/rt/rt_shadow.rgen "layout(set = 0, binding = 2) uniform sampler2D gBufferPosition;"}}

The light buffer (11) and the environment CDF pair (17, 18) are the buffers raygen
and miss share: raygen draws light samples and env samples from them, and miss
reads the env map index and the CDFs to weight a miss by the environment's pdf.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(11, BUFFER, RGEN | MISS);"}}

Most AOV storage images are the opposite, raygen-only, because the raygen owns the
pixel coordinate and a hit shader has no idea which pixel invoked it. Bindings 27
and 28 are the exception, and they are the first warning that the flags are not a
reliable guide to who writes what: no shader in the tree declares them at all, and
the layout file says why they carry `RAYGEN` anyway.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "(raygen flag for layout parity)"}}

## The bindless array and the flag that never moved

Binding 12 is sized to the texture manager's capacity, not to however many
textures the scene happens to use. The renderer hands the path tracer the same
number the G-buffer's table was created with, so a material's bindless slot
indexes the array directly:

{{cite ohao/gpu/vulkan/renderer.cpp "slot->setBindlessCapacity(m_textureManager ? m_textureManager->getMaxTextures()"}}

The path tracer then clamps that against what the device allows for an
update-after-bind combined-image-sampler array in one set and one stage, leaving
room for the set's other resources. The shaders learn the final size through a
specialization constant, so an index at or past it reads as "no texture":

{{cite shaders/rt/pt_closesthit.rchit "layout(constant_id = 1) const uint OHAO_BINDLESS_COUNT = 4096u;"}}

Three descriptor binding flags apply to binding 12. Two still do work; the third
is a leftover.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |"}}

`PARTIALLY_BOUND` is what makes an unwritten table legal. The per-frame update
writes the *whole* capacity once any texture exists — entries past the table's
live views repeat view 0 — so the tail is defined whenever the table is non-empty.
Before the first texture is registered the write is skipped and the binding is
left entirely unwritten, which only the flag allows. What makes the sentinel safe
is the material encoding: index `0xFFFFFFFF` means "no texture", and every sampler
fetch in the closest-hit shader is guarded by that test:

{{cite shaders/rt/pt_closesthit.rchit "if (diffuseTexIdx != 0xFFFFFFFFu) {"}}

`UPDATE_AFTER_BIND` is the flag that puts the array under the device's
update-after-bind limits, which are the ones the capacity clamp reads, and it
requires the matching layout and pool flags below. It sits on binding 12 alone;
every other binding written in the same `vkUpdateDescriptorSets` call has no such
cover, which is safe only because each frame slot's set is not in use when it is
rewritten.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);"}}

The variable-count flag is the leftover. It currently buys nothing — the
allocation asks for exactly `m_bindlessCapacity` descriptors in every set, the same
number the layout already declares, so nothing is elided:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "variableCounts.fill(m_bindlessCapacity);"}}

It is also in the wrong place. Vulkan requires the binding carrying
`VARIABLE_DESCRIPTOR_COUNT` to have the highest binding number in the set; here it
sits on 12, and the set now runs to 37. Bindings 13–35 arrived in at least ten
separate additions: the unlabelled surface/shading history quad at 13–16 and the
env CDF pair at 17–18, then eight that still carry their sub-plan label in the
layout file — 3.A, 3.B, 3.C, 3.C.6, 4.B, 4.C, ReSTIR GI Phase 1, and DLSS-RR at
the end:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(35, IMAGE, RGEN);   // DLSS-RR specular hit-distance guide (R32F)"}}

The `RenderScene` tables took 36 and 37 after that, and the flag stayed put
through all of them. Two fixes exist and neither has been applied. Deleting
`VARIABLE_DESCRIPTOR_COUNT` from binding 12's flags is free: the allocation
already requests the full capacity, so the binding's descriptor count is
unchanged, no shader line moves, and nothing observable differs. Renumbering the
bindless array above 37 keeps the flag and puts it where Vulkan wants it, at the
cost of editing the `layout(set = 0, binding = ...)` line in every RT shader that
samples a texture. Only the second has an excuse.

## Nothing is allowed to be unbound — almost

Because only binding 12 is partially bound, every other slot must hold a live
handle before the trace, even when the resource is conceptually absent. The
scene tables are required outright: `render()` returns without tracing if any of
the TLAS, the vertex and index arenas, the mesh table, triangle slots, material
table, instance table, material refs or the light buffer is null.

{{cite ohao/render/rt/path_tracer_render.cpp "!m_instances || !m_materialRefs || !m_lightBuffer) return;"}}

With no HDR environment loaded, the environment upload fabricates a four-byte CDF
buffer holding a single `1.0` rather than leave bindings 17 and 18 empty:

{{cite ohao/gpu/vulkan/env_upload.cpp "if (!m_envMarginalCDFBuffer)    createDummyCDF"}}

Binding 11 needs no dummy, because the light buffer is never absent. `LightTable`
allocates it at initialisation, sized for the header plus the maximum light count,
and writes the header on the first frame:

{{cite ohao/render/scene/light_table.cpp "m_dirty = true;   // the first frame writes the header"}}

A scene with no lights therefore binds a valid buffer whose header says
`count = 0`. All three path-tracer raygens and the miss shader statically read
binding 11; in the raygens the guard `if (lightBuf.lightCount > 0u)` is itself a
load from that buffer, and the miss shader reads `lightBuf.envMapTexIdx` with no
guard at all, which is why the header's env slot holds `0xFFFFFFFF` ("none") rather
than uninitialised memory:

{{cite shaders/rt/pt_raygen_offline.rgen "layout(set = 0, binding = 11) readonly buffer LightBuffer {"}}

This is exercised, not hypothetical: `env_demo --lighting=none` builds a scene
with no light entities and leans on the HDR environment alone.

{{cite examples/env_demo.cpp "// LightingMode::None → no lights, HDR env IBL only (pure environment)"}}

The writes are packed rather than sparse: a running `writeCount` skips the
optional bindings (12 when the table is empty, 17 and 18 without CDF buffers), so
the write index and the binding number diverge. The array is sized three entries
above the maximum the function can emit — five unconditional writes for bindings
0, 1, 2, 6 and 7, the eight `RenderScene` buffers (4, 5, 8, 9, 10, 11, 36, 37) in
a loop, the three optional ones, fourteen more for 13–16 and 19–28, and seven for
29–35 — thirty-seven writes in a 40-entry array:

{{cite ohao/render/rt/path_tracer_render.cpp "VkWriteDescriptorSet writes[40] = {};"}}

Add four more unconditional bindings to that function without raising the 40, and
the last write lands past the end of a stack array. Nothing checks `writeCount`
against the array size.

## 29–35: one profile's bindings, written for both

Bindings 29–34 are the ReSTIR GI reservoir ping-pong (three RGBA32F planes read,
three written) and 35 is the DLSS-RR specular hit-distance guide. Only the
realtime raygen declares them:

{{cite shaders/rt/pt_raygen_realtime.rgen "binding = 29, rgba32f) uniform readonly  image2D prevGIReservoir0"}}

They are written in every frame-slot set all the same, at the tail of the one
descriptor update, with the *prev* half of the ping-pong at 29–31 and the *curr*
half at 32–34:

{{cite ohao/render/rt/path_tracer_render.cpp "for (uint32_t i = 0; i < 6; ++i) addImage(29u + i, &giReservoirInfo[i]);"}}

These seven were the last holes in the invariant, and the hole was not
theoretical. Until they were written, the realtime profile bound seven
statically-used storage images that no `VkWriteDescriptorSet` ever filled, with no
`PARTIALLY_BOUND` cover, and `rt_realtime` lost the device; the comment above the
writes records it. Writing them for the offline profile too costs seven descriptor
writes per frame and keeps the write function profile-blind.

## Format qualifiers are load-bearing

For a storage image the format in the GLSL `layout(...)` qualifier is not
documentation — Vulkan requires it to match the format of the bound image view.
The layout file no longer annotates the formats beside the image bindings, so the
authority is the image creation code and the shaders, and they have to agree
with each other. Binding 21 is the roughness AOV; the image is created
`R16_SFLOAT`, and all three raygen variants declare it `r16f`:

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.format = VK_FORMAT_R16_SFLOAT;"}}

Binding 26 is the packed normal and roughness. The image and its view are
`A2B10G10R10_UNORM_PACK32`, and the shader side declares `rgb10_a2`:

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "binding = 26, rgb10_a2"}}

That format is not a preference. The NRD build is forced to
`NRD_NORMAL_ENCODING = 2`, which is its `NRD_NORMAL_ENCODING_R10G10B10A2_UNORM`
mode, so the packed normal-roughness AOV can be handed to REBLUR without a repack:

{{cite external/cmake/nrd.cmake "set(NRD_NORMAL_ENCODING"}}

Shader and image agree in both cases, so nothing renders wrong today; what is
unsafe is adding binding 38 and trusting a qualifier copied from a neighbour.

## The pool has no slack either

The pool reserves, per frame-slot set, 25 storage images and 10 storage buffers,
which is exactly what the layout consumes: 18 output, AOV and history images, the
six reservoir planes and the hit-distance guide; and the seven scene tables, the
light buffer and the two CDFs.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 25 * kSets},"}}

Adding a storage-image or storage-buffer binding without bumping that count makes
`vkAllocateDescriptorSets` return `VK_ERROR_OUT_OF_POOL_MEMORY` and
`createDescriptorResources` return false. That failure is named, not silent —
`init()` logs it as an error on the `render` channel and aborts:

{{cite ohao/render/rt/path_tracer.cpp "PathTracer: failed to create descriptor resources"}}

It is total, though. `ensureRTRenderer` drops the half-built profile when `init`
fails, and the RT dispatch then returns immediately because `getRTRenderer` hands
back null, so no trace is ever recorded:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (!rtRenderer || !isRTSupported()) return;"}}

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
profile. That is what the 0–37 union buys. A third raygen file,
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
path. Everything this page keeps running into — a texture array sized to the whole
table whatever the scene holds, seven realtime-only bindings written every frame for
a profile that never reads them, a variable-count flag stranded in the middle of
the set — is the bill for that property.
:::

:::key
Set 0 is a union, not a contract. A binding appearing in `createDescriptorResources`
proves only that a slot was reserved. Whether anything is written into it lives in
`path_tracer_render.cpp`, and 29–35 sat declared but unwritten until a lost device
forced the writes in.
:::

## Contracts

- The layout's `UPDATE_AFTER_BIND_POOL` flag and the pool's `UPDATE_AFTER_BIND` flag must be set together, but dropping one is not symmetrical with dropping the other: without the pool flag, `vkAllocateDescriptorSets` is the invalid call; without the layout flag, `vkCreateDescriptorSetLayout` already is, because binding 12 carries `UPDATE_AFTER_BIND_BIT`. Only binding 12 uses the capability.
- Every `layout(set = 0, binding = N)` across the four RT stages must match the number *and* the descriptor type in `createDescriptorResources`, and for storage images the format qualifier must match the view format.
- Bindings 4, 5, 8, 9, 10, 36 and 37 are hit-stage only (closest-hit and any-hit), and 12 excludes raygen. Reading them from the wrong stage is a layout mismatch at pipeline creation, not a compile error in the shader.
- Binding 12's size is a specialization constant on miss, closest-hit and any-hit as well as a layout count. The two must come from the same `bindlessCapacity()`; a shader built with a larger constant than the layout would index past the array.
- Binding 11 is always backed: `LightTable` owns the buffer and writes a header with `count = 0` when there are no lights. Bindings 17 and 18 are backed by one-float dummies when there is no environment map.
- There is one descriptor set per frame in flight (`MAX_FRAMES_IN_FLIGHT = 3`). `render()` writes only the set of the slot it is given, after that slot's fence wait, so the writes never touch a set a pending submission reads. Every binding except 12 must be written in every set, the realtime-only 29–35 included.
