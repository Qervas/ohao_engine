---
module: path-tracer
id: bindings
title: Descriptor map 0–41
standard: v2
---

## One set is the entire interface

A ray-tracing dispatch has nowhere to hide per-draw state: raygen, closest-hit,
any-hit and miss all execute inside a single `vkCmdTraceRaysKHR` against whatever
was bound before it. OHAO takes that literally. The RT pipeline layout declares
one descriptor set and one push-constant range, so every buffer, image and
acceleration structure any RT stage can reach must occupy a numbered slot in
set 0. The set's numbers run from 0 to 41 with gaps: the path tracer's own images
and buffers at 0–13 and 17–26 (13 the camera ray's emission, which NRD's compositor
adds back), the two `RenderScene` tables the hit shaders add at 36 and 37, OpenPBR's
energy tables at 38, and the radiance cache's three buffers at 39–41. Bindings 14–16
and 27–35 are not in the layout.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(13, IMAGE, RGEN);   // the camera ray's emission (RGBA16F): nrd_compose adds it back"}}

{{cite ohao/render/rt/path_tracer_pipeline.cpp "layoutInfo.setLayoutCount = 1;"}}

`createDescriptorResources` builds the layout once at init, collecting bindings
and their flags in two parallel vectors, and ends the list with the energy
tables and the radiance cache, all read by the raygens only:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(38, BUFFER, RGEN);  // OpenPBR's energy tables (rt/path_tracer.slang)"}}
{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(39, BUFFER, RGEN);  // the radiance cache: keys,"}}
{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(41, BUFFER, RGEN);  // resolved radiance (render/rt/radiance_cache.hpp)"}}

The cache's buffers (its keys, the frame's sums and the resolved radiance) belong to
`render::RadianceCache`; the radiance cache unit describes them. They are bound in
every set, in both profiles: the offline profile's cache holds one entry and its
trace never reads it.

That buffer is a `render::OpenPBRTablesBuffer`, created once at init and written from the
generated `render::kOpenPBRTables` (the Kulla-Conty and dielectric albedo tables
`material/openpbr.slang` looks up); it never changes:

{{cite ohao/render/rt/path_tracer.cpp "if (!m_openpbrTables.create(m_device, m_physicalDevice)) {"}}

{{cite ohao/render/material/openpbr_tables_buffer.cpp "std::memcpy(mapped, kOpenPBRTables, bytes);"}}

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

The bindless texture array is visible to closest-hit, any-hit, miss and raygen.
The hit shaders sample material textures from it, and the raygen
(`rt/path_tracer.slang`) samples the environment itself, both for a missed ray
and for an environment light sample, because the miss shader only reports the
miss:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RGEN | HIT | MISS, m_bindlessCapacity,"}}
{{cite shaders/rt/path_tracer.slang "return textures[NonUniformResourceIndex(tex)].SampleLevel(uv, 0.0).rgb * envScale();"}}

Surface colour still arrives through the ray payload: the raygen samples no
material texture. That is a property of *this* layout, not of the engine — the
deferred pipeline's RT passes have no raygen at all. RT GI and RT shadows are compute passes of inline ray
queries, each with its own set 0, and they read the GBuffer straight from the
thread that launches the rays:

{{cite shaders/rt/gi_trace.slang "[[vk::binding(1, 0)]] Texture2D<float4> gPosition;"}}

The light buffer (11) and the environment CDF pair (17, 18) are declared for raygen
and miss. The raygen draws light samples and environment samples from them and
weighs a missed ray by the environment's density itself; the miss shader reads none
of them, so their `MISS` flag grants an access no shader takes. The flags say who
*may* read a binding, not who does.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(11, BUFFER, RGEN | MISS);"}}

The storage images are raygen-only, because the raygen owns the pixel coordinate
and a hit shader has no idea which pixel invoked it.

## The bindless array and the flag that never moved

Binding 12 is sized to the texture manager's capacity, not to however many
textures the scene happens to use. The renderer hands the path tracer the same
number the G-buffer's table was created with, so a material's bindless slot
indexes the array directly:

{{cite ohao/gpu/vulkan/renderer.cpp "slot->setBindlessCapacity(m_textureManager ? m_textureManager->getMaxTextures()"}}

The path tracer then clamps that against what the device allows for an
update-after-bind combined-image-sampler array in one set and one stage, leaving
room for the set's other resources:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "constexpr uint32_t kOtherResources = 64;   // the set's 29 other bindings, with margin"}}

The shader does not learn the clamped size. It declares the array at a fixed 4096
(Slang decorates an unsized resource array with a stride spirv-val rejects) and
treats an index at or past 4096 as "no texture":

{{cite shaders/rt/path_tracer.slang "[[vk::binding(12, 0)]] Sampler2D textures[4096];"}}

Two descriptor binding flags apply to binding 12.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);"}}

`PARTIALLY_BOUND` is what makes an unwritten table legal. The per-frame update
writes the *whole* capacity once any texture exists — entries past the table's
live views repeat view 0 — so the tail is defined whenever the table is non-empty.
Before the first texture is registered the write is skipped and the binding is
left entirely unwritten, which only the flag allows. What makes the sentinel safe
is the material encoding: index `0xFFFFFFFF` means "no texture", and every sampler
fetch in the hit shaders is guarded by a range test that excludes it:

{{cite shaders/rt/path_tracer.slang "if (albedoTex < 4096u) baseColor *= sampleTex(albedoTex, uv).rgb;"}}

`UPDATE_AFTER_BIND` is the flag that puts the array under the device's
update-after-bind limits, which are the ones the capacity clamp reads, and it
requires the matching layout and pool flags below. It sits on binding 12 alone;
every other binding written in the same `vkUpdateDescriptorSets` call has no such
cover, which is safe only because each frame slot's set is not in use when it is
rewritten.

A third flag, `VARIABLE_DESCRIPTOR_COUNT`, is deliberately absent: Vulkan allows it
only on a set's highest-numbered binding, binding 12 is not that, and every set is
allocated at exactly `m_bindlessCapacity` anyway:

{{cite ohao/render/rt/path_tracer_descriptors.cpp "(VUID 03004), and every set is allocated at the full capacity anyway."}}

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
`count = 0`. The raygen statically reads binding 11: the light count and the
environment's texture index are its first two header words, read with no guard,
which is why the header's env slot holds `0xFFFFFFFF` ("none") rather than
uninitialised memory:

{{cite shaders/rt/path_tracer.slang "uint lightCount() { return asuint(lightData[0].x); }"}}

This is exercised, not hypothetical: `env_demo --lighting=none` builds a scene
with no light entities and leans on the HDR environment alone.

{{cite examples/env_demo.cpp "// LightingMode::None → no lights, HDR env IBL only (pure environment)"}}

The writes are packed rather than sparse: a running `writeCount` skips the
optional bindings (12 when the table is empty, 17 and 18 without CDF buffers), so
the write index and the binding number diverge. The function emits at most
thirty writes — seven unconditional ones for bindings 0, 1, 2, 3, 6, 7 and 13, twelve
buffers (4, 5, 8, 9, 10, 11, 36, 37, the energy tables at 38 and the cache's 39–41)
in a loop, the three optional ones, and eight for 19–26 — into a 41-entry array:

{{cite ohao/render/rt/path_tracer_render.cpp "VkWriteDescriptorSet writes[41] = {};"}}

Nothing checks `writeCount` against the array size; the eleven spare entries are
the margin.

## Format qualifiers are load-bearing

For a storage image the format in the shader's `[[vk::image_format(...)]]`
attribute is not documentation — Vulkan requires it to match the format of the bound image view.
The layout file no longer annotates the formats beside the image bindings, so the
authority is the image creation code and the shaders, and they have to agree
with each other. Binding 21 is the roughness AOV; the image is created
`R16_SFLOAT`, and the raygen declares it so (`r16f`):

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.format = VK_FORMAT_R16_SFLOAT;"}}

Binding 26 is the packed normal and roughness. The image and its view are
`A2B10G10R10_UNORM_PACK32`, and the shader side declares `rgb10_a2`:

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;"}}
{{cite shaders/rt/path_tracer.slang "RWTexture2D<float4> normalRoughnessAOV;"}}

That format is not a preference. The NRD build is forced to
`NRD_NORMAL_ENCODING = 2`, which is its `NRD_NORMAL_ENCODING_R10G10B10A2_UNORM`
mode, so the packed normal-roughness AOV can be handed to REBLUR without a repack:

{{cite external/cmake/nrd.cmake "set(NRD_NORMAL_ENCODING"}}

Shader and image agree in both cases, so nothing renders wrong today; what is
unsafe is adding binding 42 and trusting a qualifier copied from a neighbour.

## The pool has no slack either

The pool reserves, per frame-slot set, 14 storage images and 14 storage buffers,
which is exactly what the layout consumes: the accumulation pair, the output, the
emission guide and ten AOV images; and the seven scene tables, the light buffer, the two CDFs, the
energy tables and the radiance cache's three buffers.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14 * kSets},"}}

{{cite ohao/render/rt/path_tracer_descriptors.cpp "{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 14 * kSets},  // 1-3, 6, 7, 13, 19-26"}}

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

They are two separate `PathTracer` instances of the same class running the same
shaders, `PathTracerShaderSet`'s default, the Slang path tracer with its four named
entry points:

{{cite ohao/render/rt/path_tracer.hpp "rt_path_tracer.slang.raygen.spv"}}

What differs is settings: bounce count, firefly clamp, denoise mode (and with it
whether the trace keeps its own running mean) and the radiance cache, whose capacity is
2^21 entries for the realtime profile and one entry otherwise. The descriptor code does
not branch on any of them: neither `createDescriptorResources` nor the per-frame
write function knows which profile it serves, and every binding is written in every
set whichever denoiser runs.

:::key
Set 0 is a reservation, not a usage report. A binding appearing in
`createDescriptorResources` proves only that a slot was reserved and who may read it;
whether anything is written into it lives in `path_tracer_render.cpp`, and whether
any shader reads it lives in `rt/path_tracer.slang`.
:::

## Contracts

- The layout's `UPDATE_AFTER_BIND_POOL` flag and the pool's `UPDATE_AFTER_BIND` flag must be set together, but dropping one is not symmetrical with dropping the other: without the pool flag, `vkAllocateDescriptorSets` is the invalid call; without the layout flag, `vkCreateDescriptorSetLayout` already is, because binding 12 carries `UPDATE_AFTER_BIND_BIT`. Only binding 12 uses the capability.
- Every `[[vk::binding(N, 0)]]` in `rt/path_tracer.slang` must match the number *and* the descriptor type in `createDescriptorResources`, and for storage images the format attribute must match the view format.
- Bindings 4, 5, 8, 9, 10, 36 and 37 are hit-stage only (closest-hit and any-hit), and 38–41 are raygen only. Reading them from the wrong stage is a layout mismatch at pipeline creation, not a compile error in the shader.
- The shader declares binding 12 at a fixed 4096 and tests indices against 4096, while the layout's count is the texture table's capacity clamped to the device's limits. A material index the table hands out must stay below both.
- Binding 11 is always backed: `LightTable` owns the buffer and writes a header with `count = 0` when there are no lights. Bindings 17 and 18 are backed by one-float dummies when there is no environment map.
- There is one descriptor set per frame in flight (`MAX_FRAMES_IN_FLIGHT = 3`). `render()` writes only the set of the slot it is given, after that slot's fence wait, so the writes never touch a set a pending submission reads. Every binding except 12 must be written in every set.
