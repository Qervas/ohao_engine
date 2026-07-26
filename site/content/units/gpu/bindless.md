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

## One layout, two set numbers

The same `VkDescriptorSetLayout` object is consumed at two different set indices.
The forward pipeline puts camera/light UBOs at set 0 and appends the bindless layout
as set 1; the GBuffer pipeline has no UBO set at all — matrices and material scalars
arrive as push constants — so the bindless layout becomes its set 0.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Bind bindless texture descriptor set (set 1) if available"}}
{{cite ohao/render/deferred/gbuffer_pass.cpp "// Bind bindless textures for static pipeline (set 0)"}}

They also differ in *when* they bind. The GBuffer pass binds inside its
pipeline-switch guard, so the set is written once per pipeline change; the forward
path binds it unconditionally once per render pass, before any `vkCmdBindPipeline`.
Neither rebinds per draw.

Vulkan permits one layout at two indices — a set layout carries no index of its own —
but the GLSL does not: `forward.frag` declares `layout(set = 1, binding = 0)` and
`gbuffer.frag` declares `layout(set = 0, binding = 0)` for the same array. Adding a
UBO set to the GBuffer pipeline shifts bindless to set 1 while the shader still names
set 0, which presents a uniform buffer where the SPIR-V wants a combined image
sampler — a descriptor-type mismatch validation rejects at
`vkCreateGraphicsPipelines`. That failure is loud. The silent one is the `< 4096u`
guard in the next section.

{{cite shaders/core/gbuffer.frag "layout(set = 0, binding = 0) uniform sampler2D textures[];"}}

The symmetry stops at the push constant, and only one of the two paths survives it.
Both pipelines push a 240-byte block of three `mat4` then three `vec4` —
`ObjectPushConstants` for forward, the byte-identical `GBufferUBO` for the GBuffer
pass — and `gbuffer.frag` declares exactly that. `forward.frag` declares a different,
96-byte block: `mat4 model`, then `vec3 baseColor` and four scalars, then the two
texture indices.

{{cite ohao/gpu/vulkan/scene_upload.cpp "vkCmdPushConstants(cmd, m_pipelineLayout,"}}
{{cite shaders/core/forward.frag "    float albedoTexIdx;   // uint32 packed as float (0xFFFFFFFF = no texture)"}}

Under std430 push-constant rules `albedoTexIdx` lands at byte 88 and `normalTexIdx` at
92 — both inside the `viewProj` the C++ side wrote across bytes 64–127. `forward.frag`
therefore runs `floatBitsToUint` over a view-projection matrix element and indexes the
array with the result; the test it clears is `!= 0xFFFFFFFFu`, a sentinel compare, not
a bounds check. Everything past `model` is misread the same way, and `forward.vert`
declares a third variant again, with `vec2 padding` where the fragment shader has the
two indices. The bindless plumbing on the forward path is real — layout, binding,
`nonuniformEXT` — but the material data feeding it is not what the renderer pushes.
Only the GBuffer consumer is a working path.

{{cite shaders/core/forward.vert "    vec2 padding;"}}

Startup order is likewise load-bearing: the manager must exist before
`createPipeline()`, because the pipeline layout is built from
`getDescriptorSetLayout()`. If initialisation fails the manager is reset to null and
the pipeline is created *without* the bindless set — the renderer degrades to
untextured rather than crashing.

{{cite ohao/gpu/vulkan/renderer.cpp "// so the forward pipeline layout can include the bindless descriptor set"}}

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
`registerName` aliases many keys onto one handle, which is how the inverse-rendering
ground-plane bind points every tile actor at a single uploaded image. That aliasing is
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
source line. Exactly one caller defends against this by comparing the returned handle
against the known defaults, which is why `BindlessTextureHandle` bothers to default
its `operator<=>`.

{{cite ohao/render/diff/diff_map_bind.hpp "if (!handle.valid() || handle == tm->getDefaultWhiteTexture() ||"}}

## The cost of `updateDescriptorSet`

There is no incremental write path. Each call walks all 4096 slots, builds a
`VkWriteDescriptorSet` for every non-null view, and submits them in one
`vkUpdateDescriptorSets`. Every load path runs it internally before returning, so
uploading N textures issues O(N²) descriptor writes.

Scene load is not the only time that happens. The inverse-rendering forward evaluation
re-uploads the ground-plane albedo map every call — unload the previous image,
`loadTextureFromMemory` the new one, full 4096-slot walk — and it sits one level below
a loop over views and averaging passes, so it runs once per view per pass, several
times per loss evaluation of an optimizer step.

{{cite ohao/render/diff/diff_vk_forward.hpp "(void)bindGroundAlbedoMap(renderer, inv, s_map);"}}
{{cite ohao/render/diff/diff_vk_forward.hpp "auto img = forwardStudioDeferred(renderer, inv, tileRgb, v, frames);"}}

The walk is the cheap half of that. After the first pass the `unloadTexture` in front
of it issues a `vkDeviceWaitIdle` and the upload behind it ends in a
`vkQueueWaitIdle`: two full drains per re-upload.

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

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "vkQueueWaitIdle(m_graphicsQueue);"}}

The staging path also takes the *first* `HOST_VISIBLE` memory type without requiring
`HOST_COHERENT`, and never calls `vkFlushMappedMemoryRanges`. On desktop discrete
GPUs that type is coherent in practice, so it works; it is not portable by spec.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))"}}

## What the path tracer uses instead

The stage flags on the binding are the tell: `FRAGMENT | COMPUTE`, with no ray
tracing stages.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;"}}

This set cannot be bound to the RT pipeline, and it is not. `rt_build.cpp` builds a
separate layered `VK_FORMAT_R8G8B8A8_UNORM` image, creates a single-layer view per
slice, and hands the view array straight to the path tracer's own descriptor at
set 0, binding 12.

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

- The manager must exist before pipeline creation, or the bindless layout is absent from the pipeline layout and every material renders untextured.
- The layout is set **1** in the forward pipeline and set **0** in the GBuffer pipeline; changing either pipeline's set count requires editing the matching `layout(set = …)` in `forward.frag` or `gbuffer.frag`, or pipeline creation fails validation on the descriptor-type mismatch.
- `forward.frag`'s push-constant block is not `ObjectPushConstants`: its `albedoTexIdx` and `normalTexIdx` fall inside the pushed `viewProj`. The shader block must be brought into agreement before any bindless sampling on the forward path means anything.
- `updateDescriptorSet()` never needs an external call — every load path runs it before returning. After an **unload** it is not just unnecessary but ineffective: the loop skips null-view slots, so the freed element still names a destroyed `VkImageView` until a load reuses that slot. An unloaded index must not be sampled.
- `unloadTexture` issues its own `vkDeviceWaitIdle` before destroying the image and view: nothing else in the manager tracks whether an in-flight submission may still sample them. Dropping that stall to speed up the inverse-rendering loop reintroduces a use-after-free.
- The `< 4096u` guards in `gbuffer.frag` hard-code the `maxTextures` default. Raising capacity without editing those two lines silently drops rough-metal and emissive textures above slot 4095.
- Texture identity is the string `<actorName>_<suffix>_<materialIndex>`. Duplicate actor names alias their textures; renaming an actor unbinds them with no diagnostic.
