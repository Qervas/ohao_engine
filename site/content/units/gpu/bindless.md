---
module: gpu
id: bindless
title: Bindless textures
standard: v2
---

## The set that never changes shape

A conventional Vulkan renderer allocates one descriptor set per material and
rebinds it before every draw. `BindlessTextureManager` replaces that with a single
descriptor set holding one `COMBINED_IMAGE_SAMPLER` array of 4096 elements, bound
once per pass rather than once per draw. A material stops owning descriptors and
starts owning *indices* — four `uint32_t` array slots stored in its row of the
render world's material table, which the shader reads back and uses to subscript
the array. The manager is the only owner of the images behind them.

Three descriptor-indexing flags make that legal. `VARIABLE_DESCRIPTOR_COUNT` moves
the array size to `vkAllocateDescriptorSets` time; `PARTIALLY_BOUND` makes the 4093
slots that hold nothing at startup legal to leave unwritten, provided no invocation
indexes them; and `UPDATE_AFTER_BIND` allows textures to be written into the array
after pipelines are bound. The third is spelled differently on each of the three
objects it touches: `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` on the binding,
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT` on the layout, and
`VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT` on the pool. The `_POOL_` in the
middle name belongs to the layout flag; there is no such spelling for pools.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;"}}
{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,"}}

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |"}}

The shader side needs one more thing: within a subgroup, neighbouring fragments hit
different materials and therefore different array elements, so the index is not
dynamically uniform and every access must be wrapped in `nonuniformEXT`.

{{cite ohao/gpu/vulkan/device_setup.cpp "features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;"}}

:::why
A layered `VkImage` (a *texture array*) is the other plausible answer: every layer
shares an extent and format, so sources are resampled to fit, and a 1×1 fallback costs
as much as the largest map. This manager uses a *descriptor array*: separate images of
arbitrary size, format and mip count, unified only by an index. That costs a Vulkan
1.2 feature set and non-uniform indexing, and buys keeping a 4096² albedo and a 1×1
default in one lookup without rescaling either. The path tracer takes the same
indices: one table, one numbering, read by both pipelines.
:::

## One raster consumer, at set 0

The manager's set has exactly one raster consumer: the G-buffer pass. Its pipeline
layout has two sets — the bindless table at index 0 and, at index 1, the "scene" set
holding RenderScene's instance, material and material-reference buffers, the frame
slot's camera UBO, the SSS tile buffer and the draw table. The pass binds both in each
of its two render passes, when the frame has draws:

{{cite ohao/render/deferred/gbuffer_pass.cpp "// Set 0: bindless textures; set 1: the scene."}}

The sets are bound once per render pass, before its two indirect draws (one-sided and
double-sided lists) — every submesh is a draw of the shared vertex and index arenas,
and the sets do not change between them.

Vulkan lets a set layout sit at any index — it carries no index of its own — but the
GLSL does not: `gbuffer.frag` names `layout(set = 0, binding = 0)` for the textures
and `set = 1` for the scene buffers. Swapping the two in the pipeline layout presents
a storage buffer where the SPIR-V wants a combined image sampler — a descriptor-type
mismatch validation rejects at `vkCreateGraphicsPipelines`. That failure is loud. The
silent one is the `< 4096u` guard in the next section.

{{cite shaders/includes/scene/gbuffer_body.glsl "layout(set = 0, binding = 0) uniform sampler2D textures[];"}}

No push constant carries the indices, and none carries the material either: the draws
are indirect, and a command's `firstInstance` is the draw's index in `RenderScene`'s
draw table. The vertex stage reads that record for the instance slot and the submesh's
material slot and hands the slot on as a flat varying; the fragment shader resolves it
through the instance's reference list into a row of the material buffer, and the four
texture indices come out of that row.

{{cite shaders/core/gbuffer.vert "fragMaterialSlot = d.info.x;"}}

Startup order is load-bearing: the manager must be handed to `DeferredRenderer`
before the G-buffer pass builds its pipeline, because the layout at index 0 is the
manager's only if a manager is present at that moment. If the manager's own
initialisation fails, `VulkanRenderer` resets it to null and carries on, and the pass
puts an empty stand-in layout at index 0 so the scene set can still sit at index 1 —
while `gbuffer.frag` still declares and samples the set-0 array. That is not a
graceful untextured mode; it is a pipeline layout whose set 0 does not cover its
shader, which validation reports at pipeline creation.

{{cite ohao/render/deferred/gbuffer_pass.cpp "m_emptyLayout != VK_NULL_HANDLE ? m_emptyLayout : m_textureManager->getDescriptorSetLayout(),"}}

## Two sentinels, one of them fragile

`UINT32_MAX` is the "no texture" value. The material table stores each index as a
float bit pattern (`std::bit_cast`), and the shader recovers it with
`floatBitsToUint`. Albedo and normal are guarded against exactly that sentinel.

{{cite shaders/includes/scene/gbuffer_body.glsl "albedo *= texture(textures[nonuniformEXT(albedoTexIdx)], fragTexCoord).rgb;"}}

Roughness/metallic and emissive are not. They use a range test instead:

{{cite shaders/includes/scene/gbuffer_body.glsl "if (roughMetalTexIdx < 4096u) {"}}

That literal `4096` is the `maxTextures` argument the renderer happens to
pass. Raising the array capacity to, say, 8192 would leave every roughness-metallic
and emissive texture in slots ≥ 4096 silently ignored — no validation error, just
materials that quietly fall back to their scalar parameters.

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

## Indices in a table, names as a side door

Nothing stores a texture name on a material. The model upload loads each of a
material's maps with `loadTextureFromMemory` and writes the returned slot straight
into the `MaterialDesc` field for that kind of map:

{{cite ohao/render/scene/model_upload.cpp "mats[mi].*field = h.index;"}}

and the material table packs those fields, bit-cast, into the row both pipelines read:

{{cite ohao/render/scene/material_table.cpp "d.baseColor.r, d.baseColor.g, d.baseColor.b, bits(d.albedoTex),"}}

A string survives in one place, as a key for the manager's own lookup tables: the
environment map is registered as `ohao_env_map`.

{{cite ohao/gpu/vulkan/env_upload.cpp "registerExternalTexture(envView, "ohao_env_map", BindlessTextureType::Custom);"}}

The manager can also alias a key onto an existing handle with `registerName`, which
nothing in the engine calls. The alias is still why unloading cannot just erase by
`tex.name` — it must scan both maps for every key still pointing at the freed slot.

{{cite ohao/gpu/vulkan/bindless_texture_manager.hpp "void registerName(BindlessTextureHandle handle, std::string_view name) {"}}
{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "if (it->second.index == handle.index) it = m_nameToHandle.erase(it);"}}

Nothing deduplicates the model textures themselves: the upload walks materials, not
the model's texture pool, so two materials pointing at the same pooled image upload it
twice into two slots.

## A valid handle is not a successful upload

Every failure path in `loadTextureFromMemory` — image creation failed, free list
exhausted — returns `getDefaultTexture(type)`, and those defaults are real, valid
handles. The model upload checks only that:

{{cite ohao/render/scene/model_upload.cpp "if (h.valid()) {"}}

so a failed upload stores the 1×1 default's slot into the material and renders a
surface that looks like a deliberate material.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "case BindlessTextureType::Metallic:"}}

Black is the roughness/metallic default, and `gbuffer.frag` multiplies the sampled
channels into the scalar parameters, so a silently-defaulted rough-metal map drives
`roughness` and `metallic` to zero. The G-buffer stores the roughness as authored;
OpenPBR's module holds it at its floor (`kMinSpecularRoughness`, 0.01) when it shades:

{{cite shaders/includes/scene/gbuffer_body.glsl "roughness *= rm.g;  // G channel = Roughness"}}
{{cite shaders/material/openpbr.slang "static const float kMinSpecularRoughness = 0.01;"}}

The result is a *mirror* dielectric where the artist painted a rough one — the zeroed
roughness is the visible artifact. **Nothing in the tree defends against it.** The defence is available —
`BindlessTextureHandle` defaults its `operator<=>`, so a caller can compare a
returned handle against `getDefaultWhiteTexture()` and refuse it. No caller does.

{{cite ohao/gpu/vulkan/bindless_texture_manager.hpp "constexpr auto operator<=>(const BindlessTextureHandle&) const noexcept = default;"}}

## The cost of `updateDescriptorSet`

There is no incremental write path. Each call walks all 4096 slots, builds a
`VkWriteDescriptorSet` for every non-null view, and submits them in one
`vkUpdateDescriptorSets`. Every load path runs it internally before returning, so
uploading N textures issues O(N²) descriptor writes.

Scene load is not the only time that happens. Any caller that swaps a texture
mid-run pays it — unload the previous image, `loadTextureFromMemory` the new one,
full 4096-slot walk — and the walk is the cheap half: the `unloadTexture` in front
of it issues a `vkDeviceWaitIdle` and the upload behind it ends in a
`vkQueueWaitIdle`, two full drains per re-upload.

Nothing in the tree does this in a hot loop. Treat the O(N²) walk plus two drains
as the price of any design that swaps textures per frame rather than per scene — a
per-view texture swap inside an evaluation loop, for instance, pays it once per
view.

The walk also has a blind spot on the unload side. `unloadTexture` destroys the view
and then zeroes the slot; the update loop skips every slot whose view is null. So
`updateDescriptorSet()` cannot clear that array element after an unload — it keeps
naming a destroyed `VkImageView` until a load reuses the slot, which the LIFO free
list makes the very next allocation. Nothing here un-writes a bindless element; only
overwriting one fixes it.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "tex = BindlessTextureInfo{};  // Reset"}}
{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "if (m_textures[i].view == VK_NULL_HANDLE) continue;"}}

One line in the walk is load-bearing and looks like it is not:

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "imageInfos.reserve(m_textures.size());"}}

Each write stores `&imageInfos.back()`, a pointer into a vector still being appended
to. The reserve is sized to the maximum possible entry count, so no reallocation can
occur and every stored pointer survives until `vkUpdateDescriptorSets` reads it.
Delete it as a cleanup and every `pImageInfo` becomes dangling.

## Memory, and the allocator that is never used

`initialize` takes a `GpuAllocator*`, stores it, and never reads it again; every
production call site passes `nullptr` anyway.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "m_allocator = allocator;"}}

So each texture performs its own `vkAllocateMemory` for the image, and each upload
ends in a full `vkQueueWaitIdle` — immediately after which the staging buffer and its
separate allocation are destroyed, in the same function. Staging never survives the
call, so the durable count is one memory object per texture. Vulkan caps live
allocations at `maxMemoryAllocationCount`, so filling a 4096-slot array is a
memory-object problem before it is a VRAM problem.

There are two such paths, and they are near-duplicates of each other: the initial
upload and the in-place update carry their own copies of the same staging, submit,
wait and teardown sequence. Both end in `vkQueueWaitIdle(m_graphicsQueue)` — the
lines are textually identical, which is why a change to one is easy to forget in
the other.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "bool BindlessTextureManager::createTextureImage(std::span<const uint8_t> data, uint32_t width, uint32_t height,"}}
{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "bool BindlessTextureManager::updateTextureFromMemory(BindlessTextureHandle handle,"}}

Both copies also take the *first* `HOST_VISIBLE` memory type without requiring
`HOST_COHERENT`, and neither calls `vkFlushMappedMemoryRanges`. On desktop discrete
GPUs that type is coherent in practice, so it works; it is not portable by spec.

## The path tracer reads the same table through its own set

The stage flags on the manager's binding are the tell: `FRAGMENT | COMPUTE`, with no
ray tracing stages.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;"}}

This set cannot be bound to the RT pipeline, and it is not. Instead each RT frame the
renderer copies the table's *views* into a parallel array: slot `i` of the array is
bindless index `i`, up to the highest slot in use, with holes filled by the default
white texture so the array is dense. The environment map's slot gets its own sampler;
every other slot gets the manager's default one.

{{cite ohao/gpu/vulkan/env_upload.cpp "const uint32_t count = m_textureManager->usedSlotEnd();"}}
{{cite ohao/gpu/vulkan/env_upload.cpp "renderer.setBindlessTextures(m_rtTextureViews, m_rtTextureSamplers);"}}

The path tracer writes those into binding 12 of its own descriptor set. The binding
is sized to the manager's capacity — `ensureRTRenderer` passes `getMaxTextures()` —
and is a partially bound, update-after-bind array visible to the raygen as well as the
hit and miss stages (the path tracer's raygen samples the environment itself), with
every element below the capacity written (entries past the table's views
repeat view 0) so no index reads an unwritten descriptor.

{{cite ohao/gpu/vulkan/renderer.cpp "slot->setBindlessCapacity(m_textureManager ? m_textureManager->getMaxTextures()"}}
{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RGEN | HIT | MISS, m_bindlessCapacity,"}}
{{cite shaders/rt/path_tracer.slang "[[vk::binding(12, 0)]] Sampler2D textures[4096];"}}

Both pipelines mark the index non-uniform (`nonuniformEXT` in the G-buffer's GLSL,
`NonUniformResourceIndex` in the path tracer's Slang) and treat `0xFFFFFFFF` as no
texture, and — unlike a separate layered image — they share one slot numbering: an
index meaning "brick albedo" in the G-buffer means the same image in the path tracer's
closest hit, which treats any index at or past 4096 as none. What differs is the
descriptor, not the identity. Two sets name the same images, and a texture upload
reaches the path tracer only when the next RT frame rebuilds the array.

## Two entry points, one caller

`loadTexture(path, …)` — the `stb_image` entry point that decodes from disk and picks
`R8G8B8A8_SRGB` for albedo/emissive and `UNORM` for everything else — has no callers.
Model loaders decode textures themselves, so the shipping path is
`loadTextureFromMemory`, where the caller supplies the format. The *policy* survives
at the call sites (SRGB for albedo and emissive, UNORM for normal and rough-metal);
only the function encoding it is unreachable. `loadTexture` is also the sole entry
point that deduplicates by key, which is why two models sharing an image upload the
same pixels twice.

`registerExternalTexture` has exactly one caller: the environment-map upload, which
creates the HDR image itself and hands the manager only its view. The function nulls
`image` and `memory` so cleanup skips them, but `cleanup()` destroys every non-null
view unconditionally — so after registration the *manager* owns the view and the
renderer owns the image and its memory.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "tex.image = VK_NULL_HANDLE;  // External, don't destroy"}}
{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "        if (tex.view != VK_NULL_HANDLE) {"}}

The renderer's teardown follows that split: it unloads the slot, which destroys the
view, and then frees the image and memory itself.

{{cite ohao/gpu/vulkan/env_upload.cpp "// The bindless table owns the view once registered: unloading it destroys the view."}}

Registering a view the caller also destroys would double-free it; the contract is that
a registered view is surrendered.

:::key
The index in a material is a slot in this manager's free list, not an identity. It is
reused after `unloadTexture`, shared by the G-buffer and the path tracer, and handed
out even when the upload failed. Treat a non-`UINT32_MAX` index as "something is
bound here", never as "the texture I asked for is bound here".
:::

## Contracts

- The manager must be set on `DeferredRenderer` before the G-buffer pass initialises, or set 0 of the G-buffer pipeline layout is an empty stand-in while `gbuffer.frag` still samples it.
- The bindless layout is set **0** and the scene set is set **1** in the G-buffer pipeline; reordering them requires editing the `layout(set = …)` lines in `gbuffer.frag`, or pipeline creation fails validation on the descriptor-type mismatch.
- The pass has no per-draw push constant: the draw record gives the material slot, and texture indices come from the material row. A per-draw texture lane belongs in `DrawRecord` (`shaders/shared/layout.json`), which the vertex shader reads through `firstInstance`.
- `updateDescriptorSet()` never needs an external call — every load path runs it before returning. After an **unload** it is not just unnecessary but ineffective: the loop skips null-view slots, so the freed element still names a destroyed `VkImageView` until a load reuses that slot. An unloaded index must not be sampled.
- `unloadTexture` issues its own `vkDeviceWaitIdle` before destroying the image and view: nothing else in the manager tracks whether an in-flight submission may still sample them. Dropping that stall to speed up a texture-swapping loop reintroduces a use-after-free.
- The `< 4096u` guards in `gbuffer.frag` hard-code the `maxTextures` argument. Raising capacity without editing those two lines silently drops rough-metal and emissive textures above slot 4095.
- The path tracer's binding 12 is rebuilt from the table every RT frame and sized to `getMaxTextures()`; a view registered with `registerExternalTexture` belongs to the manager from that moment.
