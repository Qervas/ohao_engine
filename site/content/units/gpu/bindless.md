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
starts owning a *name* — a `std::string` per map — which the draw loop resolves to a
`uint32_t` array index and packs into a push constant. Nothing durable holds the
index; it is rebuilt every frame.

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
The engine ships both plausible answers, which makes the trade-off unusually easy to
read. The path tracer uses a *texture array*: one layered `VkImage`, so every layer
shares an extent and format and sources are resampled to fit. This manager uses a
*descriptor array*: separate images of arbitrary size, format and mip count, unified
only by an index. That costs a Vulkan 1.2 feature set and non-uniform indexing, and
buys keeping a 4096² albedo and a 1×1 default in one lookup without rescaling either.
:::

## One consumer, at set 0

The manager's set has exactly one raster consumer: the G-buffer pass. Its pipeline
has no UBO set at all — matrices and material scalars arrive as push constants — so
the bindless layout is its only set, at index 0.

{{cite ohao/render/deferred/gbuffer_pass.cpp "// Bind bindless textures for static pipeline (set 0)"}}

The bind sits inside the pipeline-switch guard of the draw loop, so the set is bound
once per pipeline change, not once per draw. With a single static pipeline that is
once per pass.

Vulkan lets a set layout sit at any index — it carries no index of its own — but the
GLSL does not: `gbuffer.frag` names `layout(set = 0, binding = 0)`. Adding a UBO set
in front of the bindless one shifts bindless to set 1 while the shader still names
set 0, which presents a uniform buffer where the SPIR-V wants a combined image
sampler — a descriptor-type mismatch validation rejects at
`vkCreateGraphicsPipelines`. That failure is loud. The silent one is the `< 4096u`
guard in the next section.

{{cite shaders/core/gbuffer.frag "layout(set = 0, binding = 0) uniform sampler2D textures[];"}}

The push constant is the other half of the contract, and it agrees on both sides: the
pass pushes a 240-byte `GBufferUBO` of three `mat4` then three `vec4`, and
`gbuffer.frag` declares the same block field for field. The texture indices travel in
the `w` lanes of `materialParams` and `albedoColor` and the `x` lane of
`emissiveParams`, as `uint32_t` bit patterns packed into floats.

{{cite ohao/render/deferred/gbuffer_pass.hpp "};  // 240 bytes — fits in 256-byte push constant limit"}}

Startup order is load-bearing: the manager must be handed to `DeferredRenderer`
before the G-buffer pass builds its pipeline, because the pipeline layout includes the
bindless set only if a manager is present at that moment. If the manager's own
initialisation fails, `VulkanRenderer` resets it to null and carries on, and the
G-buffer pipeline layout is built with zero sets — while `gbuffer.frag` still
declares and samples the set-0 array. That is not a graceful untextured mode; it is a
pipeline layout that does not cover its shader, which validation reports at pipeline
creation.

{{cite ohao/render/deferred/gbuffer_pass.cpp "pipelineLayoutInfo.setLayoutCount = 0;"}}

## Two sentinels, one of them fragile

`UINT32_MAX` is the "no texture" value. C++ packs it into a float via `memcpy` for
the push constant; the shader recovers it with `floatBitsToUint`. Albedo and normal
are guarded against exactly that sentinel.

{{cite shaders/core/gbuffer.frag "if (albedoTexIdx != 0xFFFFFFFFu) {"}}

Roughness/metallic and emissive are not. They use a range test instead:

{{cite shaders/core/gbuffer.frag "if (roughMetalTexIdx < 4096u) {"}}

That literal `4096` is the `maxTextures` default argument the renderer happens to
pass. Raising the array capacity to, say, 8192 would leave every roughness-metallic
and emissive texture in slots ≥ 4096 silently ignored — no validation error, just
materials that quietly fall back to their scalar parameters.

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

## Strings as the binding key

Nothing stores a handle on the material. What the material component holds per map is
a `std::string`:

{{cite ohao/gpu/vulkan/material.hpp "std::string albedoTexture;                 // Base color/diffuse texture"}}

The upload path names each texture `<actorName>_albedo_<materialIndex>` and writes
that string into the material component; the draw loop rebuilds the identical string,
hashes it back to a slot, and packs the index into that draw's push constant. The
`uint32_t` exists only between those two statements.

{{cite ohao/gpu/vulkan/light_upload.cpp "matComp->getMaterial().albedoTexture = texName;"}}
{{cite ohao/render/deferred/gbuffer_pass.cpp "const auto handle = textureManager->findTexture(textureName);"}}
{{cite ohao/render/deferred/gbuffer_pass.cpp "ubo.materialParams = glm::vec4(metallic, roughness, packIdx(roughMetalTexIdx), packIdx(albedoTexIdx));"}}

Two actors with the same name therefore share textures, and renaming an actor
detaches its textures with no error. The indirection buys something real:
`registerName` aliases many keys onto one handle, which is how `light_upload.cpp`
points several material slots at a single uploaded image. That aliasing is
also why unloading cannot just erase by `tex.name` — it must scan both maps for every
key still pointing at the freed slot.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "if (it->second.index == handle.index) it = m_nameToHandle.erase(it);"}}

## A valid handle is not a successful upload

Every failure path in `loadTextureFromMemory` — image creation failed, free list
exhausted — returns `getDefaultTexture(type)`, and those defaults are real, valid
handles. A caller checking only `handle.valid()` registers the 1×1 default under the
actor's texture name and renders a surface that looks like a deliberate material.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "case BindlessTextureType::Metallic:"}}

Black is the roughness/metallic default, and `gbuffer.frag` multiplies the sampled
channels into the scalar parameters, so a silently-defaulted rough-metal map drives
`ao`, `roughness` and `metallic` to zero on three consecutive lines. The roughness
floor two lines further down clamps that back to `0.04`:

{{cite shaders/core/gbuffer.frag "roughness *= rm.g;  // G channel = Roughness"}}
{{cite shaders/core/gbuffer.frag "roughness = max(roughness, 0.04);"}}

The result is not a fully occluded dielectric but a fully occluded *near-mirror* — the
zeroed roughness is the more visible artifact of the two, and it is on the adjacent
source line. **Nothing in the tree defends against it.** The defence is available —
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

## What the path tracer uses instead

The stage flags on the binding are the tell: `FRAGMENT | COMPUTE`, with no ray
tracing stages.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;"}}

This set cannot be bound to the RT pipeline, and it is not. `rt_build.cpp` builds a
separate layered `VK_FORMAT_R8G8B8A8_UNORM` image, creates a single-layer view per
slice, and hands the view array straight to the path tracer's own descriptor at
set 0, binding 12. The renderer owns those per-layer views in `m_rtBindlessViews` and
destroys them with the rest of the RT scene resources, on re-upload and at shutdown.

{{cite ohao/gpu/vulkan/rt_build.cpp "renderer.setBindlessTextures(bindlessViews, bindlessSamplers);"}}
{{cite shaders/rt/pt_closesthit.rchit "layout(set = 0, binding = 12) uniform sampler2D textures[];"}}

Both use `nonuniformEXT` and the same `0xFFFFFFFF` sentinel, which makes them read as
one system in the shader source. They are two, with independent slot numbering: an
index meaning "brick albedo" in the GBuffer means something unrelated in
closest-hit.

## Two entry points nothing calls

`loadTexture(path, …)` — the `stb_image` entry point that decodes from disk and picks
`R8G8B8A8_SRGB` for albedo/emissive and `UNORM` for everything else — has no callers.
Model loaders decode textures themselves, so the shipping path is
`loadTextureFromMemory`, where the caller supplies the format. The *policy* survives
at the call sites (SRGB for albedo and emissive, UNORM for normal and rough-metal);
only the function encoding it is unreachable. `loadTexture` is also the sole entry
point that deduplicates by key, which is why two actors sharing a model upload the
same pixels twice.

`registerExternalTexture` has no callers either, and carries a latent bug worth
knowing before someone wires it up: it nulls `image` and `memory` so cleanup skips
them, but `cleanup()` destroys every non-null view unconditionally, including one it
does not own.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "tex.image = VK_NULL_HANDLE;  // External, don't destroy"}}
{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "        if (tex.view != VK_NULL_HANDLE) {"}}

:::key
The index in a material is a slot in this manager's free list, not an identity. It is
reused after `unloadTexture`, unrelated to the path tracer's layer numbering, and
handed out even when the upload failed. Treat a non-`UINT32_MAX` index as "something
is bound here", never as "the texture I asked for is bound here".
:::

## Contracts

- The manager must be set on `DeferredRenderer` before the G-buffer pass initialises, or the bindless layout is absent from the G-buffer pipeline layout while `gbuffer.frag` still samples it.
- The layout is set **0** in the G-buffer pipeline; adding a set in front of it requires editing `layout(set = …)` in `gbuffer.frag`, or pipeline creation fails validation on the descriptor-type mismatch.
- `GBufferUBO` and `gbuffer.frag`'s push-constant block must stay field-for-field identical; the texture indices are read from fixed lanes of the last three `vec4`s.
- `updateDescriptorSet()` never needs an external call — every load path runs it before returning. After an **unload** it is not just unnecessary but ineffective: the loop skips null-view slots, so the freed element still names a destroyed `VkImageView` until a load reuses that slot. An unloaded index must not be sampled.
- `unloadTexture` issues its own `vkDeviceWaitIdle` before destroying the image and view: nothing else in the manager tracks whether an in-flight submission may still sample them. Dropping that stall to speed up a texture-swapping loop reintroduces a use-after-free.
- The `< 4096u` guards in `gbuffer.frag` hard-code the `maxTextures` default. Raising capacity without editing those two lines silently drops rough-metal and emissive textures above slot 4095.
- Texture identity is the string `<actorName>_<suffix>_<materialIndex>`. Duplicate actor names alias their textures; renaming an actor unbinds them with no diagnostic.
